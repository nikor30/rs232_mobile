package server

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

const (
	testTenant = "11111111-2222-3333-4444-555555555555"
	testClient = "app-client-id"
	oidAlice   = "aaaaaaaa-0000-0000-0000-000000000001"
)

// idp stands in for Entra: it hands out whatever ID token the test has made.
type idp struct {
	ts       *httptest.Server
	key      *rsa.PrivateKey
	token    string     // what the token endpoint answers next
	lastForm url.Values // what the server sent to the token endpoint
}

func b64(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

func jwt(key *rsa.PrivateKey, alg, kid string, claims map[string]any) string {
	h, _ := json.Marshal(map[string]string{"alg": alg, "kid": kid, "typ": "JWT"})
	c, _ := json.Marshal(claims)
	signed := b64(h) + "." + b64(c)
	if alg == "none" {
		return signed + "."
	}
	sum := sha256.Sum256([]byte(signed))
	sig, _ := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, sum[:])
	return signed + "." + b64(sig)
}

func newIDP(t *testing.T) *idp {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	p := &idp{key: key}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /discovery/v2.0/keys", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, 200, map[string]any{"keys": []map[string]string{{"kty": "RSA", "kid": "k1",
			"n": b64(key.N.Bytes()), "e": b64(big.NewInt(int64(key.E)).Bytes())}}})
	})
	mux.HandleFunc("POST /oauth2/v2.0/token", func(w http.ResponseWriter, r *http.Request) {
		r.ParseForm()
		p.lastForm = r.PostForm
		writeJSON(w, 200, map[string]string{"id_token": p.token})
	})
	p.ts = httptest.NewServer(mux)
	t.Cleanup(p.ts.Close)
	return p
}

// signIn walks through one sign-in as a browser would and returns where the
// browser ends up and the browser itself. claims gets the nonce of this sign-in.
func (p *idp) signIn(e *env, claims func(nonce string) string) (string, *browser) {
	e.t.Helper()
	e.now = e.now.Add(time.Minute) // sign-ins are rationed per address, and an unknown key is looked up once a minute
	hc := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	resp, err := hc.Get(e.ts.URL + "/auth/entra/login")
	if err != nil {
		e.t.Fatal(err)
	}
	resp.Body.Close()
	to, _ := url.Parse(resp.Header.Get("Location"))
	q := to.Query()
	if resp.StatusCode != 302 || !strings.HasPrefix(to.String(), p.ts.URL+"/oauth2/v2.0/authorize?") || q.Get("client_id") != testClient ||
		q.Get("code_challenge_method") != "S256" || q.Get("redirect_uri") != e.ts.URL+"/auth/entra/callback" || len(q.Get("state")) < 20 {
		e.t.Fatalf("start of the sign-in: HTTP %d to %s", resp.StatusCode, to)
	}
	p.token = claims(q.Get("nonce"))
	req, _ := http.NewRequest("GET", e.ts.URL+"/auth/entra/callback?code=the-code&state="+q.Get("state"), nil)
	for _, c := range resp.Cookies() {
		req.AddCookie(c)
	}
	resp, err = hc.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	resp.Body.Close()
	b := &browser{e: e}
	for _, c := range resp.Cookies() {
		if c.Name == e.sv.cookieName() {
			b.cookie = c
		}
	}
	// PKCE: what was sent to the token endpoint has to hash to the challenge
	sum := sha256.Sum256([]byte(p.lastForm.Get("code_verifier")))
	if b64(sum[:]) != q.Get("code_challenge") || p.lastForm.Get("code") != "the-code" {
		e.t.Fatalf("token request: %v", p.lastForm)
	}
	return resp.Header.Get("Location"), b
}

func TestEntraSignIn(t *testing.T) {
	e := newEnv(t)
	p := newIDP(t)
	e.sv.entra = newEntra(EntraConfig{Tenant: testTenant, ClientID: testClient, ClientSecret: "s3cret", Authority: p.ts.URL}, e.ts.URL)
	other, _ := rsa.GenerateKey(rand.Reader, 2048)

	good := func(nonce string) map[string]any {
		return map[string]any{"iss": p.ts.URL + "/v2.0", "aud": testClient, "tid": testTenant, "oid": oidAlice, "nonce": nonce,
			"preferred_username": "Alice@Example.com", "exp": e.now.Add(time.Hour).Unix(), "nbf": e.now.Add(-time.Minute).Unix()}
	}
	with := func(change func(c map[string]any)) func(string) string {
		return func(nonce string) string {
			c := good(nonce)
			change(c)
			return jwt(p.key, "RS256", "k1", c)
		}
	}
	ok := with(func(map[string]any) {})

	// Entra vouches for Alice, but nobody has made her an administrator here
	if to, b := p.signIn(e, ok); to != "/#entra=refused" || b.cookie != nil {
		t.Fatalf("no account: to %q, cookie %v", to, b.cookie)
	}
	if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": "alice", "kind": "entra", "entra": "nonsense"}, nil); code != 400 {
		t.Fatalf("entra account without a name: HTTP %d", code)
	}
	if code := e.admin("POST", "/admin/v1/users", map[string]string{"name": "alice", "kind": "entra", "entra": "alice@example.com"}, nil); code != 201 {
		t.Fatalf("create: HTTP %d", code)
	}
	to, b := p.signIn(e, ok)
	if to != "/" || b.cookie == nil {
		t.Fatalf("sign-in: to %q", to)
	}
	var who struct{ User, Kind string }
	if b.do("GET", "/auth/methods", nil, &who); who.User != "alice" || who.Kind != "entra" {
		t.Fatalf("methods: %+v", who)
	}
	if code := b.do("POST", "/admin/v1/enroll-tokens", nil, nil); code != 201 {
		t.Fatalf("admin API after the sign-in: HTTP %d", code)
	}
	// she cannot sign in with a password: there is none
	if _, code := e.login("alice", "", ""); code != 401 {
		t.Fatalf("password login of an Entra account: HTTP %d", code)
	}

	// from the first sign-in on the object ID counts, not the name
	var users []userView
	e.admin("GET", "/admin/v1/users", nil, &users)
	if len(users) != 1 || users[0].EntraOID != oidAlice || !users[0].Ready {
		t.Fatalf("users: %+v", users)
	}
	if to, _ := p.signIn(e, with(func(c map[string]any) { c["preferred_username"] = "renamed@example.com" })); to != "/" {
		t.Fatalf("same person, new name: to %q", to)
	}
	if to, _ := p.signIn(e, with(func(c map[string]any) { c["oid"] = "bbbbbbbb-0000-0000-0000-000000000002" })); to != "/#entra=refused" {
		t.Fatalf("another person with her old name: to %q", to)
	}

	// everything a token can be wrong in
	for name, claims := range map[string]func(string) string{
		"another application":  with(func(c map[string]any) { c["aud"] = "someone-else" }),
		"another tenant":       with(func(c map[string]any) { c["tid"] = "99999999-2222-3333-4444-555555555555" }),
		"another issuer":       with(func(c map[string]any) { c["iss"] = "https://evil.example/v2.0" }),
		"expired":              with(func(c map[string]any) { c["exp"] = e.now.Add(-time.Hour).Unix() }),
		"not yet valid":        with(func(c map[string]any) { c["nbf"] = e.now.Add(time.Hour).Unix() }),
		"no expiry":            with(func(c map[string]any) { delete(c, "exp") }),
		"another nonce":        with(func(c map[string]any) { c["nonce"] = "replayed" }),
		"no object ID":         with(func(c map[string]any) { delete(c, "oid") }),
		"signed by a stranger": func(nonce string) string { return jwt(other, "RS256", "k1", good(nonce)) },
		"unknown key":          func(nonce string) string { return jwt(p.key, "RS256", "k2", good(nonce)) },
		"not signed":           func(nonce string) string { return jwt(nil, "none", "k1", good(nonce)) },
		"another algorithm in the header": func(nonce string) string {
			parts := strings.Split(jwt(p.key, "RS256", "k1", good(nonce)), ".")
			return b64([]byte(`{"alg":"HS256","kid":"k1","typ":"JWT"}`)) + "." + parts[1] + "." + parts[2]
		},
		"claims changed after signing": func(nonce string) string {
			parts := strings.Split(jwt(p.key, "RS256", "k1", good(nonce)), ".")
			c := good(nonce)
			c["oid"] = oidAlice
			c["aud"] = testClient
			c["extra"] = true
			j, _ := json.Marshal(c)
			return parts[0] + "." + b64(j) + "." + parts[2]
		},
		"garbage": func(string) string { return "not.a.jwt" },
	} {
		if to, b := p.signIn(e, claims); to != "/#entra=refused" || b.cookie != nil {
			t.Errorf("%s: to %q, cookie %v", name, to, b.cookie)
		}
	}

	// the answer has to come back in the browser that asked, and only once
	hc := &http.Client{CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	resp, _ := hc.Get(e.ts.URL + "/auth/entra/login")
	resp.Body.Close()
	to2, _ := url.Parse(resp.Header.Get("Location"))
	state := to2.Query().Get("state")
	p.token = ok(to2.Query().Get("nonce"))
	callback := func(withCookie bool) string {
		req, _ := http.NewRequest("GET", e.ts.URL+"/auth/entra/callback?code=the-code&state="+state, nil)
		if withCookie {
			for _, c := range resp.Cookies() {
				req.AddCookie(c)
			}
		}
		r, _ := hc.Do(req)
		r.Body.Close()
		return r.Header.Get("Location")
	}
	if to := callback(false); to != "/#entra=refused" {
		t.Fatalf("answer in another browser: to %q", to)
	}
	if to := callback(true); to != "/" {
		t.Fatalf("answer in the right browser: to %q", to)
	}
	if to := callback(true); to != "/#entra=refused" {
		t.Fatalf("the same answer again: to %q", to)
	}

	// a disabled account stays out, and without Entra configured the paths do not exist
	e.admin("POST", "/admin/v1/users/alice/disable", map[string]bool{"disabled": true}, nil)
	e.now = e.now.Add(time.Minute)
	if to, _ := p.signIn(e, ok); to != "/#entra=refused" {
		t.Fatalf("disabled account: to %q", to)
	}
	e.sv.entra = nil
	if r, _ := http.Get(e.ts.URL + "/auth/entra/login"); r.StatusCode != 404 {
		t.Fatalf("login path without Entra: HTTP %d", r.StatusCode)
	}
}
