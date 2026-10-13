package server

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"rs232c2/internal/store"
)

func TestPlaceTemplatesAndPresence(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	var devs []deviceView
	list := func() deviceView {
		devs = nil
		e.admin("GET", "/admin/v1/devices", nil, &devs)
		return devs[0]
	}
	// where a device belongs
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/place", map[string]string{"region": " Nord ", "site": "Werk 2"}, nil); code != 200 {
		t.Fatalf("place: %d", code)
	}
	if d := list(); d.Region != "Nord" || d.Site != "Werk 2" {
		t.Fatalf("placed: %+v", d)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/place", map[string]string{"region": strings.Repeat("x", 49)}, nil); code != 400 {
		t.Fatalf("long region: %d", code)
	}
	// connected right now: only for a few polls after the last one
	c.Poll(nil, nil)
	if !list().Online {
		t.Fatal("not online after a poll")
	}
	e.now = e.now.Add(3 * time.Minute)
	if list().Online {
		t.Fatal("online three minutes after the last poll")
	}

	// templates: made, changed, sent to a device as a stored configuration, deleted
	var tpl store.Template
	if code := e.admin("POST", "/admin/v1/templates", map[string]string{"name": "Uplink", "folder": "Cisco", "text": "conf t\n"}, &tpl); code != 201 || tpl.ID == 0 || tpl.By != "token" {
		t.Fatalf("new template: %d %+v", code, tpl)
	}
	for _, bad := range []map[string]string{{"name": "", "text": "x"}, {"name": "a", "text": ""}, {"name": "a", "text": strings.Repeat("x", store.MaxTemplateText+1)}} {
		if code := e.admin("POST", "/admin/v1/templates", bad, nil); code != 400 {
			t.Fatalf("bad template %v: %d", bad["name"], code)
		}
	}
	url := "/admin/v1/templates/" + jsonNumber(tpl.ID)
	if code := e.admin("PUT", url, map[string]string{"name": "Uplink", "folder": "Cisco/Edge", "text": "conf t\nint g0/1\n"}, &tpl); code != 200 || tpl.Folder != "Cisco/Edge" {
		t.Fatalf("change: %d %+v", code, tpl)
	}
	var all []store.Template
	if e.admin("GET", "/admin/v1/templates", nil, &all); len(all) != 1 || all[0].Text != "conf t\nint g0/1\n" {
		t.Fatalf("list: %+v", all)
	}
	var cmd store.Command
	if code := e.admin("POST", url+"/send", map[string]string{"device": c.ID()}, &cmd); code != 201 || cmd.Type != "config_put" || cmd.By != "token" {
		t.Fatalf("send: %d %+v", code, cmd)
	}
	r, err := c.Poll(nil, nil)
	if err != nil || len(r.Cmds) != 1 {
		t.Fatalf("poll: %+v %v", r, err)
	}
	var args struct{ Name, Text string }
	json.Unmarshal(r.Cmds[0].Args, &args)
	if args.Name != "Uplink" || args.Text != "conf t\nint g0/1\n" {
		t.Fatalf("what the device got: %+v", args)
	}
	if code := e.admin("POST", url+"/send", map[string]string{"device": "nope"}, nil); code != 404 {
		t.Fatalf("send to nobody: %d", code)
	}
	if code := e.admin("DELETE", url, nil, nil); code != 200 {
		t.Fatalf("delete: %d", code)
	}
	if code := e.admin("DELETE", url, nil, nil); code != 404 {
		t.Fatalf("second delete: %d", code)
	}

	// a firmware package sorted into a folder stays there over a restart
	pkg, _, _, _ := testPackage(1000, "1.9.0", 1791900000)
	var info FirmwareInfo
	e.upload(pkg, &info)
	if code := e.admin("POST", "/admin/v1/firmware/"+info.ID+"/folder", map[string]string{"folder": "Freigegeben"}, nil); code != 200 {
		t.Fatalf("folder: %d", code)
	}
	if sh := newShelf(e.sv.cfg.FirmwareDir); sh.get(info.ID) == nil || sh.get(info.ID).info.Folder != "Freigegeben" {
		t.Fatal("folder not kept")
	}
}

func jsonNumber(n uint64) string { b, _ := json.Marshal(n); return string(b) }
