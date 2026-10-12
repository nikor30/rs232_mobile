package store

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// The store works on its state in memory and hands every change to a backend
// that makes it durable: one JSON file (local use, tests) or PostgreSQL.
type backend interface {
	// load returns what is kept, or an empty state if nothing is yet.
	load() (*data, error)
	// save writes the state. ch names what has changed since the last save
	// that worked; a backend may also write everything.
	save(d *data, ch *changes) error
	// check says whether the backend is still there to write to.
	check() error
	close() error
}

// changes names the parts of the state that differ from what the backend has.
// A token, device or user that is named here and no longer exists was deleted.
type changes struct {
	meta    bool // server key, command counter
	tokens  map[string]bool
	devices map[string]*devChange
	users   map[string]bool
	audit   []*AuditEntry // appended
}

type devChange struct {
	row  bool // the device itself
	cmds map[uint64]bool
	cons map[uint64]bool
}

func (ch *changes) empty() bool {
	return !ch.meta && len(ch.tokens) == 0 && len(ch.devices) == 0 && len(ch.users) == 0 && len(ch.audit) == 0
}

func (ch *changes) token(id string) {
	if ch.tokens == nil {
		ch.tokens = map[string]bool{}
	}
	ch.tokens[id] = true
}

func (ch *changes) user(name string) {
	if ch.users == nil {
		ch.users = map[string]bool{}
	}
	ch.users[name] = true
}

func (ch *changes) dev(id string) *devChange {
	if ch.devices == nil {
		ch.devices = map[string]*devChange{}
	}
	dc := ch.devices[id]
	if dc == nil {
		dc = &devChange{cmds: map[uint64]bool{}, cons: map[uint64]bool{}}
		ch.devices[id] = dc
	}
	return dc
}

func (ch *changes) device(id string)          { ch.dev(id).row = true }
func (ch *changes) cmd(id string, cmd uint64) { ch.dev(id).cmds[cmd] = true }
func (ch *changes) con(id string, con uint64) { ch.dev(id).cons[con] = true }

// everything marks the whole state as changed (import into an empty database).
func everything(d *data) changes {
	ch := changes{meta: true, audit: d.Audit}
	for id := range d.Tokens {
		ch.token(id)
	}
	for name := range d.Users {
		ch.user(name)
	}
	for id, dev := range d.Devices {
		ch.device(id)
		for _, c := range dev.Commands {
			ch.cmd(id, c.ID)
		}
		for _, l := range dev.Consoles {
			ch.con(id, l.ID)
		}
	}
	return ch
}

// ---------------------------------------------------------------- JSON file

// file keeps everything in one JSON file that is rewritten atomically on every
// change. That is enough for a handful of devices and one server process.
type file struct{ path string }

const stateFile = "state.json"

func readFile(path string) (*data, error) {
	d := &data{}
	b, err := os.ReadFile(path)
	switch {
	case err == nil:
		if err := json.Unmarshal(b, d); err != nil {
			return nil, err
		}
	case !os.IsNotExist(err):
		return nil, err
	}
	return d, nil
}

func (f *file) load() (*data, error) { return readFile(f.path) }

func (f *file) save(d *data, _ *changes) error {
	b, err := json.MarshalIndent(d, "", " ")
	if err != nil {
		return err
	}
	tmp := f.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, f.path)
}

func (f *file) check() error {
	_, err := os.Stat(filepath.Dir(f.path))
	return err
}

func (f *file) close() error { return nil }

// Open keeps the state in the file state.json in dir, creating it (and the
// server key) on first use.
func Open(dir string) (*Store, error) {
	return open(&file{path: filepath.Join(dir, stateFile)}, nil)
}
