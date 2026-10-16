package server

import (
	"encoding/base64"
	"encoding/json"
	"testing"
	"time"
)

func TestBackupOnTheSDCard(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	other := e.enrolled()
	withCard := map[string]any{"ctl": true, "sd": map[string]any{"on": true}}

	// a device with a card and changes allowed gets its backup once a day, in the same answer
	r, err := c.Poll(withCard, nil)
	if err != nil || len(r.Cmds) != 1 || r.Cmds[0].Type != "sd_backup" {
		t.Fatalf("first poll: %v %+v", err, r)
	}
	var a struct{ Key string }
	json.Unmarshal(r.Cmds[0].Args, &a)
	if k, _ := base64.StdEncoding.DecodeString(a.Key); len(k) != 32 {
		t.Fatalf("key: %q", a.Key)
	}
	first := a.Key
	if r, _ := c.Poll(withCard, nil); len(r.Cmds) != 0 {
		t.Fatalf("a second backup within the day: %+v", r.Cmds)
	}
	// not without a card, not without the permission, not while lent out as a stick
	e.now = e.now.Add(25 * time.Hour)
	for _, st := range []map[string]any{{"ctl": true}, {"ctl": false, "sd": map[string]any{"on": true}}, {"ctl": true, "sd": map[string]any{"on": true, "stick": true}}} {
		if r, _ := other.Poll(st, nil); len(r.Cmds) != 0 {
			t.Fatalf("backup queued for %v", st)
		}
	}
	if r, _ := c.Poll(withCard, nil); len(r.Cmds) != 1 {
		t.Fatalf("no backup after a day")
	} else if json.Unmarshal(r.Cmds[0].Args, &a); a.Key != first {
		t.Fatal("the key changed")
	}

	// the key is not kept in the command once it is handed over
	var cmds []struct {
		Type string
		Args json.RawMessage
	}
	e.admin("GET", "/admin/v1/devices/"+c.ID()+"/commands", nil, &cmds)
	for _, x := range cmds {
		if x.Type == "sd_backup" && len(x.Args) > 0 && string(x.Args) != "null" {
			t.Fatalf("key still in the command: %s", x.Args)
		}
	}

	// by hand, and a restore with another device's key
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/backup", nil, nil); code != 200 {
		t.Fatalf("backup now: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+other.ID()+"/restore", map[string]string{"from": c.ID()}, nil); code != 200 {
		t.Fatalf("restore: HTTP %d", code)
	}
	r, _ = other.Poll(nil, nil)
	if len(r.Cmds) != 1 || r.Cmds[0].Type != "sd_restore" {
		t.Fatalf("restore command: %+v", r.Cmds)
	}
	json.Unmarshal(r.Cmds[0].Args, &a)
	if a.Key != first {
		t.Fatal("the restore does not carry the key of the device the backup is from")
	}
	if code := e.admin("POST", "/admin/v1/devices/"+other.ID()+"/restore", map[string]string{"from": "nope"}, nil); code != 404 {
		t.Fatalf("restore from an unknown device: HTTP %d", code)
	}
}
