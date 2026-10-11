package server

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"rs232c2/internal/store"
)

// Sign-in with Microsoft Entra ID: OpenID Connect, authorization code flow
// with PKCE, the ID token checked here against the tenant's signing keys.
//
// Entra says who somebody is; whether that person is an administrator of this
// server is decided here: there has to be an account of kind "entra" for them
// (KONZEPT.md §5.1). A second factor is Entra's business - require it there
// with Conditional Access. The v2.0 ID token does not say how somebody signed
// in, so this server cannot check it.

// EntraConfig comes from the app registration in the Entra admin center.
type EntraConfig struct {
	Tenant       string // tenant ID (a GUID); only this tenant's users get in
	ClientID     string
	ClientSecret string
	// Authority is where the tenant's endpoints live. Empty means Microsoft's
	// public cloud; the tests put their own identity provider here.
	Authority string
}

const (
	oidcCookie  = "c2_oidc"
	oidcTTL     = 10 * time.Minute
	maxOIDC     = 1000
	jwksTTL     = time.Hour
	jwksRetry   = time.Minute
	clockSkew   = 5 * time.Minute
	maxIDPReply = 256 * 1024
)

type oidcFlow struct {
	nonce, verifier string
	expires         time.Time
}

type entra struct {
	cfg      EntraConfig
	redirect string
	hc       *http.Client

	mu      sync.Mutex
	flows   map[string]*oidcFlow // by state
	keys    map[string]*rsa.PublicKey
	fetched time.Time
}

func newEntra(cfg EntraConfig, publicURL string) *entra {
	if cfg.Authority == "" {
		cfg.Authority = "https://login.microsoftonline.com/" + cfg.Tenant
	}
	cfg.Authority = strings.TrimRight(cfg.Authority, "/")
	return &entra{cfg: cfg, redirect: strings.TrimRight(publicURL, "/") + "/auth/entra/callback",
		hc: &http.Client{Timeout: 15 * time.Second}, flows: map[string]*oidcFlow{}}
}

func randomURL(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}

// entraLogin sends the browser to Entra.
func (sv *Server) entraLogin(w http.ResponseWriter, r *http.Request) {
	if sv.entra == nil {
		http.NotFound(w, r)
		return
	}
	if sv.limited(sv.limAuth, w, r) {
		return
	}
	e := sv.entra
	state, f := randomURL(24), &oidcFlow{nonce: randomURL(24), verifier: randomURL(48), expires: sv.now().Add(oidcTTL)}
	e.mu.Lock()
	for k, x := range e.flows {
		if sv.now().After(x.expires) {
			delete(e.flows, k)
		}
	}
	full := len(e.flows) >= maxOIDC
	if !full {
		e.flows[state] = f
	}
	e.mu.Unlock()
	if full {
		http.Error(w, "too many sign-ins under way", http.StatusServiceUnavailable)
		return
	}
	challenge := sha256.Sum256([]byte(f.verifier))
	q := url.Values{
		"client_id":             {e.cfg.ClientID},
		"response_type":         {"code"},
		"response_mode":         {"query"},
		"redirect_uri":          {e.redirect},
		"scope":                 {"openid profile"},
		"state":                 {state},
		"nonce":                 {f.nonce},
		"code_challenge":        {base64.RawURLEncoding.EncodeToString(challenge[:])},
		"code_challenge_method": {"S256"},
	}
	// The state also goes into a cookie: the answer has to come back in the
	// browser that started here. Lax, because it comes back from another site.
	sv.setCookie(w, oidcCookie, state, int(oidcTTL/time.Second), http.SameSiteLaxMode)
	http.Redirect(w, r, e.cfg.Authority+"/oauth2/v2.0/authorize?"+q.Encode(), http.StatusFound)
}

// entraCallback is where Entra sends the browser back. Whatever goes wrong,
// the browser lands on the login page with a short reason; the details are in
// the log and in the audit record.
func (sv *Server) entraCallback(w http.ResponseWriter, r *http.Request) {
	if sv.entra == nil {
		http.NotFound(w, r)
		return
	}
	if sv.limited(sv.limAuth, w, r) {
		return
	}
	sv.setCookie(w, oidcCookie, "", -1, http.SameSiteLaxMode)
	name, err := sv.entraFinish(r)
	if err != nil {
		log.Printf("entra: sign-in refused: %v", err)
		sv.audit(r, "-", "login entra", "", err.Error(), false)
		http.Redirect(w, r, "/#entra=refused", http.StatusFound)
		return
	}
	sv.audit(r, name, "login entra", name, "", true)
	sv.startSession(w, principal{Name: name, Kind: store.UserEntra})
	http.Redirect(w, r, "/", http.StatusFound)
}

type idClaims struct {
	Iss   string  `json:"iss"`
	Aud   string  `json:"aud"`
	Tid   string  `json:"tid"`
	Oid   string  `json:"oid"`
	Nonce string  `json:"nonce"`
	Exp   float64 `json:"exp"`
	Nbf   float64 `json:"nbf"`
	Name  string  `json:"preferred_username"`
}

func (sv *Server) entraFinish(r *http.Request) (string, error) {
	e := sv.entra
	q := r.URL.Query()
	state := q.Get("state")
	c, err := r.Cookie(oidcCookie)
	if err != nil || state == "" || subtle.ConstantTimeCompare([]byte(c.Value), []byte(state)) != 1 {
		return "", errors.New("the answer does not belong to a sign-in started in this browser")
	}
	e.mu.Lock()
	f := e.flows[state]
	delete(e.flows, state) // one answer per sign-in
	e.mu.Unlock()
	if f == nil || sv.now().After(f.expires) {
		return "", errors.New("sign-in unknown or too old")
	}
	if msg := q.Get("error"); msg != "" {
		return "", fmt.Errorf("Entra says %s: %.200s", msg, q.Get("error_description"))
	}
	form := url.Values{
		"client_id":     {e.cfg.ClientID},
		"client_secret": {e.cfg.ClientSecret},
		"grant_type":    {"authorization_code"},
		"code":          {q.Get("code")},
		"redirect_uri":  {e.redirect},
		"code_verifier": {f.verifier},
	}
	resp, err := e.hc.PostForm(e.cfg.Authority+"/oauth2/v2.0/token", form)
	if err != nil {
		return "", fmt.Errorf("token endpoint: %w", err)
	}
	defer resp.Body.Close()
	var tok struct {
		IDToken string `json:"id_token"`
		Error   string `json:"error"`
		Desc    string `json:"error_description"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxIDPReply)).Decode(&tok); err != nil || tok.IDToken == "" {
		return "", fmt.Errorf("token endpoint: HTTP %d %s %.200s", resp.StatusCode, tok.Error, tok.Desc)
	}
	cl, err := e.verify(tok.IDToken, sv.now())
	if err != nil {
		return "", err
	}
	if cl.Nonce == "" || subtle.ConstantTimeCompare([]byte(cl.Nonce), []byte(f.nonce)) != 1 {
		return "", errors.New("ID token: wrong nonce")
	}
	return sv.entraAccount(cl)
}

// entraAccount finds the account the person Entra vouches for may use.
func (sv *Server) entraAccount(cl *idClaims) (string, error) {
	oid, upn := strings.ToLower(cl.Oid), strings.ToLower(cl.Name)
	if !guid.MatchString(oid) {
		return "", errors.New("ID token: no object ID")
	}
	var byName *store.User
	for _, u := range sv.st.Users() {
		if u.Kind != store.UserEntra {
			continue
		}
		if u.EntraOID == oid {
			if u.Disabled {
				return "", fmt.Errorf("account %q is disabled", u.Name)
			}
			now := sv.now()
			return u.Name, sv.st.UpdateUser(u.Name, func(u *store.User) error { u.LastLogin = &now; return nil })
		}
		if u.EntraOID == "" && u.EntraName != "" && u.EntraName == upn {
			byName = u
		}
	}
	if byName == nil {
		return "", fmt.Errorf("no account here for %q (object ID %s)", upn, oid)
	}
	if byName.Disabled {
		return "", fmt.Errorf("account %q is disabled", byName.Name)
	}
	// First sign-in of an account made for a sign-in name: from now on the
	// object ID counts. Names can be changed and given to somebody else.
	now := sv.now()
	return byName.Name, sv.st.UpdateUser(byName.Name, func(u *store.User) error {
		if u.EntraOID != "" {
			return store.ErrState
		}
		u.EntraOID, u.LastLogin = oid, &now
		return nil
	})
}

// verify checks an ID token: signed by one of the tenant's keys (RS256), made
// by this tenant for this application, and valid now.
func (e *entra) verify(token string, now time.Time) (*idClaims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("ID token: not a JWT")
	}
	var head struct{ Alg, Kid string }
	hb, err1 := base64.RawURLEncoding.DecodeString(parts[0])
	pb, err2 := base64.RawURLEncoding.DecodeString(parts[1])
	sig, err3 := base64.RawURLEncoding.DecodeString(parts[2])
	if err1 != nil || err2 != nil || err3 != nil || json.Unmarshal(hb, &head) != nil {
		return nil, errors.New("ID token: malformed")
	}
	if head.Alg != "RS256" { // nothing else, above all not "none"
		return nil, fmt.Errorf("ID token: algorithm %q", head.Alg)
	}
	key, err := e.key(head.Kid, now)
	if err != nil {
		return nil, err
	}
	sum := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	if err := rsa.VerifyPKCS1v15(key, crypto.SHA256, sum[:], sig); err != nil {
		return nil, errors.New("ID token: bad signature")
	}
	var cl idClaims
	if err := json.Unmarshal(pb, &cl); err != nil {
		return nil, errors.New("ID token: claims are not JSON with one audience")
	}
	t := float64(now.Unix())
	switch {
	case cl.Iss != e.cfg.Authority+"/v2.0":
		return nil, fmt.Errorf("ID token: issuer %q", cl.Iss)
	case cl.Aud != e.cfg.ClientID:
		return nil, errors.New("ID token: made for another application")
	case !strings.EqualFold(cl.Tid, e.cfg.Tenant):
		return nil, fmt.Errorf("ID token: tenant %q", cl.Tid)
	case cl.Exp == 0 || t > cl.Exp+clockSkew.Seconds():
		return nil, errors.New("ID token: expired")
	case cl.Nbf != 0 && t < cl.Nbf-clockSkew.Seconds():
		return nil, errors.New("ID token: not valid yet")
	}
	return &cl, nil
}

// key returns the tenant's signing key with that ID. The set is fetched when
// it is an hour old, or when a key is missing - Entra changes them now and
// then - but not more often than once a minute.
func (e *entra) key(kid string, now time.Time) (*rsa.PublicKey, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if k := e.keys[kid]; k != nil && now.Sub(e.fetched) < jwksTTL {
		return k, nil
	}
	if e.keys != nil && now.Sub(e.fetched) < jwksRetry {
		if k := e.keys[kid]; k != nil {
			return k, nil
		}
		return nil, fmt.Errorf("ID token: unknown signing key %q", kid)
	}
	resp, err := e.hc.Get(e.cfg.Authority + "/discovery/v2.0/keys")
	if err != nil {
		return nil, fmt.Errorf("signing keys: %w", err)
	}
	defer resp.Body.Close()
	var set struct {
		Keys []struct{ Kty, Kid, N, E string }
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxIDPReply)).Decode(&set); err != nil || resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("signing keys: HTTP %d", resp.StatusCode)
	}
	keys := map[string]*rsa.PublicKey{}
	for _, k := range set.Keys {
		n, err1 := base64.RawURLEncoding.DecodeString(k.N)
		ex, err2 := base64.RawURLEncoding.DecodeString(k.E)
		if k.Kty != "RSA" || err1 != nil || err2 != nil || len(n) < 256 || len(ex) == 0 || len(ex) > 4 {
			continue
		}
		keys[k.Kid] = &rsa.PublicKey{N: new(big.Int).SetBytes(n), E: int(new(big.Int).SetBytes(ex).Int64())}
	}
	e.keys, e.fetched = keys, now
	if k := keys[kid]; k != nil {
		return k, nil
	}
	return nil, fmt.Errorf("ID token: unknown signing key %q", kid)
}
