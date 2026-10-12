package store

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib" // the "pgx" driver for database/sql
)

// PostgreSQL keeps the same state as the JSON file, one table per kind of
// thing. The store still decides everything in memory and under its lock; the
// database is where the result is written, one transaction per change, and
// where it is read from once, at the start. So there is exactly one server
// process per database - an advisory lock makes a second one refuse to start.

// The schema grows by appending to this list; what a database already has is
// noted in schema_version. Never change an entry that has shipped.
var migrations = [][]string{{
	`CREATE TABLE server (
		only_row   boolean PRIMARY KEY DEFAULT true CHECK (only_row),
		server_key bytea  NOT NULL,
		next_cmd   bigint NOT NULL)`,
	`CREATE TABLE tokens (
		id      text PRIMARY KEY,
		secret  bytea NOT NULL,
		note    text  NOT NULL,
		expires timestamptz NOT NULL)`,
	// What a device says about itself (info, status) and what it answers
	// (result) is kept as it came: json, not jsonb, which would refuse some of
	// what a device may send.
	`CREATE TABLE devices (
		id           text PRIMARY KEY,
		pub          bytea NOT NULL,
		name         text  NOT NULL,
		info         json,
		state        text  NOT NULL,
		sas          text  NOT NULL,
		sas_attempts integer NOT NULL,
		enrolled     timestamptz NOT NULL,
		last_seen    timestamptz,
		status       json)`,
	`CREATE TABLE commands (
		id      bigint PRIMARY KEY,
		device  text NOT NULL REFERENCES devices ON DELETE CASCADE,
		type    text NOT NULL,
		args    json,
		state   text NOT NULL,
		created timestamptz NOT NULL,
		sent    timestamptz,
		done    timestamptz,
		result  json)`,
	`CREATE INDEX commands_device ON commands (device, id)`,
	`CREATE TABLE consoles (
		id        bigint PRIMARY KEY,
		device    text NOT NULL REFERENCES devices ON DELETE CASCADE,
		port      integer NOT NULL,
		started   timestamptz NOT NULL,
		ended     timestamptz,
		reason    text   NOT NULL,
		bytes_in  bigint NOT NULL,
		bytes_out bigint NOT NULL)`,
	`CREATE INDEX consoles_device ON consoles (device, id)`,
	`CREATE TABLE users (
		name          text PRIMARY KEY,
		kind          text NOT NULL,
		disabled      boolean NOT NULL,
		pass_salt     bytea,
		pass_hash     bytea,
		pass_iter     integer NOT NULL,
		totp          bytea,
		totp_ok       boolean NOT NULL,
		totp_step     bigint  NOT NULL,
		setup_hash    bytea,
		setup_expires timestamptz,
		entra_name    text NOT NULL,
		entra_oid     text NOT NULL,
		created       timestamptz NOT NULL,
		last_login    timestamptz)`,
	`CREATE TABLE audit (
		seq    bigserial PRIMARY KEY,
		time   timestamptz NOT NULL,
		who    text NOT NULL,
		ip     text NOT NULL,
		action text NOT NULL,
		target text NOT NULL,
		detail text NOT NULL,
		ok     boolean NOT NULL)`,
	`CREATE INDEX audit_time ON audit (time)`,
}}

const (
	pgLock     = 0x72733233 // "rs23": the advisory lock of the one server process
	pgTimeout  = 15 * time.Second
	maxAuditDB = 200000 // the database keeps more of the audit log than the server shows
)

type pg struct {
	db     *sql.DB
	lock   *sql.Conn // holds the advisory lock for as long as it lives
	audits int
}

// OpenPostgres keeps the state in the PostgreSQL database at url
// ("postgres://user:password@host:5432/db?sslmode=..."). If the database is
// new and importDir holds the state.json of a server that ran on the file, that
// state is taken over and the file is renamed.
func OpenPostgres(url, importDir string) (*Store, error) {
	p, err := openPG(url)
	if err != nil {
		return nil, err
	}
	var first *data
	path := filepath.Join(importDir, stateFile)
	if importDir != "" {
		if _, err := os.Stat(path); err == nil {
			if first, err = readFile(path); err != nil {
				p.close()
				return nil, fmt.Errorf("%s: %w", path, err)
			}
		}
	}
	s, err := open(p, first)
	if err != nil {
		return nil, err
	}
	if s.tookFirst {
		// It holds the server key: do not leave a second copy that looks current.
		if err := os.Rename(path, path+".imported"); err != nil {
			s.Close()
			return nil, err
		}
		s.imported = path + ".imported"
	}
	return s, nil
}

// Imported names the file a state was taken over from by this start, if any.
func (s *Store) Imported() string { return s.imported }

func openPG(url string) (*pg, error) {
	db, err := sql.Open("pgx", url)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(4)
	db.SetConnMaxIdleTime(5 * time.Minute)
	p := &pg{db: db}
	// The database may still be starting when the server does.
	deadline := time.Now().Add(time.Minute)
	for {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		err = db.PingContext(ctx)
		cancel()
		if err == nil || time.Now().After(deadline) {
			break
		}
		time.Sleep(time.Second)
	}
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("database: %w", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), pgTimeout)
	defer cancel()
	if p.lock, err = db.Conn(ctx); err != nil {
		db.Close()
		return nil, err
	}
	var got bool
	// one lock per schema: tests share a database
	if err = p.lock.QueryRowContext(ctx, `SELECT pg_try_advisory_lock($1, hashtext(current_schema()))`, pgLock).Scan(&got); err == nil && !got {
		err = errors.New("another server process is using this database")
	}
	if err == nil {
		err = p.migrate(ctx)
	}
	if err != nil {
		p.close()
		return nil, err
	}
	return p, nil
}

func (p *pg) migrate(ctx context.Context) error {
	if _, err := p.db.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_version (version integer NOT NULL)`); err != nil {
		return err
	}
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var have int
	if err := tx.QueryRowContext(ctx, `SELECT coalesce(max(version), 0) FROM schema_version`).Scan(&have); err != nil {
		return err
	}
	if have > len(migrations) {
		return fmt.Errorf("the database has schema %d, this server knows %d: it is older than the database", have, len(migrations))
	}
	for v := have; v < len(migrations); v++ {
		for _, q := range migrations[v] {
			if _, err := tx.ExecContext(ctx, q); err != nil {
				return fmt.Errorf("schema %d: %w", v+1, err)
			}
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO schema_version VALUES ($1)`, v+1); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func (p *pg) close() error {
	if p.lock != nil {
		p.lock.Close()
	}
	return p.db.Close()
}

// ---------------------------------------------------------------- values

// txt makes a string storable: PostgreSQL takes neither a zero byte nor
// anything that is not UTF-8, and names and reasons come from devices and
// from whoever types into the login form.
func txt(s string) string {
	return strings.ReplaceAll(strings.ToValidUTF8(s, "�"), "\x00", "")
}

// js hands JSON over as text; nothing is NULL.
func js(raw json.RawMessage) any {
	if len(raw) == 0 {
		return nil
	}
	return strings.ToValidUTF8(string(raw), "�")
}

func raw(b []byte) json.RawMessage {
	if len(b) == 0 {
		return nil
	}
	return json.RawMessage(b)
}

func tp(t sql.NullTime) *time.Time {
	if !t.Valid {
		return nil
	}
	u := t.Time.UTC()
	return &u
}

// ---------------------------------------------------------------- load

func (p *pg) load() (*data, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 4*pgTimeout)
	defer cancel()
	// one snapshot for all tables
	tx, err := p.db.BeginTx(ctx, &sql.TxOptions{Isolation: sql.LevelRepeatableRead, ReadOnly: true})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	d := &data{Tokens: map[string]*Token{}, Devices: map[string]*Device{}, Users: map[string]*User{}}

	err = tx.QueryRowContext(ctx, `SELECT server_key, next_cmd FROM server`).Scan(&d.ServerKey, &d.NextCmd)
	if errors.Is(err, sql.ErrNoRows) {
		return d, nil // a new database
	} else if err != nil {
		return nil, err
	}

	each := func(query string, row func(*sql.Rows) error) error {
		rows, err := tx.QueryContext(ctx, query)
		if err != nil {
			return err
		}
		defer rows.Close()
		for rows.Next() {
			if err := row(rows); err != nil {
				return err
			}
		}
		return rows.Err()
	}

	err = each(`SELECT id, secret, note, expires FROM tokens`, func(r *sql.Rows) error {
		t := &Token{}
		if err := r.Scan(&t.ID, &t.Secret, &t.Note, &t.Expires); err != nil {
			return err
		}
		t.Expires = t.Expires.UTC()
		d.Tokens[t.ID] = t
		return nil
	})
	if err != nil {
		return nil, err
	}

	err = each(`SELECT id, pub, name, info, state, sas, sas_attempts, enrolled, last_seen, status FROM devices`, func(r *sql.Rows) error {
		v := &Device{}
		var info, status []byte
		var seen sql.NullTime
		if err := r.Scan(&v.ID, &v.Pub, &v.Name, &info, &v.State, &v.SAS, &v.SASAttempts, &v.Enrolled, &seen, &status); err != nil {
			return err
		}
		v.Info, v.Status, v.Enrolled, v.LastSeen = raw(info), raw(status), v.Enrolled.UTC(), tp(seen)
		d.Devices[v.ID] = v
		return nil
	})
	if err != nil {
		return nil, err
	}

	err = each(`SELECT id, device, type, args, state, created, sent, done, result FROM commands ORDER BY id`, func(r *sql.Rows) error {
		c := &Command{}
		var dev string
		var args, result []byte
		var sent, done sql.NullTime
		if err := r.Scan(&c.ID, &dev, &c.Type, &args, &c.State, &c.Created, &sent, &done, &result); err != nil {
			return err
		}
		c.Args, c.Result, c.Created, c.Sent, c.Done = raw(args), raw(result), c.Created.UTC(), tp(sent), tp(done)
		if v := d.Devices[dev]; v != nil {
			v.Commands = append(v.Commands, c)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}

	err = each(`SELECT id, device, port, started, ended, reason, bytes_in, bytes_out FROM consoles ORDER BY id`, func(r *sql.Rows) error {
		l := &ConsoleLog{}
		var dev string
		var ended sql.NullTime
		if err := r.Scan(&l.ID, &dev, &l.Port, &l.Start, &ended, &l.Reason, &l.In, &l.Out); err != nil {
			return err
		}
		l.Start, l.End = l.Start.UTC(), tp(ended)
		if v := d.Devices[dev]; v != nil {
			v.Consoles = append(v.Consoles, l)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}

	err = each(`SELECT name, kind, disabled, pass_salt, pass_hash, pass_iter, totp, totp_ok, totp_step,
	                   setup_hash, setup_expires, entra_name, entra_oid, created, last_login FROM users`, func(r *sql.Rows) error {
		u := &User{}
		var setup, login sql.NullTime
		if err := r.Scan(&u.Name, &u.Kind, &u.Disabled, &u.PassSalt, &u.PassHash, &u.PassIter, &u.TOTP, &u.TOTPOk, &u.TOTPStep,
			&u.SetupHash, &setup, &u.EntraName, &u.EntraOID, &u.Created, &login); err != nil {
			return err
		}
		u.SetupExpires, u.Created, u.LastLogin = tp(setup), u.Created.UTC(), tp(login)
		d.Users[u.Name] = u
		return nil
	})
	if err != nil {
		return nil, err
	}

	// the server works with the newest part of the audit log
	err = each(fmt.Sprintf(`SELECT time, who, ip, action, target, detail, ok FROM
	                        (SELECT * FROM audit ORDER BY seq DESC LIMIT %d) newest ORDER BY seq`, maxAudit), func(r *sql.Rows) error {
		e := &AuditEntry{}
		if err := r.Scan(&e.Time, &e.Who, &e.IP, &e.Action, &e.Target, &e.Detail, &e.OK); err != nil {
			return err
		}
		e.Time = e.Time.UTC()
		d.Audit = append(d.Audit, e)
		return nil
	})
	if err != nil {
		return nil, err
	}
	return d, nil
}

// ---------------------------------------------------------------- save

// writer runs the statements of one transaction and remembers the first error.
type writer struct {
	ctx context.Context
	tx  *sql.Tx
	err error
}

func (w *writer) exec(query string, args ...any) {
	if w.err == nil {
		_, w.err = w.tx.ExecContext(w.ctx, query, args...)
	}
}

func (p *pg) save(d *data, ch *changes) error {
	if ch.empty() {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), pgTimeout)
	defer cancel()
	tx, err := p.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	w := &writer{ctx: ctx, tx: tx}

	if ch.meta {
		w.exec(`INSERT INTO server (server_key, next_cmd) VALUES ($1, $2)
		        ON CONFLICT (only_row) DO UPDATE SET server_key = excluded.server_key, next_cmd = excluded.next_cmd`,
			d.ServerKey, int64(d.NextCmd))
	}
	for id := range ch.tokens {
		t := d.Tokens[id]
		if t == nil {
			w.exec(`DELETE FROM tokens WHERE id = $1`, id)
			continue
		}
		w.exec(`INSERT INTO tokens (id, secret, note, expires) VALUES ($1, $2, $3, $4)
		        ON CONFLICT (id) DO UPDATE SET secret = excluded.secret, note = excluded.note, expires = excluded.expires`,
			t.ID, t.Secret, txt(t.Note), t.Expires)
	}
	for id, dc := range ch.devices {
		v := d.Devices[id]
		if v == nil {
			w.exec(`DELETE FROM devices WHERE id = $1`, id) // takes its commands and consoles along
			continue
		}
		// The row first, also when only a command changed: on an import, and
		// after a save that failed, it may not be there yet.
		w.exec(`INSERT INTO devices (id, pub, name, info, state, sas, sas_attempts, enrolled, last_seen, status)
		        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
		        ON CONFLICT (id) DO UPDATE SET pub = excluded.pub, name = excluded.name, info = excluded.info,
		          state = excluded.state, sas = excluded.sas, sas_attempts = excluded.sas_attempts,
		          enrolled = excluded.enrolled, last_seen = excluded.last_seen, status = excluded.status`,
			v.ID, v.Pub, txt(v.Name), js(v.Info), v.State, v.SAS, v.SASAttempts, v.Enrolled, v.LastSeen, js(v.Status))
		if len(dc.cmds) > 0 {
			for _, c := range v.Commands {
				if !dc.cmds[c.ID] {
					continue
				}
				w.exec(`INSERT INTO commands (id, device, type, args, state, created, sent, done, result)
				        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
				        ON CONFLICT (id) DO UPDATE SET state = excluded.state, sent = excluded.sent,
				          done = excluded.done, result = excluded.result`,
					int64(c.ID), v.ID, c.Type, js(c.Args), c.State, c.Created, c.Sent, c.Done, js(c.Result))
			}
			// the history is bounded: what the store dropped from the front goes here too
			if len(v.Commands) > 0 {
				w.exec(`DELETE FROM commands WHERE device = $1 AND id < $2`, v.ID, int64(v.Commands[0].ID))
			}
		}
		if len(dc.cons) > 0 {
			for _, l := range v.Consoles {
				if !dc.cons[l.ID] {
					continue
				}
				w.exec(`INSERT INTO consoles (id, device, port, started, ended, reason, bytes_in, bytes_out)
				        VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
				        ON CONFLICT (id) DO UPDATE SET ended = excluded.ended, reason = excluded.reason,
				          bytes_in = excluded.bytes_in, bytes_out = excluded.bytes_out`,
					int64(l.ID), v.ID, l.Port, l.Start, l.End, txt(l.Reason), l.In, l.Out)
			}
			if len(v.Consoles) > 0 {
				w.exec(`DELETE FROM consoles WHERE device = $1 AND id < $2`, v.ID, int64(v.Consoles[0].ID))
			}
		}
	}
	for name := range ch.users {
		u := d.Users[name]
		if u == nil {
			w.exec(`DELETE FROM users WHERE name = $1`, name)
			continue
		}
		w.exec(`INSERT INTO users (name, kind, disabled, pass_salt, pass_hash, pass_iter, totp, totp_ok, totp_step,
		                           setup_hash, setup_expires, entra_name, entra_oid, created, last_login)
		        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)
		        ON CONFLICT (name) DO UPDATE SET kind = excluded.kind, disabled = excluded.disabled,
		          pass_salt = excluded.pass_salt, pass_hash = excluded.pass_hash, pass_iter = excluded.pass_iter,
		          totp = excluded.totp, totp_ok = excluded.totp_ok, totp_step = excluded.totp_step,
		          setup_hash = excluded.setup_hash, setup_expires = excluded.setup_expires,
		          entra_name = excluded.entra_name, entra_oid = excluded.entra_oid, last_login = excluded.last_login`,
			u.Name, u.Kind, u.Disabled, u.PassSalt, u.PassHash, u.PassIter, u.TOTP, u.TOTPOk, u.TOTPStep,
			u.SetupHash, u.SetupExpires, txt(u.EntraName), txt(u.EntraOID), u.Created, u.LastLogin)
	}
	for _, e := range ch.audit {
		w.exec(`INSERT INTO audit (time, who, ip, action, target, detail, ok) VALUES ($1, $2, $3, $4, $5, $6, $7)`,
			e.Time, txt(e.Who), txt(e.IP), txt(e.Action), txt(e.Target), txt(e.Detail), e.OK)
	}
	if p.audits += len(ch.audit); p.audits >= 500 {
		p.audits = 0
		w.exec(`DELETE FROM audit WHERE seq <= (SELECT max(seq) FROM audit) - $1`, maxAuditDB)
	}
	if w.err != nil {
		return w.err
	}
	return tx.Commit()
}

// ---------------------------------------------------------------- tests

// TestDatabase is the environment variable that points the tests at a
// PostgreSQL server (test/pg.sh). Without it they run on the JSON file.
const TestDatabase = "C2_TEST_DATABASE_URL"

// OpenForTest opens the state that belongs to dir: the file in it, or, with
// TestDatabase set, a schema of its own in that database.
func OpenForTest(dir string) (*Store, error) {
	url := os.Getenv(TestDatabase)
	if url == "" {
		return Open(dir)
	}
	schema := fmt.Sprintf("t%x", sha256.Sum256([]byte(dir)))[:24]
	db, err := sql.Open("pgx", url)
	if err != nil {
		return nil, err
	}
	_, err = db.Exec(`CREATE SCHEMA IF NOT EXISTS ` + schema)
	db.Close()
	if err != nil {
		return nil, err
	}
	sep := "?"
	if strings.Contains(url, "?") {
		sep = "&"
	}
	return OpenPostgres(url+sep+"search_path="+schema, dir)
}
