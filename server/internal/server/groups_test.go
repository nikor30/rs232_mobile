package server

import (
	"testing"

	"rs232c2/internal/store"
)

func TestGroupsAndRights(t *testing.T) {
	e := newEnv(t)
	a, b := e.enrolled(), e.enrolled()
	secret := e.newUser("tech", "a long password 1")
	tech, code := e.login("tech", "a long password 1", e.code(secret))
	if code != 200 {
		t.Fatalf("login: %d", code)
	}
	var me struct {
		Name  string
		Admin bool
	}
	if tech.do("GET", "/admin/v1/me", nil, &me); me.Name != "tech" || !me.Admin {
		t.Fatalf("a new account is an administrator: %+v", me)
	}
	if code := e.admin("POST", "/admin/v1/users/tech/role", map[string]string{"role": "owner"}, nil); code != 400 {
		t.Fatalf("made-up role: %d", code)
	}
	if code := tech.do("POST", "/admin/v1/users/tech/role", map[string]string{"role": "member"}, nil); code != 409 {
		t.Fatalf("own role: %d", code)
	}
	if code := e.admin("POST", "/admin/v1/users/tech/role", map[string]string{"role": "member"}, nil); code != 200 {
		t.Fatalf("role: %d", code)
	}
	if tech.do("GET", "/admin/v1/me", nil, &me); me.Admin {
		t.Fatal("still an administrator")
	}

	// a member without a group sees no device and nothing of the server
	var devs []deviceView
	if tech.do("GET", "/admin/v1/devices", nil, &devs); len(devs) != 0 {
		t.Fatalf("sees %d devices", len(devs))
	}
	for _, path := range []string{"/admin/v1/users", "/admin/v1/audit", "/admin/v1/server", "/admin/v1/status"} {
		if code := tech.do("GET", path, nil, nil); code != 403 {
			t.Fatalf("%s: %d", path, code)
		}
	}
	for path, body := range map[string]any{"/admin/v1/enroll-tokens": nil, "/admin/v1/groups": map[string]string{"name": "x"},
		"/admin/v1/devices/" + a.ID() + "/revoke": nil, "/admin/v1/templates": map[string]string{"name": "x", "text": "y"}} {
		if code := tech.do("POST", path, body, nil); code != 403 {
			t.Fatalf("POST %s: %d", path, code)
		}
	}
	if code := tech.do("GET", "/admin/v1/devices/"+a.ID()+"/commands", nil, nil); code != 404 {
		t.Fatalf("a device of others: %d", code)
	}

	// groups
	var g, g2 groupView
	if code := e.admin("POST", "/admin/v1/groups", map[string]any{"name": " Werk Nord ", "members": map[string]string{"tech": "use", "nobody": "use"}}, &g); code != 201 || g.Name != "Werk Nord" || len(g.Members) != 1 {
		t.Fatalf("group: %d %+v", code, g)
	}
	if code := e.admin("POST", "/admin/v1/groups", map[string]any{"name": "werk nord"}, nil); code != 409 {
		t.Fatalf("same name: %d", code)
	}
	if code := e.admin("POST", "/admin/v1/groups", map[string]any{"name": "x", "members": map[string]string{"tech": "own"}}, nil); code != 400 {
		t.Fatalf("made-up right: %d", code)
	}
	e.admin("POST", "/admin/v1/groups", map[string]any{"name": "Labor"}, &g2)
	dev := func(id string) string { return "/admin/v1/devices/" + id }
	if code := e.admin("POST", dev(a.ID())+"/groups", map[string]any{"groups": []uint64{g.ID, 999}}, nil); code != 400 {
		t.Fatalf("unknown group: %d", code)
	}
	if code := e.admin("POST", dev(a.ID())+"/groups", map[string]any{"groups": []uint64{g2.ID, g.ID}}, nil); code != 200 {
		t.Fatalf("assign: %d", code)
	}
	if code := e.admin("POST", dev(a.ID())+"/place", map[string]string{"region": "EMEA", "country": "Deutschland", "site": "Werk 2"}, nil); code != 200 {
		t.Fatalf("place: %d", code)
	}

	// with "use": the one device, reading and the console, nothing that changes it
	if tech.do("GET", "/admin/v1/devices", nil, &devs); len(devs) != 1 || devs[0].ID != a.ID() || devs[0].Right != "use" || devs[0].Country != "Deutschland" || len(devs[0].Groups) != 2 {
		t.Fatalf("sees: %+v", devs)
	}
	var groups []groupView
	if tech.do("GET", "/admin/v1/groups", nil, &groups); len(groups) != 1 || groups[0].Mine != "use" || groups[0].Members != nil {
		t.Fatalf("groups of a member: %+v", groups)
	}
	cmd := func(typ string, args any) map[string]any { return map[string]any{"type": typ, "args": args} }
	if code := tech.do("POST", dev(a.ID())+"/commands", cmd("ping", nil), nil); code != 201 {
		t.Fatalf("ping: %d", code)
	}
	if code := tech.do("POST", dev(a.ID())+"/commands", cmd("console", map[string]int{"port": 1, "minutes": 5}), nil); code != 201 {
		t.Fatalf("console: %d", code)
	}
	if code := tech.do("POST", dev(a.ID())+"/commands", cmd("reboot", nil), nil); code != 403 {
		t.Fatalf("reboot with use: %d", code)
	}
	if code := tech.do("POST", dev(b.ID())+"/commands", cmd("ping", nil), nil); code != 404 {
		t.Fatalf("ping to a device of others: %d", code)
	}
	var tpl store.Template
	e.admin("POST", "/admin/v1/templates", map[string]string{"name": "Uplink", "text": "hostname x"}, &tpl)
	send := "/admin/v1/templates/1/send"
	if code := tech.do("POST", send, map[string]string{"device": a.ID()}, nil); code != 403 {
		t.Fatalf("template with use: %d", code)
	}

	// with "manage": also that, still only in its groups
	if code := e.admin("PUT", "/admin/v1/groups/1", map[string]any{"name": "Werk Nord", "members": map[string]string{"tech": "manage"}}, nil); code != 200 {
		t.Fatalf("change: %d", code)
	}
	if code := tech.do("POST", dev(a.ID())+"/commands", cmd("reboot", nil), nil); code != 201 {
		t.Fatalf("reboot with manage: %d", code)
	}
	if code := tech.do("POST", send, map[string]string{"device": a.ID()}, nil); code != 201 {
		t.Fatalf("template with manage: %d", code)
	}
	if code := tech.do("POST", send, map[string]string{"device": b.ID()}, nil); code != 403 {
		t.Fatalf("template to a device of others: %d", code)
	}
	if code := tech.do("POST", dev(a.ID())+"/place", map[string]string{"site": "x"}, nil); code != 403 {
		t.Fatalf("place by a member: %d", code)
	}

	// the group goes: the device is out of it, the member sees nothing again
	if code := e.admin("DELETE", "/admin/v1/groups/1", nil, nil); code != 200 {
		t.Fatalf("delete: %d", code)
	}
	if tech.do("GET", "/admin/v1/devices", nil, &devs); len(devs) != 0 {
		t.Fatalf("sees after the group went: %+v", devs)
	}
	devs = nil
	e.admin("GET", "/admin/v1/devices", nil, &devs)
	for _, d := range devs {
		if d.ID == a.ID() && (len(d.Groups) != 1 || d.Groups[0] != g2.ID) {
			t.Fatalf("groups left: %v", d.Groups)
		}
	}
	// a deleted account leaves its groups
	e.admin("PUT", "/admin/v1/groups/2", map[string]any{"name": "Labor", "members": map[string]string{"tech": "use"}}, nil)
	e.admin("DELETE", "/admin/v1/users/tech", nil, nil)
	groups = nil
	if e.admin("GET", "/admin/v1/groups", nil, &groups); len(groups) != 1 || len(groups[0].Members) != 0 {
		t.Fatalf("after deleting the account: %+v", groups)
	}
}
