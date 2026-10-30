package server

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"rs232c2/internal/store"
)

func init() { passIter = 1000 } // the real number costs half a second per try

// browser is a client with a cookie, as the web interface is one.
type browser struct {
	e      *env
	cookie *http.Cookie
	plain  bool // leave out the X-C2 header, as a request set off by another site would
}

func (b *browser) do(method, path string, in, out any) int {
	b.e.t.Helper()
	var body io.Reader
	if in != nil {
		j, _ := json.Marshal(in)
		body = bytes.NewReader(j)
	}
	req, _ := http.NewRequest(method, b.e.ts.URL+path, body)
	if b.cookie != nil {
		req.AddCookie(b.cookie)
	}
	if !b.plain {
		req.Header.Set("X-C2", "1")
	}
	hc := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	resp, err := hc.Do(req)
	if err != nil {
		b.e.t.Fatal(err)
	}
	defer resp.Body.Close()
	for _, c := range resp.Cookies() {
		if c.Name == b.e.sv.cookieName() {
			b.cookie = c
			if c.MaxAge < 0 {
				b.cookie = nil
			}
		}
	}
	if out != nil {
		json.NewDecoder(resp.Body).Decode(out)
	}
	return resp.StatusCode
}

// newUser creates a local account through the admin token and sets it up, as
// its owner would. It returns the authenticator's secret.
func (e *env) newUser(name, password string) []byte {
	e.t.Helper()
	var made struct{ Setup string }
	if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": name, "kind": "local"}, &made); code != 201 || made.Setup == "" {
		e.t.Fatalf("create %s: HTTP %d %+v", name, code, made)
	}
	b := &browser{e: e}
	var s struct{ Secret, URI string }
	if code := b.do("POST", "/auth/setup", map[string]string{"name": name, "setup": made.Setup, "password": password}, &s); code != 200 {
		e.t.Fatalf("setup %s: HTTP %d", name, code)
	}
	secret, err := b32.DecodeString(s.Secret)
	if err != nil || len(secret) != 20 || !strings.Contains(s.URI, "secret="+s.Secret) {
		e.t.Fatalf("setup answer: %+v", s)
	}
	if code := b.do("POST", "/auth/setup/confirm", map[string]string{"name": name, "setup": made.Setup, "code": e.code(secret)}, nil); code != 200 {
		e.t.Fatalf("confirm %s: HTTP %d", name, code)
	}
	e.now = e.now.Add(time.Minute) // the code just used is spent
	return secret
}

func (e *env) code(secret []byte) string { return totpAt(secret, e.now.Unix()/30) }

func (e *env) login(name, password, code string) (*browser, int) {
	b := &browser{e: e}
	return b, b.do("POST", "/auth/login", map[string]string{"name": name, "password": password, "code": code}, nil)
}

func TestTOTPKnownAnswers(t *testing.T) {
	// RFC 6238, appendix B, SHA-1, cut to six digits
	secret := []byte("12345678901234567890")
	for unix, want := range map[int64]string{59: "287082", 1111111109: "081804", 1234567890: "005924", 20000000000: "353130"} {
		if got := totpAt(secret, unix/30); got != want {
			t.Errorf("t=%d: %s, want %s", unix, got, want)
		}
	}
}

func TestAccountSetupAndLogin(t *testing.T) {
	e := newEnv(t)
	const pw = "correct horse battery"
	var made struct{ Setup string }
	if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": "Alice", "kind": "local"}, &made); code != 201 {
		t.Fatalf("create: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": "alice", "kind": "local"}, nil); code != 409 {
		t.Fatalf("same name again: HTTP %d", code)
	}
	for _, bad := range []string{"", "a", "token", "has space", "<b>"} {
		if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": bad, "kind": "local"}, nil); code != 400 {
			t.Fatalf("name %q: HTTP %d", bad, code)
		}
	}
	b := &browser{e: e}
	// not usable before it is set up
	if _, code := e.login("alice", pw, "000000"); code != 401 {
		t.Fatalf("login before setup: HTTP %d", code)
	}
	if code := b.do("POST", "/auth/setup", map[string]string{"name": "alice", "setup": "AAAA-AAAA-AAAA-AAAA", "password": pw}, nil); code != 401 {
		t.Fatalf("wrong setup code: HTTP %d", code)
	}
	if code := b.do("POST", "/auth/setup", map[string]string{"name": "alice", "setup": made.Setup, "password": "short"}, nil); code != 400 {
		t.Fatalf("short password: HTTP %d", code)
	}
	var s struct{ Secret string }
	if code := b.do("POST", "/auth/setup", map[string]string{"name": "alice", "setup": made.Setup, "password": pw}, &s); code != 200 {
		t.Fatalf("setup: HTTP %d", code)
	}
	secret, _ := b32.DecodeString(s.Secret)
	// password alone is not an account yet
	if _, code := e.login("alice", pw, e.code(secret)); code != 401 {
		t.Fatalf("login before the authenticator is confirmed: HTTP %d", code)
	}
	if code := b.do("POST", "/auth/setup/confirm", map[string]string{"name": "alice", "setup": made.Setup, "code": "000000"}, nil); code != 401 || b.cookie != nil {
		t.Fatalf("wrong code at setup: HTTP %d", code)
	}
	if code := b.do("POST", "/auth/setup/confirm", map[string]string{"name": "alice", "setup": made.Setup, "code": e.code(secret)}, nil); code != 200 || b.cookie == nil {
		t.Fatalf("confirm: HTTP %d", code)
	}
	if !b.cookie.HttpOnly || b.cookie.SameSite != http.SameSiteStrictMode {
		t.Fatalf("cookie: %+v", b.cookie)
	}
	// the setup code is spent
	if code := (&browser{e: e}).do("POST", "/auth/setup", map[string]string{"name": "alice", "setup": made.Setup, "password": "another password!"}, nil); code != 401 {
		t.Fatalf("setup code twice: HTTP %d", code)
	}

	// signed in: reading works, changing needs the header other sites cannot send
	var who struct{ User, Kind string }
	if b.do("GET", "/auth/methods", nil, &who); who.User != "alice" || who.Kind != "local" {
		t.Fatalf("methods: %+v", who)
	}
	if code := b.do("GET", "/admin/v1/devices", nil, nil); code != 200 {
		t.Fatalf("devices with cookie: HTTP %d", code)
	}
	b.plain = true
	if code := b.do("POST", "/admin/v1/enroll-tokens", map[string]int{"ttl_s": 60}, nil); code != 403 {
		t.Fatalf("POST without X-C2: HTTP %d", code)
	}
	b.plain = false
	if code := b.do("POST", "/admin/v1/enroll-tokens", map[string]int{"ttl_s": 60}, nil); code != 201 {
		t.Fatalf("POST with X-C2: HTTP %d", code)
	}
	if code := b.do("DELETE", "/admin/v1/users/alice", nil, nil); code != 409 {
		t.Fatalf("deleting oneself: HTTP %d", code)
	}

	// a code works once
	if _, code := e.login("alice", pw, e.code(secret)); code != 401 {
		t.Fatalf("the code from the setup again: HTTP %d", code)
	}
	e.now = e.now.Add(time.Minute)
	if _, code := e.login("alice", "wrong password here", e.code(secret)); code != 401 {
		t.Fatalf("wrong password: HTTP %d", code)
	}
	b2, code := e.login("ALICE ", pw, e.code(secret))
	if code != 200 || b2.cookie == nil {
		t.Fatalf("login: HTTP %d", code)
	}
	// the neighbouring time step is taken too, a stale code is not
	e.now = e.now.Add(2 * time.Minute)
	if _, code := e.login("alice", pw, totpAt(secret, e.now.Unix()/30-2)); code != 401 {
		t.Fatalf("stale code: HTTP %d", code)
	}
	if _, code := e.login("alice", pw, totpAt(secret, e.now.Unix()/30+1)); code != 200 {
		t.Fatalf("code of the next step: HTTP %d", code)
	}

	// signing out ends that session and no other
	old := *b.cookie
	if code := b.do("POST", "/auth/logout", nil, nil); code != 200 || b.cookie != nil {
		t.Fatalf("logout: HTTP %d", code)
	}
	if code := (&browser{e: e, cookie: &old}).do("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("old cookie after logout: HTTP %d", code)
	}
	if code := b2.do("GET", "/admin/v1/devices", nil, nil); code != 200 {
		t.Fatalf("other session after logout: HTTP %d", code)
	}
	// a session nobody uses ends; one in use ends after twelve hours
	e.now = e.now.Add(webIdle + time.Minute)
	if code := b2.do("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("idle session: HTTP %d", code)
	}
	b3, _ := e.login("alice", pw, e.code(secret))
	for i := 0; i < int(webMax/(20*time.Minute)); i++ {
		e.now = e.now.Add(20 * time.Minute)
		if code := b3.do("GET", "/admin/v1/devices", nil, nil); code != 200 && i < int(webMax/(20*time.Minute))-1 {
			t.Fatalf("session in use, after %d steps: HTTP %d", i, code)
		}
	}
	e.now = e.now.Add(time.Minute)
	if code := b3.do("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("session older than %s: HTTP %d", webMax, code)
	}
}

func TestLoginLockAndReset(t *testing.T) {
	e := newEnv(t)
	const pw = "correct horse battery"
	secret := e.newUser("bob", pw)
	for i := 0; i < loginFails; i++ {
		if _, code := e.login("bob", "not the password!", e.code(secret)); code != 401 {
			t.Fatalf("wrong password %d: HTTP %d", i, code)
		}
	}
	if _, code := e.login("bob", pw, e.code(secret)); code != 401 {
		t.Fatalf("right password while locked: HTTP %d", code)
	}
	e.now = e.now.Add(2 * loginLock) // the try above started the lock again
	b, code := e.login("bob", pw, e.code(secret))
	if code != 200 {
		t.Fatalf("after the lock: HTTP %d", code)
	}
	// a name nobody has behaves like a wrong password
	if _, code := e.login("nobody", pw, "123456"); code != 401 {
		t.Fatalf("unknown name: HTTP %d", code)
	}

	// disabling signs out and keeps out
	if code := e.admin("POST", "/admin/v1/users/bob/disable", map[string]bool{"disabled": true}, nil); code != 200 {
		t.Fatalf("disable: HTTP %d", code)
	}
	if code := b.do("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("session of a disabled account: HTTP %d", code)
	}
	e.now = e.now.Add(time.Minute)
	if _, code := e.login("bob", pw, e.code(secret)); code != 401 {
		t.Fatalf("login of a disabled account: HTTP %d", code)
	}
	e.admin("POST", "/admin/v1/users/bob/disable", map[string]bool{"disabled": false}, nil)
	b, code = e.login("bob", pw, e.code(secret))
	if code != 200 {
		t.Fatalf("login after enabling: HTTP %d", code)
	}

	// reset: the old password and authenticator are gone, a new setup code is out
	var re struct{ Setup string }
	if code := e.admin("POST", "/admin/v1/users/bob/reset", nil, &re); code != 200 || re.Setup == "" {
		t.Fatalf("reset: HTTP %d", code)
	}
	if code := b.do("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("session after reset: HTTP %d", code)
	}
	e.now = e.now.Add(time.Minute)
	if _, code := e.login("bob", pw, e.code(secret)); code != 401 {
		t.Fatalf("old password after reset: HTTP %d", code)
	}
	e.now = e.now.Add(setupTTL)
	if code := (&browser{e: e}).do("POST", "/auth/setup", map[string]string{"name": "bob", "setup": re.Setup, "password": pw}, nil); code != 401 {
		t.Fatalf("setup code after a day: HTTP %d", code)
	}

	var users []userView
	e.admin("GET", "/admin/v1/users", nil, &users)
	if len(users) != 1 || users[0].Name != "bob" || users[0].Ready || users[0].Setup != nil {
		t.Fatalf("users: %+v", users)
	}
	raw, _ := json.Marshal(users)
	for _, secretField := range []string{"pass", "totp", "setup_hash"} {
		if strings.Contains(string(raw), `"`+secretField) {
			t.Fatalf("the user list shows %q: %s", secretField, raw)
		}
	}
	if code := e.admin("DELETE", "/admin/v1/users/bob", nil, nil); code != 200 {
		t.Fatalf("delete: HTTP %d", code)
	}
	if code := e.admin("DELETE", "/admin/v1/users/bob", nil, nil); code != 404 {
		t.Fatalf("delete twice: HTTP %d", code)
	}
}

func TestAuditAndTokenOff(t *testing.T) {
	e := newEnv(t)
	const pw = "correct horse battery"
	secret := e.newUser("carol", pw)
	b, _ := e.login("carol", pw, e.code(secret))
	e.login("carol", "wrong wrong wrong", "111111")
	c := e.enrolled()
	b.do("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "ping"}, nil)
	b.do("POST", "/admin/v1/devices/"+c.ID()+"/commands", map[string]string{"type": "nonsense"}, nil)
	b.do("GET", "/admin/v1/devices", nil, nil)

	var log []store.AuditEntry
	if code := b.do("GET", "/admin/v1/audit", nil, &log); code != 200 {
		t.Fatalf("audit: HTTP %d", code)
	}
	has := func(who, action, target, detail string, ok bool) bool {
		for _, a := range log {
			if a.Who == who && a.Action == action && a.Target == target && a.Detail == detail && a.OK == ok && a.IP != "" {
				return true
			}
		}
		return false
	}
	for _, want := range []struct {
		who, action, target, detail string
		ok                          bool
	}{
		{"token", "POST /users", "carol", "local", true},
		{"carol", "setup", "carol", "", true},
		{"carol", "login", "carol", "", true},
		{"-", "login", "carol", "", false},
		{"token", "POST /devices/{id}/approve", c.ID(), "", true},
		{"carol", "POST /devices/{id}/commands", c.ID(), "ping", true},
		{"carol", "POST /devices/{id}/commands", c.ID(), "nonsense", false},
	} {
		if !has(want.who, want.action, want.target, want.detail, want.ok) {
			t.Errorf("missing in the audit record: %+v", want)
		}
	}
	for _, a := range log {
		if strings.HasPrefix(a.Action, "GET") {
			t.Errorf("reading is on record: %+v", a)
		}
	}
	if log[0].Time.Before(log[len(log)-1].Time) {
		t.Error("audit record is not newest first")
	}

	// without an admin token in the configuration no token gets in, not even an empty one
	e.sv.cfg.AdminToken = ""
	if code := e.admin("GET", "/admin/v1/devices", nil, nil); code != 401 {
		t.Fatalf("token although none is configured: HTTP %d", code)
	}
	req, _ := http.NewRequest("GET", e.ts.URL+"/admin/v1/devices", nil)
	req.Header.Set("Authorization", "Bearer ")
	resp, _ := http.DefaultClient.Do(req)
	resp.Body.Close()
	if resp.StatusCode != 401 {
		t.Fatalf("empty token: HTTP %d", resp.StatusCode)
	}
	if code := b.do("GET", "/admin/v1/devices", nil, nil); code != 200 {
		t.Fatalf("account while the token is off: HTTP %d", code)
	}
	var m struct{ Token bool }
	if (&browser{e: e}).do("GET", "/auth/methods", nil, &m); m.Token {
		t.Fatalf("methods: %+v", m)
	}
}

func TestRateLimits(t *testing.T) {
	e := newEnv(t)
	post := func(path string) int {
		resp, err := http.Post(e.ts.URL+path, "application/json", strings.NewReader(`{"name":"x","password":"y","code":"z"}`))
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		if resp.StatusCode == 429 && resp.Header.Get("Retry-After") == "" {
			t.Fatal("429 without Retry-After")
		}
		return resp.StatusCode
	}
	count := func(path string, n int) (limited int) {
		for i := 0; i < n; i++ {
			if post(path) == 429 {
				limited++
			}
		}
		return limited
	}
	if n := count("/auth/login", 15); n != 5 {
		t.Fatalf("15 sign-ins at once: %d turned away, want 5", n)
	}
	if n := count("/v1/handshake", 40); n != 10 {
		t.Fatalf("40 handshakes at once: %d turned away, want 10", n)
	}
	e.now = e.now.Add(12 * time.Second)
	if n := count("/auth/login", 3); n != 1 {
		t.Fatalf("after 12 s: %d of 3 sign-ins turned away, want 1", n)
	}
	// wrong tokens at the admin API
	bad := 0
	for i := 0; i < 40; i++ {
		req, _ := http.NewRequest("GET", e.ts.URL+"/admin/v1/devices", nil)
		req.Header.Set("Authorization", "Bearer guess")
		resp, _ := http.DefaultClient.Do(req)
		resp.Body.Close()
		if resp.StatusCode == 429 {
			bad++
		}
	}
	if bad != 10 {
		t.Fatalf("40 wrong tokens: %d turned away, want 10", bad)
	}
	if code := e.admin("GET", "/admin/v1/devices", nil, nil); code != 200 {
		t.Fatalf("the right token after that: HTTP %d", code)
	}

	// the address behind a proxy counts only if the proxy is trusted
	r, _ := http.NewRequest("GET", "/", nil)
	r.RemoteAddr = "10.0.0.1:1234"
	r.Header.Add("X-Forwarded-For", "6.6.6.6, 203.0.113.9")
	if ip := e.sv.clientIP(r); ip != "10.0.0.1" {
		t.Fatalf("untrusted proxy header used: %s", ip)
	}
	e.sv.cfg.TrustProxy = true
	if ip := e.sv.clientIP(r); ip != "203.0.113.9" {
		t.Fatalf("behind a proxy: %s", ip)
	}
}
