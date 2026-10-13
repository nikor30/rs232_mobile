package store

import (
	"slices"
	"sort"
	"strings"
)

// A group ties devices to the people who may work with them. A device can be
// in several groups; an account that is not an administrator sees a device
// only through a group it is a member of, with the right that membership has.

const (
	RoleAdmin  = ""       // everything, also accounts, library and server
	RoleMember = "member" // only the devices of its groups

	RightUse    = "use"    // look at a device and open a remote console
	RightManage = "manage" // also commands, settings, templates, firmware

	MaxGroups = 200
)

type Group struct {
	ID      uint64            `json:"id"`
	Name    string            `json:"name"`
	Note    string            `json:"note,omitempty"`
	Members map[string]string `json:"members,omitempty"` // account name -> RightUse or RightManage
}

func cloneGroup(g *Group) *Group {
	c := *g
	c.Members = map[string]string{}
	for k, v := range g.Members {
		c.Members[k] = v
	}
	return &c
}

func (ch *changes) group(id uint64) {
	if ch.groups == nil {
		ch.groups = map[uint64]bool{}
	}
	ch.groups[id] = true
}

// Groups returns all groups by name.
func (s *Store) Groups() []*Group {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*Group, 0, len(s.d.Groups))
	for _, g := range s.d.Groups {
		out = append(out, cloneGroup(g))
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

// PutGroup stores a group: a new one for id 0, otherwise that one. Members
// that are no accounts (any more) are left out.
func (s *Store) PutGroup(id uint64, name, note string, members map[string]string) (*Group, error) {
	if name == "" || len(name) > 48 || len(note) > 200 || len(members) > 500 {
		return nil, ErrArgs
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, g := range s.d.Groups {
		if g.ID != id && strings.EqualFold(g.Name, name) {
			return nil, ErrExists
		}
	}
	keep := map[string]string{}
	for user, right := range members {
		if right != RightUse && right != RightManage {
			return nil, ErrArgs
		}
		if s.d.Users[user] != nil {
			keep[user] = right
		}
	}
	if s.d.Groups == nil {
		s.d.Groups = map[uint64]*Group{}
	}
	g := s.d.Groups[id]
	if id == 0 {
		if len(s.d.Groups) >= MaxGroups {
			return nil, ErrFull
		}
		s.d.NextGroup++
		g = &Group{ID: s.d.NextGroup}
		s.d.Groups[g.ID] = g
		s.ch.meta = true
	} else if g == nil {
		return nil, ErrNotFound
	}
	g.Name, g.Note, g.Members = name, note, keep
	s.ch.group(g.ID)
	return cloneGroup(g), s.save()
}

// DeleteGroup removes a group and takes every device out of it.
func (s *Store) DeleteGroup(id uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.d.Groups[id] == nil {
		return ErrNotFound
	}
	delete(s.d.Groups, id)
	s.ch.group(id)
	for _, d := range s.d.Devices {
		if i := slices.Index(d.Groups, id); i >= 0 {
			d.Groups = slices.Delete(slices.Clone(d.Groups), i, i+1)
			s.ch.device(d.ID)
		}
	}
	return s.save()
}

// SetGroups says which groups a device is in.
func (s *Store) SetGroups(id string, groups []uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return ErrNotFound
	}
	set := []uint64{}
	for _, g := range groups {
		if s.d.Groups[g] == nil {
			return ErrArgs
		}
		if !slices.Contains(set, g) {
			set = append(set, g)
		}
	}
	slices.Sort(set)
	d.Groups = set
	s.ch.device(id)
	return s.save()
}

// Right says what an account may do with a device: "" (nothing), RightUse or
// RightManage. Administrators manage everything.
func (s *Store) Right(user, role, device string) string {
	if role == RoleAdmin {
		return RightManage
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[device]
	if d == nil {
		return ""
	}
	best := ""
	for _, id := range d.Groups {
		if g := s.d.Groups[id]; g != nil {
			switch g.Members[user] {
			case RightManage:
				return RightManage
			case RightUse:
				best = RightUse
			}
		}
	}
	return best
}

// forgetMember takes a deleted account out of every group. Caller holds the lock.
func (s *Store) forgetMember(name string) {
	for _, g := range s.d.Groups {
		if _, ok := g.Members[name]; ok {
			delete(g.Members, name)
			s.ch.group(g.ID)
		}
	}
}
