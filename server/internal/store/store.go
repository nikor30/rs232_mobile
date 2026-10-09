// Package store keeps the server's durable state: its own key, the enrollment
// tokens, the devices and their command queues.
//
// Phase 1 keeps everything in one JSON file that is rewritten atomically on
// every change. That is enough for a handful of devices and one server process;
// KONZEPT.md lists the move to a database as part of the cloud phase.
package store

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"time"

	"rs232c2/internal/proto"
)

const (
	StatePending = "pending" // enrolled, waiting for a person to confirm the code
	StateActive  = "active"
	StateRevoked = "revoked"

	CmdQueued = "queued"
	CmdSent   = "sent"
	CmdDone   = "done"
	CmdFailed = "failed"

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
	Done    *time.Time      `json:"done,omitempty"`
	Result  json.RawMessage `json:"result,omitempty"`
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
}

type data struct {
	ServerKey []byte             `json:"server_key"`
	Tokens    map[string]*Token  `json:"tokens"`
	Devices   map[string]*Device `json:"devices"`
	NextCmd   uint64             `json:"next_cmd"`
}

type Store struct {
	mu   sync.Mutex
	path string
	d    data
	key  *proto.StaticKey
	now  func() time.Time
}

// Open loads the state file in dir, creating it (and the server key) on first use.
func Open(dir string) (*Store, error) {
	s := &Store{path: filepath.Join(dir, "state.json"), now: time.Now}
	b, err := os.ReadFile(s.path)
	switch {
	case err == nil:
		if err := json.Unmarshal(b, &s.d); err != nil {
			return nil, err
		}
	case !os.IsNotExist(err):
		return nil, err
	}
	if s.d.Tokens == nil {
		s.d.Tokens = map[string]*Token{}
	}
	if s.d.Devices == nil {
		s.d.Devices = map[string]*Device{}
	}
	if s.d.ServerKey == nil {
		k, err := proto.GenerateStatic()
		if err != nil {
			return nil, err
		}
		s.d.ServerKey = k.Marshal()
		if err := s.save(); err != nil {
			return nil, err
		}
	}
	if s.key, err = proto.ParseStatic(s.d.ServerKey); err != nil {
		return nil, err
	}
	return s, nil
}

// SetClock replaces the time source (tests).
func (s *Store) SetClock(now func() time.Time) { s.now = now }

func (s *Store) ServerKey() *proto.StaticKey { return s.key }

func (s *Store) save() error {
	b, err := json.MarshalIndent(&s.d, "", " ")
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, s.path)
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
		}
	}
	t := &Token{ID: hex.EncodeToString(id), Secret: secret, Note: note, Expires: s.now().Add(ttl)}
	s.d.Tokens[t.ID] = t
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
	return &c
}

func (s *Store) Device(id string) (*Device, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return nil, ErrNotFound
	}
	return clone(d), nil
}

func (s *Store) Devices() []*Device {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*Device, 0, len(s.d.Devices))
	for _, d := range s.d.Devices {
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
	open := 0
	for _, c := range d.Commands {
		if c.State == CmdQueued || c.State == CmdSent {
			open++
		}
	}
	if open >= MaxQueued {
		return nil, ErrFull
	}
	s.d.NextCmd++
	c := &Command{ID: s.d.NextCmd, Type: typ, Args: args, State: CmdQueued, Created: s.now()}
	d.Commands = append(d.Commands, c)
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
	if status != nil {
		d.Status = status
	}
	if d.State != StateActive {
		return d.State, nil, s.save()
	}
	for _, r := range results {
		for _, c := range d.Commands {
			if c.ID == r.ID && c.State == CmdSent {
				c.State, c.Result, c.Done = CmdDone, r.Out, &now
				if !r.OK {
					c.State = CmdFailed
				}
			}
		}
	}
	for _, c := range d.Commands {
		if c.State == CmdQueued {
			c.State = CmdSent
			cmds = append(cmds, *c)
		}
	}
	return d.State, cmds, s.save()
}
