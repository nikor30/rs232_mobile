package server

import (
	"testing"
	"time"

	"rs232c2/internal/store"
)

func TestOpenRegistration(t *testing.T) {
	e := newEnv(t)
	open := func(min int) int {
		return e.admin("POST", "/admin/v1/enrollment/open", map[string]int{"minutes": min}, nil)
	}
	left := func() int {
		var out struct {
			Open int `json:"open_s"`
		}
		e.admin("GET", "/admin/v1/server", nil, &out)
		return out.Open
	}

	// closed unless somebody opens it
	c := e.device()
	if _, err := c.EnrollOpen(e.ts.URL, "nobody asked", nil); err == nil {
		t.Fatal("enrolled without invitation while registration was closed")
	}
	if e.state(c.ID()) != "" {
		t.Fatal("a refused device is in the list")
	}
	for _, min := range []int{-15, 10, 20, 195, 1000} {
		if code := open(min); code != 400 {
			t.Fatalf("%d minutes: HTTP %d, want 400", min, code)
		}
	}
	if left() != 0 {
		t.Fatal("refused values opened registration")
	}

	// open: the device comes in as pending and needs its code like any other
	if code := open(15); code != 200 {
		t.Fatalf("open: HTTP %d", code)
	}
	if left() != 900 {
		t.Fatalf("left %d s, want 900", left())
	}
	sas, err := c.EnrollOpen(e.ts.URL, "bench 4", nil)
	if err != nil {
		t.Fatal(err)
	}
	if e.state(c.ID()) != store.StatePending {
		t.Fatalf("state %q, want pending", e.state(c.ID()))
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": "000000"}, nil); code == 200 && sas != "000000" {
		t.Fatal("approved with a wrong code")
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 200 {
		t.Fatalf("approve: HTTP %d", code)
	}
	if _, err := c.Poll(nil, nil); err != nil {
		t.Fatalf("poll after approval: %v", err)
	}

	// a device that is in cannot be pushed back to pending this way
	if _, err := c.EnrollOpen(e.ts.URL, "again", nil); err == nil {
		t.Fatal("an active device enrolled again without invitation")
	}
	if e.state(c.ID()) != store.StateActive {
		t.Fatalf("state %q after the second try, want active", e.state(c.ID()))
	}

	// it closes by itself, and on request
	e.now = e.now.Add(14 * time.Minute)
	d2 := e.device()
	if _, err := d2.EnrollOpen(e.ts.URL, "in time", nil); err != nil {
		t.Fatal(err)
	}
	e.now = e.now.Add(2 * time.Minute)
	if left() != 0 {
		t.Fatalf("still open after 16 minutes: %d s", left())
	}
	if _, err := e.device().EnrollOpen(e.ts.URL, "late", nil); err == nil {
		t.Fatal("enrolled after registration had closed")
	}
	open(180)
	if left() != 3*3600 {
		t.Fatalf("left %d s, want 3 h", left())
	}
	open(0)
	if _, err := e.device().EnrollOpen(e.ts.URL, "closed", nil); err == nil {
		t.Fatal("enrolled after registration was closed by hand")
	}

	// no more than MaxOpenPending wait at a time (one is waiting already)
	open(60)
	for i := 1; i < store.MaxOpenPending; i++ {
		e.now = e.now.Add(time.Minute) // the rate limit counts handshakes per address
		if _, err := e.device().EnrollOpen(e.ts.URL, "filler", nil); err != nil {
			t.Fatalf("device %d: %v", i, err)
		}
	}
	e.now = e.now.Add(time.Minute)
	if _, err := e.device().EnrollOpen(e.ts.URL, "one too many", nil); err == nil {
		t.Fatal("more devices are waiting than allowed")
	}
	// an invitation still works then
	e.now = e.now.Add(time.Minute)
	if _, err := e.device().Enroll(e.token(), "invited", nil); err != nil {
		t.Fatalf("invited device: %v", err)
	}
}
