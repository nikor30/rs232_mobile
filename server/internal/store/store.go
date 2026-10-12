// Package store keeps the server's durable state: its own key, the enrollment
// tokens, the devices and their command queues, the accounts and the audit log.
//
// The store works on the state in memory, under one lock, and writes every
// change through to a backend (backend.go): a JSON file, or PostgreSQL
// (postgres.go). Either way there is one server process per state.
package store

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"sync"
	"time"

	"rs232c2/internal/proto"
)

const (
	StatePending = "pending" // enrolled, waiting for a person to confirm the code
	StateActive  = "active"
	StateRevoked = "revoked"

	CmdQueued  = "queued"
	CmdSent    = "sent"
	CmdDone    = "done"
	CmdFailed  = "failed"
	CmdExpired = "expired" // never fetched, or fetched and never answered

	// A command nobody fetched is dropped after QueueTTL: an order given today
	// must not be carried out when the device shows up again next week. One that
	// was fetched but not answered (the device restarted, lost its link) is
	// closed after AnswerTTL - it is never sent a second time, because running
	// something twice can be worse than not running it.
	QueueTTL  = 24 * time.Hour
	AnswerTTL = 2 * time.Hour
	// A request for the remote console is only worth something while the
	// administrator is waiting for it: somebody at the device has to agree.
	ConsoleQueueTTL  = 2 * time.Minute
	ConsoleAnswerTTL = 3 * time.Minute
	CmdConsole       = "console"
	maxConsoleLogs   = 50

	// Wrong codes typed for one pending device before its enrollment is thrown
	// away: six digits must not be guessable by trying.
	MaxSASAttempts = 5
	MaxQueued      = 100
)

var (
	ErrNotFound = errors.New("not found")
	ErrState    = errors.New("not possible in this state")
	ErrSAS      = errors.New("code does not match")
	ErrToken    = errors.New("enrollment token unknown, used or expired")
	ErrFull     = errors.New("command queue is full")
	ErrBusy     = errors.New("a remote console is already open or asked for")
)

type Token struct {
	ID      string    `json:"id"` // hex
	Secret  []byte    `json:"secret"`
	Note    string    `json:"note,omitempty"`
	Expires time.Time `json:"expires"`
}

type Command struct {
	ID      uint64          `json:"id"`
	Type    string          `json:"type"`
	Args    json.RawMessage `json:"args,omitempty"`
	State   string          `json:"state"`
	Created time.Time       `json:"created"`
	Sent    *time.Time      `json:"sent,omitempty"`
	Done    *time.Time      `json:"done,omitempty"`
	Result  json.RawMessage `json:"result,omitempty"`
}

// ConsoleLog records one remote console session: who could reach which serial
// port, when, and how much went through. What was typed and shown is not kept.
type ConsoleLog struct {
	ID     uint64     `json:"id"` // the "console" command that asked for it
	Port   int        `json:"port"`
	Start  time.Time  `json:"start"`
	End    *time.Time `json:"end,omitempty"`
	Reason string     `json:"reason,omitempty"`
	In     int64      `json:"bytes_in"`  // typed by the administrator
	Out    int64      `json:"bytes_out"` // sent by the serial port
}

type Device struct {
	ID          string          `json:"id"` // hex of proto.DeviceID
	Pub         []byte          `json:"pub"`
	Name        string          `json:"name"`
	Info        json.RawMessage `json:"info,omitempty"` // what the device said about itself when enrolling
	State       string          `json:"state"`
	SAS         string          `json:"sas,omitempty"` // pending only; never leaves the server
	SASAttempts int             `json:"sas_attempts,omitempty"`
	Enrolled    time.Time       `json:"enrolled"`
	LastSeen    *time.Time      `json:"last_seen,omitempty"`
	Status      json.RawMessage `json:"status,omitempty"` // last status the device reported
	Commands    []*Command      `json:"commands,omitempty"`
	Consoles    []*ConsoleLog   `json:"consoles,omitempty"`
}

type data struct {
	ServerKey []byte             `json:"server_key"`
	Tokens    map[string]*Token  `json:"tokens"`
	Devices   map[string]*Device `json:"devices"`
	NextCmd   uint64             `json:"next_cmd"`
	Users     map[string]*User   `json:"users,omitempty"`
	Audit     []*AuditEntry      `json:"audit,omitempty"`
}

type Store struct {
	mu  sync.Mutex
	b   backend
	d   data
	ch  changes // what the backend has not been given yet
	key *proto.StaticKey
	now func() time.Time
	// "last seen" and the reported status change with every poll of every
	// device. They are written with the next real change, or after seenFlush.
	lastSave  time.Time
	tookFirst bool   // the state came from an import, not from the backend
	imported  string // and this is the file it came from
}

const seenFlush = time.Minute

// open loads the state from b. If there is none yet and first is given, the
// state starts as first (an import); otherwise with a new server key.
func open(b backend, first *data) (*Store, error) {
	s := &Store{b: b, now: time.Now}
	d, err := b.load()
	if err != nil {
		b.close()
		return nil, err
	}
	if d.ServerKey == nil && first != nil && first.ServerKey != nil {
		d = first
		s.ch = everything(d)
		s.tookFirst = true
	}
	s.d = *d
	if s.d.Tokens == nil {
		s.d.Tokens = map[string]*Token{}
	}
	if s.d.Devices == nil {
		s.d.Devices = map[string]*Device{}
	}
	if s.d.Users == nil {
		s.d.Users = map[string]*User{}
	}
	if s.d.ServerKey == nil {
		k, err := proto.GenerateStatic()
		if err != nil {
			b.close()
			return nil, err
		}
		s.d.ServerKey = k.Marshal()
		s.ch.meta = true
	}
	if s.key, err = proto.ParseStatic(s.d.ServerKey); err != nil {
		b.close()
		return nil, err
	}
	// Console sessions live in memory: one that was open when the server
	// stopped ended there, whatever the backend says.
	for _, d := range s.d.Devices {
		for _, c := range d.Consoles {
			if c.End == nil {
				now := s.now()
				c.End, c.Reason = &now, "Server neu gestartet"
				s.ch.con(d.ID, c.ID)
			}
		}
	}
	if !s.ch.empty() {
		if err := s.save(); err != nil {
			b.close()
			return nil, err
		}
	}
	return s, nil
}

// Close lets go of the backend. The store must not be used afterwards.
func (s *Store) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.b.close()
}

// SetClock replaces the time source (tests).
func (s *Store) SetClock(now func() time.Time) { s.now = now }

func (s *Store) ServerKey() *proto.StaticKey { return s.key }

// save hands the changes to the backend. If that fails they stay noted and go
// out with the next save. Callers hold the lock.
func (s *Store) save() error {
	if err := s.b.save(&s.d, &s.ch); err != nil {
		return err
	}
	s.ch = changes{}
	s.lastSave = s.now()
	return nil
}

// ---------------------------------------------------------------- tokens

func (s *Store) NewToken(ttl time.Duration, note string) (*Token, error) {
	id := make([]byte, proto.TokenIDSize)
	secret := make([]byte, proto.PSKSize)
	if _, err := rand.Read(id); err != nil {
		return nil, err
	}
	if _, err := rand.Read(secret); err != nil {
		return nil, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, t := range s.d.Tokens { // forget what has expired
		if s.now().After(t.Expires) {
			delete(s.d.Tokens, id)
			s.ch.token(id)
		}
	}
	t := &Token{ID: hex.EncodeToString(id), Secret: secret, Note: note, Expires: s.now().Add(ttl)}
	s.d.Tokens[t.ID] = t
	s.ch.token(t.ID)
	return t, s.save()
}

// TokenSecret returns the secret of a token that can still be used.
func (s *Store) TokenSecret(id string) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.d.Tokens[id]
	if t == nil || s.now().After(t.Expires) {
		return nil, ErrToken
	}
	return t.Secret, nil
}

// ---------------------------------------------------------------- devices

func clone(d *Device) *Device {
	c := *d
	c.Commands = nil
	for _, cmd := range d.Commands {
		cc := *cmd
		c.Commands = append(c.Commands, &cc)
	}
	c.Consoles = nil
	for _, l := range d.Consoles {
		ll := *l
		c.Consoles = append(c.Consoles, &ll)
	}
	return &c
}

// expire closes commands that have waited too long. Callers hold the lock.
func (s *Store) expire(d *Device) {
	now := s.now()
	for _, c := range d.Commands {
		queue, answer := QueueTTL, AnswerTTL
		if c.Type == CmdConsole {
			queue, answer = ConsoleQueueTTL, ConsoleAnswerTTL
		}
		if (c.State == CmdQueued && now.Sub(c.Created) > queue) ||
			(c.State == CmdSent && c.Sent != nil && now.Sub(*c.Sent) > answer) {
			c.State, c.Done = CmdExpired, &now
			s.ch.cmd(d.ID, c.ID) // written with the next save
		}
	}
}

func (s *Store) Device(id string) (*Device, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return nil, ErrNotFound
	}
	s.expire(d)
	return clone(d), nil
}

func (s *Store) Devices() []*Device {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*Device, 0, len(s.d.Devices))
	for _, d := range s.d.Devices {
		s.expire(d)
		out = append(out, clone(d))
	}
	return out
}

// Enroll uses up the token and records the device as pending. A device that is
// already known (same key) goes back to pending with the new code; a revoked
// one stays revoked.
func (s *Store) Enroll(tokenID string, pub []byte, name string, info json.RawMessage, sas string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	t := s.d.Tokens[tokenID]
	if t == nil || s.now().After(t.Expires) {
		return ErrToken
	}
	id := proto.IDString(proto.DeviceID(pub))
	d := s.d.Devices[id]
	if d != nil && d.State == StateRevoked {
		return ErrState
	}
	if d == nil {
		d = &Device{ID: id, Pub: pub}
		s.d.Devices[id] = d
	}
	delete(s.d.Tokens, tokenID)
	s.ch.token(tokenID)
	s.ch.device(id)
	d.Name, d.Info, d.State, d.SAS, d.SASAttempts, d.Enrolled = name, info, StatePending, sas, 0, s.now()
	return s.save()
}

// Approve activates a pending device if sas is the code its display shows.
func (s *Store) Approve(id, sas string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return ErrNotFound
	}
	if d.State != StatePending {
		return ErrState
	}
	s.ch.device(id)
	if subtle.ConstantTimeCompare([]byte(sas), []byte(d.SAS)) != 1 {
		d.SASAttempts++
		if d.SASAttempts >= MaxSASAttempts {
			delete(s.d.Devices, id) // start over with a new token
		}
		if err := s.save(); err != nil {
			return err
		}
		return ErrSAS
	}
	d.State, d.SAS, d.SASAttempts = StateActive, "", 0
	return s.save()
}

func (s *Store) Revoke(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return ErrNotFound
	}
	d.State, d.SAS = StateRevoked, ""
	s.ch.device(id)
	return s.save()
}

// ---------------------------------------------------------------- commands

func (s *Store) Enqueue(id, typ string, args json.RawMessage) (*Command, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return nil, ErrNotFound
	}
	if d.State != StateActive {
		return nil, ErrState
	}
	s.expire(d)
	open := 0
	for _, c := range d.Commands {
		if c.State == CmdQueued || c.State == CmdSent {
			open++
			if typ == CmdConsole && c.Type == CmdConsole {
				return nil, ErrBusy
			}
		}
	}
	if typ == CmdConsole {
		for _, l := range d.Consoles {
			if l.End == nil {
				return nil, ErrBusy
			}
		}
	}
	if open >= MaxQueued {
		return nil, ErrFull
	}
	s.d.NextCmd++
	c := &Command{ID: s.d.NextCmd, Type: typ, Args: args, State: CmdQueued, Created: s.now()}
	d.Commands = append(d.Commands, c)
	s.ch.meta = true
	s.ch.cmd(id, c.ID)
	if len(d.Commands) > 2*MaxQueued { // keep the history bounded
		d.Commands = d.Commands[len(d.Commands)-2*MaxQueued:]
	}
	cc := *c
	return &cc, s.save()
}

type Result struct {
	ID  uint64          `json:"id"`
	OK  bool            `json:"ok"`
	Out json.RawMessage `json:"out,omitempty"`
}

// Poll is one contact of a device: it stores what the device reports and
// returns its state and the commands it has not been given yet.
func (s *Store) Poll(id string, status json.RawMessage, results []Result) (state string, cmds []Command, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return "", nil, ErrNotFound
	}
	now := s.now()
	d.LastSeen = &now
	s.ch.device(id)
	if status != nil {
		d.Status = status
	}
	changed := now.Sub(s.lastSave) > seenFlush
	if d.State == StateActive {
		for _, r := range results {
			for _, c := range d.Commands {
				if c.ID == r.ID && c.State == CmdSent {
					c.State, c.Result, c.Done = CmdDone, r.Out, &now
					if !r.OK {
						c.State = CmdFailed
					}
					s.ch.cmd(id, c.ID)
					changed = true
				}
			}
		}
		s.expire(d)
		for _, c := range d.Commands {
			if c.State == CmdQueued {
				c.State, c.Sent = CmdSent, &now
				s.ch.cmd(id, c.ID)
				cmds = append(cmds, *c)
				changed = true
			}
		}
	}
	if changed {
		err = s.save()
	}
	return d.State, cmds, err
}

// ---------------------------------------------------------------- remote console

// ConsoleOpen starts the log entry of a remote console. The device may only
// open the one it has just agreed to: cmdID must be a "console" command that it
// answered with yes less than maxAge ago, and that has not been used before.
func (s *Store) ConsoleOpen(id string, cmdID uint64, maxAge time.Duration) (port int, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return 0, ErrNotFound
	}
	if d.State != StateActive {
		return 0, ErrState
	}
	for _, l := range d.Consoles {
		if l.ID == cmdID || l.End == nil {
			return 0, ErrState
		}
	}
	now := s.now()
	for _, c := range d.Commands {
		if c.ID != cmdID {
			continue
		}
		if c.Type != CmdConsole || c.State != CmdDone || c.Done == nil || now.Sub(*c.Done) > maxAge {
			return 0, ErrState
		}
		var a struct {
			Port int `json:"port"`
		}
		json.Unmarshal(c.Args, &a)
		d.Consoles = append(d.Consoles, &ConsoleLog{ID: cmdID, Port: a.Port, Start: now})
		s.ch.con(id, cmdID)
		if len(d.Consoles) > maxConsoleLogs {
			d.Consoles = d.Consoles[len(d.Consoles)-maxConsoleLogs:]
		}
		return a.Port, s.save()
	}
	return 0, ErrNotFound
}

// ConsoleClose completes the log entry.
func (s *Store) ConsoleClose(id string, cmdID uint64, reason string, in, out int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return ErrNotFound
	}
	for _, l := range d.Consoles {
		if l.ID == cmdID && l.End == nil {
			now := s.now()
			l.End, l.Reason, l.In, l.Out = &now, reason, in, out
			s.ch.con(id, cmdID)
			return s.save()
		}
	}
	return ErrNotFound
}
