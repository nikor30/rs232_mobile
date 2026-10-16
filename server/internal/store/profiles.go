package store

import (
	"sort"
	"strings"
	"time"
)

// Profile is what a new device gets when it is approved: where it belongs,
// who may work with it, and how it joins the network - prepared at the server
// for a customer or a site, so that the person at the device only has to scan
// the invitation and give it internet access through its hotspot.
//
// Changing a device from the control center needs its permission, and
// unattended access may only be granted at the device. A profile therefore
// only asks: the invitation carries a summary of it (EnrollToken.Profile), the
// device shows it before enrolling, and the person there accepts it - that is
// where Control and Unattended take effect. What follows at the approval
// (Wi-Fi, certificates) goes through the ordinary commands.
type Profile struct {
	ID          uint64       `json:"id"`
	Name        string       `json:"name"`
	Note        string       `json:"note,omitempty"`
	Region      string       `json:"region,omitempty"`
	Country     string       `json:"country,omitempty"`
	Site        string       `json:"site,omitempty"`
	Groups      []uint64     `json:"groups,omitempty"`
	Control     bool         `json:"control,omitempty"`      // ask the device to allow changes from here
	UnattendedH int          `json:"unattended_h,omitempty"` // ask for unattended access, hours (0..168)
	Wifi        *ProfileWifi `json:"wifi,omitempty"`
	Updated     time.Time    `json:"updated"`
	By          string       `json:"by,omitempty"`
}

// ProfileWifi is the WLAN client of the device, as its own settings name it.
type ProfileWifi struct {
	SSID       string `json:"ssid"`
	Auth       int    `json:"auth"` // 0 WPA2/3-PSK, 1 EAP-TLS, 2 PEAP, 3 EAP-TTLS
	Identity   string `json:"identity,omitempty"`
	User       string `json:"user,omitempty"`
	Pass       string `json:"pass,omitempty"` // PSK or EAP password; never shown again
	CACheck    bool   `json:"ca_check,omitempty"`
	DNS        string `json:"dns,omitempty"`
	CA         string `json:"ca,omitempty"`          // PEM of the RADIUS server's authority
	Client     []byte `json:"client,omitempty"`      // EAP-TLS: .p12 or PEM with the key
	ClientPass string `json:"client_pass,omitempty"` // of the .p12
}

const (
	MaxProfiles      = 200
	MaxProfileClient = 6000 // a .p12 or PEM: what one command can carry, base64 included
)

func cloneProfile(p *Profile) *Profile {
	c := *p
	c.Groups = append([]uint64(nil), p.Groups...)
	if p.Wifi != nil {
		w := *p.Wifi
		w.Client = append([]byte(nil), p.Wifi.Client...)
		c.Wifi = &w
	}
	return &c
}

// Profiles returns all profiles, by name.
func (s *Store) Profiles() []*Profile {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]*Profile, 0, len(s.d.Profiles))
	for _, p := range s.d.Profiles {
		out = append(out, cloneProfile(p))
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

func (s *Store) Profile(id uint64) (*Profile, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	p := s.d.Profiles[id]
	if p == nil {
		return nil, ErrNotFound
	}
	return cloneProfile(p), nil
}

// PutProfile stores a profile: a new one for in.ID 0, otherwise that one.
// Secrets left empty in an existing profile's Wi-Fi keep their old value.
func (s *Store) PutProfile(in *Profile, by string) (*Profile, error) {
	if in.Name == "" || len(in.Name) > 48 || len(in.Note) > 200 || in.UnattendedH < 0 || in.UnattendedH > 168 ||
		len(in.Region) > 48 || len(in.Country) > 48 || len(in.Site) > 48 {
		return nil, ErrArgs
	}
	if w := in.Wifi; w != nil && (w.SSID == "" || len(w.SSID) > 32 || w.Auth < 0 || w.Auth > 3 || len(w.Pass) > 64 ||
		len(w.Identity) > 64 || len(w.User) > 64 || len(w.CA) > 6000 || len(w.Client) > MaxProfileClient || len(w.DNS) > 15) {
		return nil, ErrArgs
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, g := range in.Groups {
		if s.d.Groups[g] == nil {
			return nil, ErrNotFound
		}
	}
	if s.d.Profiles == nil {
		s.d.Profiles = map[uint64]*Profile{}
	}
	p := s.d.Profiles[in.ID]
	if in.ID == 0 {
		if len(s.d.Profiles) >= MaxProfiles {
			return nil, ErrArgs
		}
		s.d.NextProfile++
		p = &Profile{ID: s.d.NextProfile}
		s.d.Profiles[p.ID] = p
		s.ch.meta = true
	} else if p == nil {
		return nil, ErrNotFound
	}
	id, old := p.ID, p.Wifi
	*p = *cloneProfile(in)
	p.ID = id
	if old != nil && p.Wifi != nil {
		if p.Wifi.Pass == "" {
			p.Wifi.Pass = old.Pass
		}
		if len(p.Wifi.Client) == 0 {
			p.Wifi.Client, p.Wifi.ClientPass = old.Client, old.ClientPass
		}
		if p.Wifi.CA == "" {
			p.Wifi.CA = old.CA
		}
	}
	p.Updated, p.By = s.now(), by
	s.ch.profile(p.ID)
	return cloneProfile(p), s.save()
}

func (s *Store) DeleteProfile(id uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.d.Profiles[id] == nil {
		return ErrNotFound
	}
	delete(s.d.Profiles, id)
	s.ch.profile(id)
	return s.save()
}

// DeviceProfile is the profile the device enrolled with, 0 for none, and
// whether it has been applied already.
func (s *Store) DeviceProfile(id string) (uint64, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return 0, false, ErrNotFound
	}
	return d.Profile, d.ProfileDone, nil
}

// ProfileApplied notes that the profile's commands are queued for the device.
func (s *Store) ProfileApplied(id string, profile uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := s.d.Devices[id]
	if d == nil {
		return ErrNotFound
	}
	d.Profile, d.ProfileDone = profile, true
	s.ch.device(id)
	return s.save()
}
