package store

import (
	"errors"
	"time"
)

// Administrator accounts and the record of what administrators did. The store
// only keeps them; what a password or a code is worth is decided in
// internal/server.

const (
	UserLocal = "local" // password and one-time codes (TOTP), both kept here
	UserEntra = "entra" // signs in at Microsoft Entra ID; nothing secret is kept here

	maxAudit = 5000
)

var ErrExists = errors.New("exists already")

type User struct {
	Name     string `json:"name"` // lower case
	Kind     string `json:"kind"`
	Disabled bool   `json:"disabled,omitempty"`

	// local accounts
	PassSalt []byte `json:"pass_salt,omitempty"`
	PassHash []byte `json:"pass_hash,omitempty"`
	PassIter int    `json:"pass_iter,omitempty"`
	TOTP     []byte `json:"totp,omitempty"`      // the shared secret
	TOTPOk   bool   `json:"totp_ok,omitempty"`   // the owner has proved that the authenticator has it
	TOTPStep int64  `json:"totp_step,omitempty"` // time step of the last code taken: none works twice
	// A new account, or one that was reset, is set up by its owner with a
	// code the administrator hands over. Only its hash is kept.
	SetupHash    []byte     `json:"setup_hash,omitempty"`
	SetupExpires *time.Time `json:"setup_expires,omitempty"`

	// Entra accounts: who may use this one. The object ID is what counts; an
	// account made for a sign-in name is tied to the object ID at first login.
	EntraName string `json:"entra_name,omitempty"`
	EntraOID  string `json:"entra_oid,omitempty"`

	Created   time.Time  `json:"created"`
	LastLogin *time.Time `json:"last_login,omitempty"`
}

// AuditEntry is one thing somebody did, or tried, with administrator rights.
type AuditEntry struct {
	Time   time.Time `json:"time"`
	Who    string    `json:"who"` // account name, "token" for the admin token, "-" if nobody was signed in
	IP     string    `json:"ip"`
	Action string    `json:"action"`
	Target string    `json:"target,omitempty"`
	Detail string    `json:"detail,omitempty"`
	OK     bool      `json:"ok"`
}

func cloneUser(u *User) *User {
	c := *u
	if u.SetupExpires != nil {
		t := *u.SetupExpires
		c.SetupExpires = &t
	}
	if u.LastLogin != nil {
		t := *u.LastLogin
		c.LastLogin = &t
	}
	return &c
}

// User returns a copy of the account.
func (s *Store) User(name string) (*User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	u := s.d.Users[name]
	if u == nil {
		return nil, ErrNotFound
	}
	return cloneUser(u), nil
}

func (s *Store) Users() []*User {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*User, 0, len(s.d.Users))
	for _, u := range s.d.Users {
		out = append(out, cloneUser(u))
	}
	return out
}

func (s *Store) AddUser(u *User) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.d.Users[u.Name] != nil {
		return ErrExists
	}
	c := cloneUser(u)
	c.Created = s.now()
	s.d.Users[u.Name] = c
	return s.save()
}

// UpdateUser changes an account under the store's lock. If change returns an
// error nothing is written and the account stays as it was.
func (s *Store) UpdateUser(name string, change func(u *User) error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	u := s.d.Users[name]
	if u == nil {
		return ErrNotFound
	}
	c := cloneUser(u)
	if err := change(c); err != nil {
		return err
	}
	c.Name, c.Created = u.Name, u.Created
	s.d.Users[name] = c
	return s.save()
}

func (s *Store) DeleteUser(name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.d.Users[name] == nil {
		return ErrNotFound
	}
	delete(s.d.Users, name)
	return s.save()
}

// Audit appends to the record. The oldest entries go when it is full.
func (s *Store) Audit(e AuditEntry) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	e.Time = s.now()
	s.d.Audit = append(s.d.Audit, &e)
	if over := len(s.d.Audit) - maxAudit; over > 0 {
		s.d.Audit = append([]*AuditEntry(nil), s.d.Audit[over:]...)
	}
	return s.save()
}

// AuditLog returns the newest n entries, newest first.
func (s *Store) AuditLog(n int) []AuditEntry {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := []AuditEntry{}
	for i := len(s.d.Audit) - 1; i >= 0 && len(out) < n; i-- {
		out = append(out, *s.d.Audit[i])
	}
	return out
}
