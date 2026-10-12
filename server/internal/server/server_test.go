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
	st, err := store.OpenForTest(e.dir)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { e.sv.st.Close() })
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
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]any{"type": "ping"}, &cmd); code != 201 {
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
	e.sv.st.Close()
	st, err := store.OpenForTest(e.dir)
	if err != nil {
		t.Fatal(err)
	}
	sv2 := New(Config{AdminToken: adminToken}, st)
	e.sv = sv2
	e.ts.Config.Handler = sv2.Handler()
	if _, err := c.Poll(nil, nil); err != nil {
		t.Fatalf("after restart: %v", err)
	}
	if os.Getenv(store.TestDatabase) != "" {
		return
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

// Devices read exactly Content-Length bytes; the handshake answer is larger
// than what net/http would send unchunked by itself.
func TestBinaryAnswersStateTheirLength(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	hs, req, err := proto.NewClientHandshake(proto.KindSession, mustKey(t, c), c.State.ServerPub, [proto.TokenIDSize]byte{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	_ = hs
	for path, body := range map[string][]byte{"/v1/handshake": req, "/v1/server-key": nil} {
		var resp *http.Response
		if body == nil {
			resp, err = http.Get(e.ts.URL + path)
		} else {
			resp, err = http.Post(e.ts.URL+path, "application/octet-stream", bytes.NewReader(body))
		}
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != 200 || resp.ContentLength != int64(len(b)) || len(resp.TransferEncoding) != 0 {
			t.Fatalf("%s: HTTP %d, Content-Length %d, body %d bytes, transfer encoding %v", path, resp.StatusCode, resp.ContentLength, len(b), resp.TransferEncoding)
		}
	}
}

func mustKey(t *testing.T, c *device.Client) *proto.StaticKey {
	t.Helper()
	k, err := proto.ParseStatic(c.State.Key)
	if err != nil {
		t.Fatal(err)
	}
	return k
}

func TestCommandTypesAndArguments(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	post := func(body string) int {
		req, _ := http.NewRequest("POST", e.ts.URL+"/admin/v1/devices/"+c.ID()+"/commands", strings.NewReader(body))
		req.Header.Set("Authorization", "Bearer "+adminToken)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		return resp.StatusCode
	}
	for body, want := range map[string]int{
		`{"type":"status"}`:                                     201,
		`{"type":"settings","args":{}}`:                         201,
		`{"type":"configs"}`:                                    201,
		`{"type":"reboot"}`:                                     201,
		`{"type":"play","args":{"name":"Demo","port":1}}`:       201,
		`{"type":"reboot","args":{"force":true}}`:               400, // takes no arguments
		`{"type":"play"}`:                                       400,
		`{"type":"play","args":{"name":"","port":1}}`:           400,
		`{"type":"play","args":{"name":"Demo","port":9}}`:       400,
		`{"type":"play","args":{"name":"Demo","port":1,"x":1}}`: 400, // nothing the device does not expect
		`{"type":"shell","args":{"cmd":"reload"}}`:              400,
		`{"type":"play","args":{"name":"Demo","port":"1; rm"}}`: 400,
	} {
		if got := post(body); got != want {
			t.Errorf("%s: HTTP %d, want %d", body, got, want)
		}
	}
	// the device gets exactly what was accepted
	r, err := c.Poll(nil, nil)
	if err != nil || len(r.Cmds) != 5 {
		t.Fatalf("poll: %+v %v", r, err)
	}
}

func TestCommandsExpire(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	cmds := func() (out []store.Command) {
		e.admin("GET", "/admin/v1/devices/"+c.ID()+"/commands", nil, &out)
		return out
	}
	e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "ping"}, nil)
	// fetched, but the device never answers
	if r, err := c.Poll(nil, nil); err != nil || len(r.Cmds) != 1 {
		t.Fatalf("poll: %+v %v", r, err)
	}
	e.now = e.now.Add(store.AnswerTTL - time.Minute)
	if got := cmds()[0].State; got != store.CmdSent {
		t.Fatalf("before the limit: %s", got)
	}
	e.now = e.now.Add(2 * time.Minute)
	if got := cmds()[0].State; got != store.CmdExpired {
		t.Fatalf("after the limit: %s", got)
	}
	// a late answer changes nothing, and the command is not sent again
	r, err := c.Poll(nil, []device.Result{{ID: cmds()[0].ID, OK: true, Out: "late"}})
	if err != nil || len(r.Cmds) != 0 || cmds()[0].State != store.CmdExpired {
		t.Fatalf("late answer: %+v %v %s", r, err, cmds()[0].State)
	}
	// queued and never fetched
	e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "reboot"}, nil)
	e.now = e.now.Add(store.QueueTTL + time.Minute)
	if r, err := c.Poll(nil, nil); err != nil || len(r.Cmds) != 0 {
		t.Fatalf("an order from yesterday was delivered: %+v %v", r, err)
	}
	if got := cmds()[1].State; got != store.CmdExpired {
		t.Fatalf("queued command: %s", got)
	}
}

func TestWebInterface(t *testing.T) {
	e := newEnv(t)
	for path, want := range map[string]string{"/": "RS232-Leitstelle", "/ui/app.js": "use strict", "/ui/style.css": "--accent", "/ui/qrcode.js": "QR Code Generator", "/ui/xterm.js": "Terminal", "/ui/xterm.css": ".xterm", "/ui/xterm-csp.js": "csp-style", "/ui/addon-fit.js": "FitAddon"} {
		resp, err := http.Get(e.ts.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != 200 || !strings.Contains(string(b), want) {
			t.Errorf("%s: HTTP %d, %d bytes", path, resp.StatusCode, len(b))
		}
		if csp := resp.Header.Get("Content-Security-Policy"); !strings.Contains(csp, "script-src 'self'") || strings.Contains(csp, "unsafe") {
			t.Errorf("%s: Content-Security-Policy %q", path, csp)
		}
	}
	for _, path := range []string{"/ui/nope.js", "/ui/..%2fserver.go", "/index.html"} {
		resp, _ := http.Get(e.ts.URL + path)
		resp.Body.Close()
		if resp.StatusCode != 404 {
			t.Errorf("%s: HTTP %d", path, resp.StatusCode)
		}
	}
	// the page is public, the data behind it is not
	resp, _ := http.Get(e.ts.URL + "/admin/v1/server")
	resp.Body.Close()
	if resp.StatusCode != 401 {
		t.Fatalf("/admin/v1/server without token: HTTP %d", resp.StatusCode)
	}
	var info struct {
		Fingerprint string
		Commands    []string
	}
	if code := e.admin("GET", "/admin/v1/server", nil, &info); code != 200 || len(info.Fingerprint) != 64 || len(info.Commands) != 7 {
		t.Fatalf("server info: HTTP %d %+v", code, info)
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

func TestHealthAndStatus(t *testing.T) {
	e := newEnv(t)
	get := func(path string) (int, string) {
		resp, err := http.Get(e.ts.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, strings.TrimSpace(string(b))
	}
	if code, body := get("/healthz"); code != 200 || body != "ok" {
		t.Fatalf("healthz: %d %q", code, body)
	}
	if code, _ := get("/admin/v1/status"); code != 401 {
		t.Fatalf("status without sign-in: %d", code)
	}

	seen := e.enrolled()
	if _, err := seen.Poll(nil, nil); err != nil {
		t.Fatal(err)
	}
	e.enrolled() // approved, never came back
	pending := e.device()
	if _, err := pending.Enroll(e.token(), "wartet", nil); err != nil {
		t.Fatal(err)
	}
	var st struct {
		UptimeS int `json:"uptime_s"`
		State   struct {
			OK    bool
			Error string
			Since time.Time
		}
		Devices      map[string]int
		SilentAfterS int              `json:"silent_after_s"`
		ConsolesOpen int              `json:"consoles_open"`
		TurnedAway   map[string]int64 `json:"turned_away"`
	}
	status := func() {
		t.Helper()
		st.State.Error = ""
		if code := e.admin("GET", "/admin/v1/status", nil, &st); code != 200 {
			t.Fatalf("status: HTTP %d", code)
		}
	}
	status()
	if !st.State.OK || st.Devices["active"] != 2 || st.Devices["pending"] != 1 || st.Devices["revoked"] != 0 || st.Devices["silent"] != 1 || st.SilentAfterS != 300 {
		t.Fatalf("status: %+v", st)
	}
	e.now = e.now.Add(6 * time.Minute)
	if status(); st.Devices["silent"] != 2 {
		t.Fatalf("after six minutes of silence: %+v", st.Devices)
	}

	// requests that were turned away are counted
	for i := 0; i < 40; i++ {
		get("/admin/v1/devices")
	}
	if status(); st.TurnedAway["admin"] == 0 || st.TurnedAway["handshake"] != 0 {
		t.Fatalf("turned away: %+v", st.TurnedAway)
	}

	if os.Getenv(store.TestDatabase) != "" {
		return // the rest needs a backend that a test can take away
	}
	if err := os.RemoveAll(e.dir); err != nil {
		t.Fatal(err)
	}
	if code, _ := get("/healthz"); code != 200 {
		t.Fatalf("healthz asks the backend again within %v: %d", healthEvery, code)
	}
	e.sv.healthAt = time.Time{}
	if code, body := get("/healthz"); code != 503 || body != "failing" {
		t.Fatalf("healthz without a data directory: %d %q", code, body)
	}
	if status(); st.State.OK || st.State.Error == "" || st.State.Since.IsZero() {
		t.Fatalf("status without a data directory: %+v", st.State)
	}
	if strings.Contains(st.State.Error, "\n") {
		t.Fatalf("error: %q", st.State.Error)
	}
	if err := os.Mkdir(e.dir, 0o700); err != nil {
		t.Fatal(err)
	}
	e.sv.healthAt = time.Time{}
	if code, _ := get("/healthz"); code != 200 {
		t.Fatalf("healthz after the repair: %d", code)
	}
	if status(); !st.State.OK {
		t.Fatalf("status after the repair: %+v", st.State)
	}
}

func TestKeyChange(t *testing.T) {
	e := newEnv(t)
	type info struct {
		Fingerprint string
		KeyChange   *struct {
			OldFingerprint string    `json:"old_fingerprint"`
			OldUntil       time.Time `json:"old_until"`
			DevicesWaiting int       `json:"devices_waiting"`
		} `json:"key_change"`
	}
	var before, during, after info
	e.admin("GET", "/admin/v1/server", nil, &before)
	if before.KeyChange != nil {
		t.Fatalf("a key change before there was one: %+v", before.KeyChange)
	}
	a, b, away := e.enrolled(), e.enrolled(), e.enrolled()
	for _, c := range []*device.Client{a, b, away} {
		if _, err := c.Poll(nil, nil); err != nil {
			t.Fatal(err)
		}
	}
	openToken := e.token()

	if code := e.admin("POST", "/admin/v1/server/rotate-key", map[string]int{"days": 400}, nil); code != 400 {
		t.Fatalf("400 days: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/server/rotate-key/end", nil, nil); code != 409 {
		t.Fatalf("end without a change: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/server/rotate-key", map[string]int{"days": 7}, &during); code != 200 {
		t.Fatalf("rotate: HTTP %d", code)
	}
	if during.Fingerprint == before.Fingerprint || during.KeyChange == nil || during.KeyChange.OldFingerprint != before.Fingerprint ||
		during.KeyChange.DevicesWaiting != 3 || !during.KeyChange.OldUntil.Equal(e.now.Add(7*24*time.Hour)) {
		t.Fatalf("after rotate: %+v %+v", during, during.KeyChange)
	}
	if code := e.admin("POST", "/admin/v1/server/rotate-key", nil, nil); code != 409 {
		t.Fatalf("a second change on top: HTTP %d", code)
	}
	// the key is what /v1/server-key hands out, and what new invitations name
	if !bytes.Equal(mustGet(t, e.ts.URL+"/v1/server-key"), e.sv.st.ServerKey().Public()) {
		t.Fatal("server-key is not the new key")
	}
	if _, err := e.device().Enroll(openToken, "zu spaet", nil); err == nil {
		t.Fatal("an invitation from before the change still works")
	}
	fresh := e.enrolled()
	if fresh.State.NextPub != nil || !bytes.Equal(fresh.State.ServerPub, e.sv.st.ServerKey().Public()) {
		t.Fatal("a device enrolled after the change is not on the new key")
	}

	// First poll: the old session is gone, the device shakes hands with the
	// key it knows and is told the new one.
	old := a.State.ServerPub
	r, err := a.Poll(nil, nil)
	if err != nil || r.State != "active" {
		t.Fatalf("poll after the change: %v %+v", err, r)
	}
	if !bytes.Equal(a.State.NextPub, e.sv.st.ServerKey().Public()) || !bytes.Equal(a.State.ServerPub, old) {
		t.Fatal("the new key was not announced, or pinned too early")
	}
	if !e.old(a.ID()) {
		t.Fatal("the server takes the device for changed before it said so")
	}
	// Second poll: the device says that it has the key.
	if r, err = a.Poll(nil, nil); err != nil || len(r.Rekey) != 0 {
		t.Fatalf("poll with the confirmation: %v", err)
	}
	if e.old(a.ID()) {
		t.Fatal("the confirmation did not count")
	}
	// The session goes on. The next handshake is answered with the new key:
	// the pinned one fails, the announced one works and is pinned.
	e.now = e.now.Add(2 * time.Hour)
	if r, err = a.Poll(nil, nil); err != nil || r.State != "active" {
		t.Fatalf("poll on the new key: %v", err)
	}
	if a.State.NextPub != nil || !bytes.Equal(a.State.ServerPub, e.sv.st.ServerKey().Public()) {
		t.Fatal("the new key was not pinned")
	}
	// commands still arrive, old device or new
	var cmd struct{ ID uint64 }
	e.admin("POST", "/admin/v1/devices/"+a.ID()+"/commands", map[string]string{"type": "ping"}, &cmd)
	if r, err = a.Poll(nil, nil); err != nil || len(r.Cmds) != 1 || r.Cmds[0].ID != cmd.ID {
		t.Fatalf("command after the change: %v %+v", err, r)
	}

	// b hears of the key, confirms, and the answer to that is lost: the server
	// counts it as changed, the device finds out with its next handshake.
	if _, err := b.Poll(nil, nil); err != nil {
		t.Fatal(err)
	}
	if err := e.sv.st.ConfirmKey(b.ID(), b.KeyTag()); err != nil || e.old(b.ID()) {
		t.Fatalf("confirm: %v", err)
	}
	e.sv.drop(sessionOf(e, b.ID()))
	if r, err = b.Poll(nil, nil); err != nil || r.State != "active" || b.State.NextPub != nil {
		t.Fatalf("poll after a lost answer: %v", err)
	}

	e.admin("GET", "/admin/v1/server", nil, &during)
	if during.KeyChange == nil || during.KeyChange.DevicesWaiting != 1 {
		t.Fatalf("one device should be waiting: %+v", during.KeyChange)
	}
	var st struct{ Devices map[string]int }
	e.admin("GET", "/admin/v1/status", nil, &st)
	if st.Devices["old_key"] != 1 {
		t.Fatalf("status: %+v", st.Devices)
	}

	// The time is up: the old key is forgotten, and who was away is out.
	e.now = e.now.Add(8 * 24 * time.Hour)
	if err := e.sv.st.Check(); err != nil {
		t.Fatal(err)
	}
	e.admin("GET", "/admin/v1/server", nil, &after)
	if after.KeyChange != nil || after.Fingerprint != during.Fingerprint {
		t.Fatalf("after the end: %+v", after.KeyChange)
	}
	if _, err := away.Poll(nil, nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("a device that missed the change: %v", err)
	}
	if !e.old(away.ID()) {
		t.Fatal("the device that missed the change is not marked")
	}
	// it comes back the way it came the first time
	sas, err := away.Enroll(e.token(), "wieder da", nil)
	if err != nil {
		t.Fatal(err)
	}
	if code := e.admin("POST", "/admin/v1/devices/"+away.ID()+"/approve", map[string]string{"sas": sas}, nil); code != 200 {
		t.Fatalf("approve: HTTP %d", code)
	}
	if r, err = away.Poll(nil, nil); err != nil || r.State != "active" || e.old(away.ID()) {
		t.Fatalf("after enrolling again: %v", err)
	}
	for _, c := range []*device.Client{a, b, fresh} {
		if r, err = c.Poll(nil, nil); err != nil || r.State != "active" {
			t.Fatalf("after the end of the change: %v", err)
		}
	}
	// and a second change can begin
	if code := e.admin("POST", "/admin/v1/server/rotate-key", nil, nil); code != 200 {
		t.Fatalf("second change: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/server/rotate-key/end", nil, &after); code != 200 || after.KeyChange != nil {
		t.Fatalf("end: HTTP %d", code)
	}
	if _, err := a.Poll(nil, nil); !errors.Is(err, device.ErrRefused) {
		t.Fatalf("after a change that was ended at once: %v", err)
	}
}

// old says whether the server still takes a device to be on the previous key.
func (e *env) old(id string) bool {
	e.t.Helper()
	var list []deviceView
	e.admin("GET", "/admin/v1/devices", nil, &list)
	for _, d := range list {
		if d.ID == id {
			return d.OldKey
		}
	}
	e.t.Fatalf("no device %s", id)
	return false
}

func sessionOf(e *env, deviceID string) (id [proto.IDSize]byte) {
	e.sv.mu.Lock()
	defer e.sv.mu.Unlock()
	for sid, s := range e.sv.sess {
		if s.deviceID == deviceID {
			return sid
		}
	}
	e.t.Fatalf("no session of %s", deviceID)
	return id
}

func mustGet(t *testing.T, url string) []byte {
	t.Helper()
	resp, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return b
}
