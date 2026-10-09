package server

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"rs232c2/internal/device"
	"rs232c2/internal/proto"
	"rs232c2/internal/store"
)

const adminToken = "test-admin-token"

type env struct {
	t   *testing.T
	sv  *Server
	ts  *httptest.Server
	dir string
	now time.Time
}

func newEnv(t *testing.T) *env {
	e := &env{t: t, dir: t.TempDir(), now: time.Date(2026, 10, 9, 12, 0, 0, 0, time.UTC)}
	st, err := store.Open(e.dir)
	if err != nil {
		t.Fatal(err)
	}
	e.sv = New(Config{AdminToken: adminToken}, st)
	e.sv.SetClock(func() time.Time { return e.now })
	e.ts = httptest.NewServer(e.sv.Handler())
	e.sv.cfg.PublicURL = e.ts.URL
	t.Cleanup(e.ts.Close)
	return e
}

// admin calls the admin API and decodes the answer into out (if not nil).
func (e *env) admin(method, path string, in, out any) int {
	e.t.Helper()
	var body io.Reader
	if in != nil {
		b, _ := json.Marshal(in)
		body = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, e.ts.URL+path, body)
	req.Header.Set("Authorization", "Bearer "+adminToken)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	if out != nil {
		if err := json.NewDecoder(resp.Body).Decode(out); err != nil {
			e.t.Fatalf("%s %s: %v", method, path, err)
		}
	}
	return resp.StatusCode
}

func (e *env) token() string {
	e.t.Helper()
	var out struct{ Token string }
	if code := e.admin("POST", "/admin/v1/enroll-tokens", map[string]any{"ttl_s": 600}, &out); code != 201 {
		e.t.Fatalf("token: HTTP %d", code)
	}
	return out.Token
}

func (e *env) device() *device.Client {
	e.t.Helper()
	c, err := device.New(http.DefaultClient, device.State{})
	if err != nil {
		e.t.Fatal(err)
	}
	return c
}

func (e *env) state(id string) string {
	e.t.Helper()
	var list []deviceView
	e.admin("GET", "/admin/v1/devices", nil, &list)
	for _, d := range list {
		if d.ID == id {
			return d.State
		}
	}
	return ""
}

// enrolled returns a device that has been enrolled and approved.
func (e *env) enrolled() *device.Client {
	e.t.Helper()
	c := e.device()
	sas, err := c.Enroll(e.token(), "test", nil)
	if err != nil {
		e.t.Fatal(err)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 200 {
		e.t.Fatalf("approve: HTTP %d", code)
	}
	return c
}

func TestEnrollApproveCommandRoundTrip(t *testing.T) {
	e := newEnv(t)
	c := e.device()
	sas, err := c.Enroll(e.token(), "Labor-1", map[string]string{"fw": "1.8.0"})
	if err != nil {
		t.Fatal(err)
	}
	if e.state(c.ID()) != store.StatePending {
		t.Fatalf("state %q after enrolling", e.state(c.ID()))
	}

	// pending: the device may poll, but gets nothing and cannot be commanded
	r, err := c.Poll(nil, nil)
	if err != nil || r.State != store.StatePending || len(r.Cmds) != 0 {
		t.Fatalf("pending poll: %+v %v", r, err)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "ping"}, nil); code != 409 {
		t.Fatalf("command for a pending device: HTTP %d", code)
	}

	// the admin API must not reveal the code
	raw, _ := json.Marshal(e.sv.st.Devices())
	if !strings.Contains(string(raw), sas) {
		t.Fatal("test is wrong: the store should hold the code")
	}
	req, _ := http.NewRequest("GET", e.ts.URL+"/admin/v1/devices", nil)
	req.Header.Set("Authorization", "Bearer "+adminToken)
	resp, _ := http.DefaultClient.Do(req)
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if strings.Contains(string(body), sas) || strings.Contains(string(body), "sas") {
		t.Fatalf("device list leaks the code: %s", body)
	}

	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 200 {
		t.Fatalf("approve: HTTP %d", code)
	}

	var cmd store.Command
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]any{"type": "ping", "args": map[string]int{"n": 1}}, &cmd); code != 201 {
		t.Fatalf("enqueue: HTTP %d", code)
	}
	r, err = c.Poll(map[string]int{"uptime": 5}, nil)
	if err != nil || r.State != store.StateActive || len(r.Cmds) != 1 || r.Cmds[0].ID != cmd.ID || r.Cmds[0].Type != "ping" {
		t.Fatalf("poll: %+v %v", r, err)
	}
	// delivered once only
	r, err = c.Poll(nil, []device.Result{{ID: cmd.ID, OK: true, Out: "pong"}})
	if err != nil || len(r.Cmds) != 0 {
		t.Fatalf("second poll: %+v %v", r, err)
	}
	var cmds []store.Command
	e.admin("GET", "/admin/v1/devices/"+c.ID()+"/commands", nil, &cmds)
	if len(cmds) != 1 || cmds[0].State != store.CmdDone || string(cmds[0].Result) != `"pong"` {
		t.Fatalf("commands: %+v", cmds)
	}
}

func TestWrongCodeAndLockout(t *testing.T) {
	e := newEnv(t)
	c := e.device()
	sas, err := c.Enroll(e.token(), "x", nil)
	if err != nil {
		t.Fatal(err)
	}
	wrong := "000000"
	if sas == wrong {
		wrong = "000001"
	}
	for i := 1; i <= store.MaxSASAttempts; i++ {
		if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": wrong}, nil); code != 403 {
			t.Fatalf("wrong code %d: HTTP %d", i, code)
		}
	}
	// the enrollment is gone; even the right code no longer works
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 404 {
		t.Fatalf("after lockout: HTTP %d", code)
	}
	if _, err := c.Poll(nil, nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("poll after lockout: %v", err)
	}
}

func TestTokenIsOneTimeAndExpires(t *testing.T) {
	e := newEnv(t)
	tok := e.token()
	if _, err := e.device().Enroll(tok, "first", nil); err != nil {
		t.Fatal(err)
	}
	if _, err := e.device().Enroll(tok, "second", nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("token reused: %v", err)
	}
	tok = e.token()
	e.now = e.now.Add(11 * time.Minute)
	if _, err := e.device().Enroll(tok, "late", nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("expired token: %v", err)
	}
}

func TestEnrollRejectsWrongServer(t *testing.T) {
	// The token names server A, but the URL leads to server B.
	a, b := newEnv(t), newEnv(t)
	tok, err := proto.ParseEnrollToken(a.token())
	if err != nil {
		t.Fatal(err)
	}
	tok.URL = b.ts.URL
	if _, err := a.device().Enroll(tok.String(), "x", nil); !errors.Is(err, device.ErrServerKey) {
		t.Fatalf("got %v, want ErrServerKey", err)
	}
}

func TestEnrollWithForgedTokenSecret(t *testing.T) {
	e := newEnv(t)
	tok, _ := proto.ParseEnrollToken(e.token())
	tok.Secret = bytes.Repeat([]byte{7}, proto.PSKSize)
	c := e.device()
	if _, err := c.Enroll(tok.String(), "x", nil); !errors.Is(err, proto.ErrConfirm) {
		t.Fatalf("got %v, want ErrConfirm", err)
	}
	if e.state(c.ID()) != "" {
		t.Fatal("device was recorded without knowing the token secret")
	}
}

func TestRevoke(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	if _, err := c.Poll(nil, nil); err != nil {
		t.Fatal(err)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/revoke", nil, nil); code != 200 {
		t.Fatalf("revoke: HTTP %d", code)
	}
	// the open session is gone, and a new handshake is refused
	if _, err := c.Poll(nil, nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("poll after revoke: %v", err)
	}
	// a revoked device cannot come back with a new token
	if _, err := c.Enroll(e.token(), "again", nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("re-enroll after revoke: %v", err)
	}
}

func TestUnknownDeviceAndStrangerKey(t *testing.T) {
	e := newEnv(t)
	known := e.enrolled()
	// a device the server has never seen
	stranger := e.device()
	stranger.State.URL, stranger.State.ServerPub = known.State.URL, known.State.ServerPub
	if _, err := stranger.Poll(nil, nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("unknown device: %v", err)
	}
}

func TestSessionExpiryAndServerRestart(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	if _, err := c.Poll(nil, nil); err != nil {
		t.Fatal(err)
	}
	e.now = e.now.Add(sessionTTL + time.Minute)
	if _, err := c.Poll(nil, nil); err != nil { // shakes hands again by itself
		t.Fatalf("after session expiry: %v", err)
	}

	// restart: sessions are gone, devices and the server key are not
	st, err := store.Open(e.dir)
	if err != nil {
		t.Fatal(err)
	}
	sv2 := New(Config{AdminToken: adminToken}, st)
	e.ts.Config.Handler = sv2.Handler()
	if _, err := c.Poll(nil, nil); err != nil {
		t.Fatalf("after restart: %v", err)
	}
	if fi, err := os.Stat(filepath.Join(e.dir, "state.json")); err != nil || fi.Mode().Perm() != 0o600 {
		t.Fatalf("state file: %v %v", fi.Mode(), err)
	}
}

func TestAdminAuthAndInput(t *testing.T) {
	e := newEnv(t)
	for _, auth := range []string{"", "Bearer wrong", "Bearer " + adminToken + "x", adminToken} {
		req, _ := http.NewRequest("GET", e.ts.URL+"/admin/v1/devices", nil)
		if auth != "" {
			req.Header.Set("Authorization", auth)
		}
		resp, _ := http.DefaultClient.Do(req)
		resp.Body.Close()
		if resp.StatusCode != 401 {
			t.Fatalf("auth %q: HTTP %d", auth, resp.StatusCode)
		}
	}
	c := e.enrolled()
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "rm -rf"}, nil); code != 400 {
		t.Fatalf("unknown command type: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/devices/nope/commands", map[string]string{"type": "ping"}, nil); code != 404 {
		t.Fatalf("unknown device: HTTP %d", code)
	}
}

func TestDeviceEndpointsRejectGarbage(t *testing.T) {
	e := newEnv(t)
	post := func(path string, body []byte) int {
		resp, err := http.Post(e.ts.URL+path, "application/octet-stream", bytes.NewReader(body))
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		return resp.StatusCode
	}
	if code := post("/v1/handshake", []byte("hello")); code != 400 {
		t.Fatalf("short handshake: HTTP %d", code)
	}
	if code := post("/v1/handshake", make([]byte, 100000)); code != 413 {
		t.Fatalf("huge handshake: HTTP %d", code)
	}
	if code := post("/v1/msg", make([]byte, 64)); code != 401 {
		t.Fatalf("msg without session: HTTP %d", code)
	}
	if code := post("/v1/msg", nil); code != 400 {
		t.Fatalf("empty msg: HTTP %d", code)
	}
}
