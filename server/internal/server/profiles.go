package server

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"rs232c2/internal/proto"
	"rs232c2/internal/store"
)

// Profiles for new devices (store/profiles.go): kept here, named in an
// invitation, applied when the device is approved - or later, by hand.

// profileView is a profile as the page sees it: the secrets only as "set".
type profileView struct {
	*store.Profile
	Wifi *wifiView `json:"wifi,omitempty"`
}

type wifiView struct {
	store.ProfileWifi
	Pass       string `json:"pass,omitempty"`
	Client     []byte `json:"client,omitempty"`
	ClientPass string `json:"client_pass,omitempty"`
	PassSet    bool   `json:"pass_set"`
	ClientSet  bool   `json:"client_set"`
	CASet      bool   `json:"ca_set"`
}

func viewProfile(p *store.Profile) profileView {
	v := profileView{Profile: p}
	if w := p.Wifi; w != nil {
		v.Wifi = &wifiView{ProfileWifi: *w, PassSet: w.Pass != "", ClientSet: len(w.Client) > 0, CASet: w.CA != ""}
		v.Wifi.ProfileWifi.CA = ""
	}
	return v
}

func tokenProfile(p *store.Profile) *proto.TokenProfile {
	t := &proto.TokenProfile{Name: p.Name, Control: p.Control, Unattended: p.UnattendedH}
	if p.Wifi != nil {
		t.SSID, t.Auth = p.Wifi.SSID, p.Wifi.Auth
	}
	return t
}

func (sv *Server) listProfiles(w http.ResponseWriter, _ *http.Request) {
	out := []profileView{}
	for _, p := range sv.st.Profiles() {
		out = append(out, viewProfile(p))
	}
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) putProfile(w http.ResponseWriter, r *http.Request) {
	in := &store.Profile{}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	in.ID = 0
	if v := r.PathValue("id"); v != "" {
		if in.ID, _ = strconv.ParseUint(v, 10, 64); in.ID == 0 {
			fail(w, store.ErrNotFound)
			return
		}
	}
	in.Name, in.Note, in.Region, in.Country, in.Site = clean(in.Name), clean(in.Note), clean(in.Region), clean(in.Country), clean(in.Site)
	if in.Wifi != nil && clean(in.Wifi.SSID) == "" {
		in.Wifi = nil // no Wi-Fi in this profile
	}
	note(r).target = in.Name
	p, err := sv.st.PutProfile(in, who(r).Name)
	if err == store.ErrArgs {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "a profile needs a name (up to 48 characters); unattended access 0 to 168 hours; a Wi-Fi needs its SSID, and a certificate file may have 6000 bytes at most"})
		return
	} else if err != nil {
		fail(w, err)
		return
	}
	code := http.StatusOK
	if r.PathValue("id") == "" {
		code = http.StatusCreated
	}
	writeJSON(w, code, viewProfile(p))
}

func (sv *Server) deleteProfile(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err := sv.st.DeleteProfile(id); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// profileDevice applies a profile to a device that is enrolled already.
// Without the permission at the device the commands are refused there; the
// unattended access it asks for is not granted this way at all.
func (sv *Server) profileDevice(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Profile uint64 `json:"profile"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil || in.Profile == 0 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	if err := sv.applyProfile(r.PathValue("id"), in.Profile, who(r).Name); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// applyProfile sets what the server keeps about the device (place, groups)
// and queues what the device has to do: certificates first, then the Wi-Fi -
// "set" restarts the device, so it comes last.
func (sv *Server) applyProfile(id string, profile uint64, by string) error {
	p, err := sv.st.Profile(profile)
	if err != nil {
		return err
	}
	if p.Region != "" || p.Country != "" || p.Site != "" {
		if err := sv.st.Place(id, p.Region, p.Country, p.Site); err != nil {
			return err
		}
	}
	if len(p.Groups) > 0 {
		if err := sv.st.SetGroups(id, p.Groups); err != nil {
			return err
		}
	}
	if wf := p.Wifi; wf != nil {
		if wf.CA != "" {
			if err := sv.queue(id, "cert_put", map[string]string{"slot": "ca", "data": base64.StdEncoding.EncodeToString([]byte(wf.CA))}, by); err != nil {
				return err
			}
		}
		if wf.Auth == 1 {
			if len(wf.Client) == 0 {
				return errors.New("the profile has no client certificate for EAP-TLS")
			}
			if err := sv.queue(id, "cert_put", map[string]string{"slot": "client", "data": base64.StdEncoding.EncodeToString(wf.Client), "pass": wf.ClientPass}, by); err != nil {
				return err
			}
		}
		set := map[string]any{"staSsid": wf.SSID, "staAuth": wf.Auth, "staIdentity": wf.Identity, "staUser": wf.User,
			"staCaCheck": wf.CACheck && wf.CA != "", "staDns": wf.DNS}
		if wf.Pass != "" {
			set["staPass"] = wf.Pass
		}
		if err := sv.queue(id, "set", set, by); err != nil {
			return err
		}
	}
	return sv.st.ProfileApplied(id, profile)
}

func (sv *Server) queue(id, typ string, args any, by string) error {
	b, _ := json.Marshal(args)
	_, err := sv.st.EnqueueBy(id, typ, b, by)
	return err
}
