package server

import (
	"encoding/base64"
	"encoding/json"
	"strconv"
	"strings"
	"testing"

	"rs232c2/internal/proto"
	"rs232c2/internal/store"
)

func TestProfileForNewDevices(t *testing.T) {
	e := newEnv(t)
	var g store.Group
	if code := e.admin("POST", "/admin/v1/groups", map[string]any{"name": "Kunde A"}, &g); code != 201 {
		t.Fatalf("group: HTTP %d", code)
	}
	in := map[string]any{
		"name": "Kunde A, Lager", "region": "EMEA", "country": "Germany", "site": "Lager 3", "groups": []uint64{g.ID},
		"control": true, "unattended_h": 24,
		"wifi": map[string]any{"ssid": "corp-dot1x", "auth": 1, "ca_check": true, "ca": "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
			"client": []byte("p12-bytes"), "client_pass": "geheim"},
	}
	var p struct {
		ID   uint64 `json:"id"`
		Wifi struct {
			Pass       string `json:"pass"`
			Client     []byte `json:"client"`
			ClientPass string `json:"client_pass"`
			CA         string `json:"ca"`
			ClientSet  bool   `json:"client_set"`
			CASet      bool   `json:"ca_set"`
		} `json:"wifi"`
	}
	if code := e.admin("POST", "/admin/v1/profiles", in, &p); code != 201 || p.ID == 0 {
		t.Fatalf("profile: HTTP %d %+v", code, p)
	}
	if len(p.Wifi.Client) != 0 || p.Wifi.ClientPass != "" || p.Wifi.CA != "" || !p.Wifi.ClientSet || !p.Wifi.CASet {
		t.Fatalf("secrets came back, or their marks are missing: %+v", p.Wifi)
	}
	if code := e.admin("POST", "/admin/v1/profiles", map[string]any{"name": "x", "unattended_h": 200}, nil); code != 400 {
		t.Fatalf("unattended 200 h: HTTP %d", code)
	}

	// the invitation carries the summary the device shows before enrolling
	var tok struct{ Token string }
	if code := e.admin("POST", "/admin/v1/enroll-tokens", map[string]any{"ttl_s": 600, "profile": p.ID}, &tok); code != 201 {
		t.Fatalf("token: HTTP %d", code)
	}
	b, _ := base64.RawURLEncoding.DecodeString(strings.TrimPrefix(tok.Token, "c2e1:"))
	var et proto.EnrollToken
	json.Unmarshal(b, &et)
	if et.Profile == nil || et.Profile.Name != "Kunde A, Lager" || !et.Profile.Control || et.Profile.Unattended != 24 || et.Profile.SSID != "corp-dot1x" || et.Profile.Auth != 1 {
		t.Fatalf("summary in the token: %+v", et.Profile)
	}
	if code := e.admin("POST", "/admin/v1/enroll-tokens", map[string]any{"profile": 999}, nil); code != 404 {
		t.Fatalf("token for a profile that does not exist: HTTP %d", code)
	}

	// enrolled with it and approved: place, group and the commands are there
	c := e.device()
	sas, err := c.Enroll(tok.Token, "neu", nil)
	if err != nil {
		t.Fatal(err)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 200 {
		t.Fatalf("approve: HTTP %d", code)
	}
	var list []deviceView
	e.admin("GET", "/admin/v1/devices", nil, &list)
	for _, d := range list {
		if d.ID == c.ID() && (d.Site != "Lager 3" || d.Country != "Germany" || len(d.Groups) != 1) {
			t.Fatalf("place or group not set: %+v", d)
		}
	}
	var cmds []store.Command
	e.admin("GET", "/admin/v1/devices/"+c.ID()+"/commands", nil, &cmds)
	var types []string
	for _, c := range cmds {
		types = append(types, c.Type)
	}
	if strings.Join(types, ",") != "cert_put,cert_put,set" {
		t.Fatalf("queued: %v", types)
	}
	var set map[string]any
	for _, c := range cmds {
		if c.Type == "set" {
			json.Unmarshal(c.Args, &set)
		}
	}
	if set["staSsid"] != "corp-dot1x" || set["staAuth"] != float64(1) || set["staCaCheck"] != true {
		t.Fatalf("set: %v", set)
	}
	// approving again does not queue it a second time
	r, err := c.Poll(nil, nil)
	if err != nil || len(r.Cmds) != 3 {
		t.Fatalf("the device gets the three commands: %v %+v", err, r)
	}

	// editing keeps secrets that are not sent again
	in["note"] = "zweite Fassung"
	in["wifi"].(map[string]any)["client"] = nil
	in["wifi"].(map[string]any)["ca"] = ""
	if code := e.admin("PUT", "/admin/v1/profiles/"+itoa(p.ID), in, &p); code != 200 || !p.Wifi.ClientSet || !p.Wifi.CASet {
		t.Fatalf("edit: HTTP %d %+v", code, p.Wifi)
	}
	if code := e.admin("DELETE", "/admin/v1/profiles/"+itoa(p.ID), nil, nil); code != 200 {
		t.Fatalf("delete: HTTP %d", code)
	}
}

func itoa(v uint64) string { return strconv.FormatUint(v, 10) }
