package store

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

// These tests run on the JSON file, and on PostgreSQL if TestDatabase is set
// (test/pg.sh): both backends have to give back what they were given.

var t0 = time.Date(2026, 10, 12, 9, 0, 0, 0, time.UTC)

type clock struct{ now time.Time }

func openT(t *testing.T, dir string, c *clock) *Store {
	t.Helper()
	s, err := OpenForTest(dir)
	if err != nil {
		t.Fatal(err)
	}
	s.SetClock(func() time.Time { return c.now })
	t.Cleanup(func() { s.Close() })
	return s
}

func onPostgres(t *testing.T) {
	if os.Getenv(TestDatabase) == "" {
		t.Skip("needs " + TestDatabase)
	}
}

// snapshot is everything the store holds, in a stable order.
func snapshot(t *testing.T, s *Store) string {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	devs := []*Device{}
	for _, d := range s.d.Devices {
		devs = append(devs, d)
	}
	sort.Slice(devs, func(i, j int) bool { return devs[i].ID < devs[j].ID })
	users := []*User{}
	for _, u := range s.d.Users {
		users = append(users, u)
	}
	sort.Slice(users, func(i, j int) bool { return users[i].Name < users[j].Name })
	toks := []*Token{}
	for _, k := range s.d.Tokens {
		toks = append(toks, k)
	}
	sort.Slice(toks, func(i, j int) bool { return toks[i].ID < toks[j].ID })
	b, err := json.MarshalIndent(map[string]any{"key": s.d.ServerKey, "next": s.d.NextCmd, "old": s.d.OldKey, "until": s.d.OldUntil,
		"tokens": toks, "devices": devs, "users": users, "audit": s.d.Audit}, "", " ")
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func must(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}

// enroll brings a device with the given key into the store, pending.
func enroll(t *testing.T, s *Store, pub, name string) string {
	t.Helper()
	tok, err := s.NewToken(time.Hour, "for "+name)
	must(t, err)
	must(t, s.Enroll(tok.ID, []byte(pub), name, json.RawMessage(`{"fw":"1.8.0","ports":4}`), "123456"))
	for _, d := range s.Devices() {
		if d.Name == name {
			return d.ID
		}
	}
	t.Fatal("enrolled device not found")
	return ""
}

// fill puts one of everything into the store.
func fill(t *testing.T, s *Store, c *clock) (active string) {
	t.Helper()
	active = enroll(t, s, "key-of-device-one", "Schrank 3")
	if err := s.Approve(active, "000000"); !errors.Is(err, ErrSAS) {
		t.Fatalf("wrong code: %v", err)
	}
	must(t, s.Approve(active, "123456"))
	enroll(t, s, "key-of-device-two", "wartet")
	gone := enroll(t, s, "key-of-device-three", "gesperrt")
	must(t, s.Revoke(gone))
	_, err := s.NewToken(time.Hour, "unbenutzt")
	must(t, err)

	c.now = c.now.Add(time.Minute)
	ping, err := s.Enqueue(active, "ping", nil)
	must(t, err)
	con, err := s.Enqueue(active, CmdConsole, json.RawMessage(`{"port":2}`))
	must(t, err)
	_, cmds, err := s.Poll(active, json.RawMessage(`{"bat":87,"rssi":-71}`), nil)
	must(t, err)
	if len(cmds) != 2 {
		t.Fatalf("got %d commands", len(cmds))
	}
	c.now = c.now.Add(time.Second)
	_, _, err = s.Poll(active, nil, []Result{{ID: ping.ID, OK: true, Out: json.RawMessage(`"pong"`)}, {ID: con.ID, OK: true}})
	must(t, err)
	_, err = s.ConsoleOpen(active, con.ID, time.Minute)
	must(t, err)
	c.now = c.now.Add(time.Minute)
	must(t, s.ConsoleClose(active, con.ID, "vom Administrator beendet", 12, 3456))
	bad, err := s.Enqueue(active, "status", nil)
	must(t, err)
	_, _, err = s.Poll(active, nil, nil)
	must(t, err)
	_, _, err = s.Poll(active, nil, []Result{{ID: bad.ID, OK: false, Out: json.RawMessage(`{"err":"nein"}`)}})
	must(t, err)
	_, err = s.Enqueue(active, "ping", nil) // stays queued
	must(t, err)

	exp := c.now.Add(24 * time.Hour)
	must(t, s.AddUser(&User{Name: "neu", Kind: UserLocal, SetupHash: []byte{1, 2, 3}, SetupExpires: &exp}))
	must(t, s.AddUser(&User{Name: "anna", Kind: UserLocal}))
	must(t, s.UpdateUser("anna", func(u *User) error {
		login := c.now
		u.PassSalt, u.PassHash, u.PassIter = []byte("salt"), []byte("hash"), 600000
		u.TOTP, u.TOTPOk, u.TOTPStep, u.LastLogin = []byte("secret"), true, 57343211, &login
		return nil
	}))
	must(t, s.AddUser(&User{Name: "bob@firma.de", Kind: UserEntra, EntraName: "bob@firma.de", EntraOID: "0000-1111", Disabled: true}))
	must(t, s.AddUser(&User{Name: "weg", Kind: UserLocal}))
	must(t, s.DeleteUser("weg"))
	if err := s.AddUser(&User{Name: "anna", Kind: UserLocal}); !errors.Is(err, ErrExists) {
		t.Fatalf("second anna: %v", err)
	}
	must(t, s.Audit(AuditEntry{Who: "anna", IP: "192.0.2.7", Action: "POST /admin/v1/users", Target: "bob@firma.de", Detail: "entra", OK: true}))
	must(t, s.Audit(AuditEntry{Who: "-", IP: "2001:db8::1", Action: "login", Target: "root", OK: false}))
	return active
}

func TestRoundTrip(t *testing.T) {
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	active := fill(t, s, c)
	want := snapshot(t, s)
	fp := s.ServerKey().Public()
	must(t, s.Close())

	s = openT(t, dir, c)
	if got := snapshot(t, s); got != want {
		t.Fatalf("after reopening:\n%s\nbefore:\n%s", got, want)
	}
	if string(s.ServerKey().Public()) != string(fp) {
		t.Fatal("server key changed")
	}
	d, err := s.Device(active)
	must(t, err)
	var status bytes.Buffer // the file is indented
	must(t, json.Compact(&status, d.Status))
	if len(d.Commands) != 4 || len(d.Consoles) != 1 || d.Consoles[0].Out != 3456 || status.String() != `{"bat":87,"rssi":-71}` {
		t.Fatalf("device: %d commands, %d consoles, status %s", len(d.Commands), len(d.Consoles), d.Status)
	}
	if log := s.AuditLog(10); len(log) != 2 || log[0].Action != "login" || log[1].Who != "anna" {
		t.Fatalf("audit: %+v", log)
	}
	// and it goes on where it stopped
	next, err := s.Enqueue(active, "ping", nil)
	must(t, err)
	if next.ID != 5 {
		t.Fatalf("next command got ID %d", next.ID)
	}
}

func TestOpenConsoleEndsWithRestart(t *testing.T) {
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	id := enroll(t, s, "key", "a")
	must(t, s.Approve(id, "123456"))
	con, err := s.Enqueue(id, CmdConsole, json.RawMessage(`{"port":1}`))
	must(t, err)
	_, _, err = s.Poll(id, nil, nil)
	must(t, err)
	_, _, err = s.Poll(id, nil, []Result{{ID: con.ID, OK: true}})
	must(t, err)
	_, err = s.ConsoleOpen(id, con.ID, time.Minute)
	must(t, err)
	must(t, s.Close())

	for i := 0; i < 2; i++ { // the second start reads what the first one wrote
		s = openT(t, dir, c)
		d, err := s.Device(id)
		must(t, err)
		if len(d.Consoles) != 1 || d.Consoles[0].End == nil || d.Consoles[0].Reason != "Server neu gestartet" {
			t.Fatalf("start %d: console %+v", i, d.Consoles)
		}
		must(t, s.Close())
	}
}

// What the store drops in memory must go from the backend too.
func TestHistoryIsBounded(t *testing.T) {
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	id := enroll(t, s, "key", "a")
	must(t, s.Approve(id, "123456"))
	for i := 0; i < 2*MaxQueued+30; i++ {
		cmd, err := s.Enqueue(id, "ping", nil)
		must(t, err)
		_, _, err = s.Poll(id, nil, nil)
		must(t, err)
		_, _, err = s.Poll(id, nil, []Result{{ID: cmd.ID, OK: true}})
		must(t, err)
	}
	// five wrong codes throw a pending device away
	lost := enroll(t, s, "other", "b")
	for i := 0; i < MaxSASAttempts; i++ {
		s.Approve(lost, "999999")
	}
	want := snapshot(t, s)
	must(t, s.Close())

	s = openT(t, dir, c)
	if got := snapshot(t, s); got != want {
		t.Fatal("state differs after reopening")
	}
	d, err := s.Device(id)
	must(t, err)
	if len(d.Commands) != 2*MaxQueued || d.Commands[0].ID != 31 {
		t.Fatalf("%d commands, first %d", len(d.Commands), d.Commands[0].ID)
	}
	if _, err := s.Device(lost); !errors.Is(err, ErrNotFound) {
		t.Fatalf("thrown-away device: %v", err)
	}
}

// A command that ran out of time while nobody wrote is closed in the backend
// with the next change.
func TestExpiryIsWritten(t *testing.T) {
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	id := enroll(t, s, "key", "a")
	must(t, s.Approve(id, "123456"))
	_, err := s.Enqueue(id, "ping", nil)
	must(t, err)
	c.now = c.now.Add(QueueTTL + time.Minute)
	s.Devices() // notices it, writes nothing
	must(t, s.Audit(AuditEntry{Who: "x", Action: "y"}))
	must(t, s.Close())

	c2 := &clock{now: t0} // a clock that would not expire it again
	s = openT(t, dir, c2)
	s.mu.Lock()
	state := s.d.Devices[id].Commands[0].State
	s.mu.Unlock()
	if state != CmdExpired {
		t.Fatalf("state %q", state)
	}
}

// ---------------------------------------------------------------- PostgreSQL only

func TestPostgresTakesWhatDevicesSend(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	tok, err := s.NewToken(time.Hour, "zero\x00byte and \xff\xfe")
	must(t, err)
	must(t, s.Enroll(tok.ID, []byte("key"), "name\x00with\xc3\x28junk", json.RawMessage("{\"a\":\"\\u0000\",\"b\":\"\xff\"}"), "123456"))
	id := s.Devices()[0].ID
	must(t, s.Approve(id, "123456"))
	con, err := s.Enqueue(id, CmdConsole, json.RawMessage(`{"port":1}`))
	must(t, err)
	_, _, err = s.Poll(id, json.RawMessage(`{"s":"\u0000\ud800"}`), nil)
	must(t, err)
	_, _, err = s.Poll(id, nil, []Result{{ID: con.ID, OK: true, Out: json.RawMessage("\"\\u0000 \xc0\"")}})
	must(t, err)
	_, err = s.ConsoleOpen(id, con.ID, time.Minute)
	must(t, err)
	must(t, s.ConsoleClose(id, con.ID, "reason\x00\xff", 1, 2))
	must(t, s.Audit(AuditEntry{Who: "-", IP: "x", Action: "login", Target: "ro\x00ot\xff", Detail: strings.Repeat("ä", 5000)}))
	must(t, s.Close())

	s = openT(t, dir, c)
	d, err := s.Device(id)
	must(t, err)
	if d.Name != "namewith\uFFFD(junk" || d.Consoles[0].Reason != "reason\uFFFD" || string(d.Status) != `{"s":"\u0000\ud800"}` {
		t.Fatalf("name %q reason %q status %s", d.Name, d.Consoles[0].Reason, d.Status)
	}
	if !json.Valid(d.Info) || !json.Valid(d.Commands[0].Result) {
		t.Fatalf("info %q result %q", d.Info, d.Commands[0].Result)
	}
	if log := s.AuditLog(1); log[0].Target != "root\uFFFD" || len(log[0].Detail) != 10000 {
		t.Fatalf("audit %q", log[0].Target)
	}
}

func TestPostgresOneServerPerDatabase(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	if _, err := OpenForTest(dir); err == nil || !strings.Contains(err.Error(), "another server") {
		t.Fatalf("second server: %v", err)
	}
	must(t, s.Close())
	s2, err := OpenForTest(dir) // the lock went with the first one
	must(t, err)
	s2.Close()
}

func TestPostgresImportsTheFile(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	f, err := Open(dir) // a server that ran on the file
	must(t, err)
	f.SetClock(func() time.Time { return c.now })
	fill(t, f, c)
	want := snapshot(t, f)
	must(t, f.Close())

	s := openT(t, dir, c)
	path := filepath.Join(dir, stateFile)
	if s.Imported() != path+".imported" {
		t.Fatalf("imported %q", s.Imported())
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("the file is still there: %v", err)
	}
	if got := snapshot(t, s); got != want {
		t.Fatalf("imported:\n%s\nfile:\n%s", got, want)
	}
	must(t, s.Close())

	// The database now has a state: a file that turns up later is left alone.
	must(t, os.Rename(path+".imported", path))
	s = openT(t, dir, c)
	if s.Imported() != "" {
		t.Fatal("imported twice")
	}
	if got := snapshot(t, s); got != want {
		t.Fatal("state differs after the second start")
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatal(err)
	}
}

func TestPostgresKeepsMoreAuditThanItShows(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	for i := 0; i < maxAudit+20; i++ {
		s.mu.Lock() // many entries, one transaction
		e := &AuditEntry{Time: c.now, Who: "x", Action: fmt.Sprint(i)}
		s.d.Audit = append(s.d.Audit, e)
		s.ch.audit = append(s.ch.audit, e)
		s.mu.Unlock()
	}
	must(t, s.Audit(AuditEntry{Who: "x", Action: "last"}))
	must(t, s.Close())
	s = openT(t, dir, c)
	log := s.AuditLog(maxAudit + 100)
	if len(log) != maxAudit || log[0].Action != "last" || log[maxAudit-1].Action != "21" {
		t.Fatalf("%d entries, newest %q, oldest %q", len(log), log[0].Action, log[len(log)-1].Action)
	}
	var n int
	must(t, s.b.(*pg).db.QueryRow(`SELECT count(*) FROM audit`).Scan(&n))
	if n != maxAudit+21 {
		t.Fatalf("%d rows", n)
	}
}

func TestCheckNoticesALostBackend(t *testing.T) {
	if os.Getenv(TestDatabase) != "" {
		t.Skip("the file only: a database does not go away with a directory")
	}
	dir, c := filepath.Join(t.TempDir(), "data"), &clock{now: t0}
	must(t, os.Mkdir(dir, 0o700))
	s := openT(t, dir, c)
	must(t, s.Check())
	must(t, os.RemoveAll(dir))
	if s.Check() == nil {
		t.Fatal("the directory is gone and nobody noticed")
	}
	c.now = c.now.Add(time.Hour)
	if s.Check() == nil {
		t.Fatal("still gone")
	}
	if since, err := s.Health(); err == nil || !since.Equal(t0) {
		t.Fatalf("health: since %v, %v", since, err)
	}
	if _, err := s.NewToken(time.Hour, "x"); err == nil {
		t.Fatal("a token was saved into nothing")
	}
	// What could not be written goes out with the first check that works again.
	must(t, os.Mkdir(dir, 0o700))
	c.now = c.now.Add(time.Hour)
	must(t, s.Check())
	if since, err := s.Health(); err != nil || !since.IsZero() {
		t.Fatalf("health after the repair: since %v, %v", since, err)
	}
	before := snapshot(t, s)
	must(t, s.Close())
	if after := snapshot(t, openT(t, dir, c)); after != before {
		t.Fatalf("what was not written at first is missing:\n%s\n---\n%s", before, after)
	}
}

// dropLock ends the connection that holds the server's lock, the way a
// restart of the database does.
func dropLock(t *testing.T, s *Store) {
	t.Helper()
	p := s.b.(*pg)
	var pid int
	must(t, p.lock.QueryRowContext(t.Context(), `SELECT pg_backend_pid()`).Scan(&pid))
	_, err := p.db.Exec(`SELECT pg_terminate_backend($1)`, pid)
	must(t, err)
	time.Sleep(200 * time.Millisecond)
}

func TestPostgresTakesTheLockAgain(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	dropLock(t, s)
	must(t, s.Check())
	if _, err := OpenForTest(dir); err == nil || !strings.Contains(err.Error(), "another server") {
		t.Fatalf("second server after the lock was taken again: %v", err)
	}
	if _, err := s.NewToken(time.Hour, "x"); err != nil {
		t.Fatalf("save after the lock was taken again: %v", err)
	}
}

func TestPostgresStopsWritingWhenAnotherServerTookOver(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	dropLock(t, s)
	s2, err := OpenForTest(dir) // faster than the first one's next check
	must(t, err)
	defer s2.Close()
	if err := s.Check(); !errors.Is(err, ErrTaken) {
		t.Fatalf("check: %v", err)
	}
	if _, err := s.NewToken(time.Hour, "x"); !errors.Is(err, ErrTaken) {
		t.Fatalf("the first server still writes: %v", err)
	}
	if _, err := s2.NewToken(time.Hour, "y"); err != nil {
		t.Fatalf("the second server: %v", err)
	}
}

func TestKeyChangeIsKept(t *testing.T) {
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	first := s.ServerKey().Public()
	a, b := enroll(t, s, "a", "eins"), enroll(t, s, "b", "zwei")
	_, err := s.NewToken(time.Hour, "offen")
	must(t, err)
	must(t, s.RotateKey(48*time.Hour))
	if bytes.Equal(first, s.ServerKey().Public()) {
		t.Fatal("the key is the same")
	}
	if err := s.RotateKey(time.Hour); !errors.Is(err, ErrState) {
		t.Fatalf("a second change on top: %v", err)
	}
	must(t, s.ConfirmKey(a, "0000000000000000")) // not the new key: does not count
	must(t, s.ConfirmKey(b, KeyTag(s.ServerKey().Public())))
	before := snapshot(t, s)
	must(t, s.Close())

	s = openT(t, dir, c)
	if after := snapshot(t, s); after != before {
		t.Fatalf("the key change did not survive:\n%s\n---\n%s", before, after)
	}
	if k, old, err := s.KeyFor(a); err != nil || !old || !bytes.Equal(k.Public(), first) {
		t.Fatalf("a device on the old key: old=%v %v", old, err)
	}
	if k, old, err := s.KeyFor(b); err != nil || old || !bytes.Equal(k.Public(), s.ServerKey().Public()) {
		t.Fatalf("a device on the new key: old=%v %v", old, err)
	}
	if old, until, waiting := s.KeyChange(); !bytes.Equal(old, first) || !until.Equal(t0.Add(48*time.Hour)) || waiting != 1 {
		t.Fatalf("key change: until %v, %d waiting", until, waiting)
	}

	// When the time is up the old private key leaves the backend too.
	c.now = c.now.Add(49 * time.Hour)
	if _, _, err := s.KeyFor(a); !errors.Is(err, ErrState) {
		t.Fatalf("after the time is up: %v", err)
	}
	must(t, s.Check())
	must(t, s.Close())
	s = openT(t, dir, c)
	s.mu.Lock()
	gone := s.d.OldKey == nil && s.d.OldUntil == nil && s.old == nil
	s.mu.Unlock()
	if !gone {
		t.Fatal("the old key is still kept")
	}
	if _, _, err := s.KeyFor(a); !errors.Is(err, ErrState) {
		t.Fatalf("after the restart: %v", err)
	}
	must(t, s.RotateKey(time.Hour)) // and the next change can begin
}

// A database from before the key change (schema 1) is brought up to date, and
// what it held is still there.
func TestPostgresMigratesFromTheFirstSchema(t *testing.T) {
	onPostgres(t)
	dir, c := t.TempDir(), &clock{now: t0}
	s := openT(t, dir, c)
	id := enroll(t, s, "a", "eins")
	key := s.ServerKey().Public()
	p := s.b.(*pg)
	for _, q := range []string{
		`ALTER TABLE server DROP COLUMN old_key, DROP COLUMN old_until`,
		`ALTER TABLE devices DROP COLUMN old_key`,
		`DELETE FROM schema_version WHERE version > 1`,
	} {
		_, err := p.db.Exec(q)
		must(t, err)
	}
	must(t, s.Close())

	s = openT(t, dir, c)
	if !bytes.Equal(s.ServerKey().Public(), key) {
		t.Fatal("the server key changed")
	}
	if _, old, err := s.KeyFor(id); err != nil || old {
		t.Fatalf("the device: old=%v %v", old, err)
	}
	must(t, s.RotateKey(time.Hour))
	if _, old, err := s.KeyFor(id); err != nil || !old {
		t.Fatalf("after a key change: old=%v %v", old, err)
	}
}
