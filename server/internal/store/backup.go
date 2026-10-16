package store

import (
	"crypto/rand"
	"time"
)

// BackupKey returns the key of the device's encrypted backup on its SD card,
// made the first time it is asked for. It stays here: the device gets it with
// each backup or restore command and forgets it again (CLAUDE.md, firmware
// src/sdbackup.cpp); the command keeps it only until it is handed over.
func (s *Store) BackupKey(id string) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return nil, ErrNotFound
	}
	if len(d.BackupKey) != 32 {
		k := make([]byte, 32)
		if _, err := rand.Read(k); err != nil {
			return nil, err
		}
		d.BackupKey = k
		s.ch.device(id)
		if err := s.save(); err != nil {
			return nil, err
		}
	}
	return append([]byte(nil), d.BackupKey...), nil
}

// BackupDue says whether a device should get its daily backup now, and if so
// notes the time - at most once in every.
func (s *Store) BackupDue(id string, every time.Duration) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil || d.State != StateActive || (d.BackupAt != nil && s.now().Sub(*d.BackupAt) < every) {
		return false
	}
	for _, c := range d.Commands {
		if c.Type == "sd_backup" && (c.State == CmdQueued || c.State == CmdSent) {
			return false
		}
	}
	now := s.now()
	d.BackupAt = &now
	s.ch.device(id)
	return true
}
