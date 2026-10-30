package server

import (
	"context"
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base32"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"rs232c2/internal/store"
)

// Who may use the admin API (KONZEPT.md §5.1):
//
//   - an account with a password and one-time codes from an authenticator app
//     (TOTP, RFC 6238), signed in through the web interface;
//   - the admin token from the environment, if one is set: for the first
//     account, for scripts and as a way in when nothing else works.
//
// A signed-in browser holds a random session cookie; the server keeps its hash
// in memory, so a restart signs everybody out.

const (
	kindToken = "token"

	webIdle        = 30 * time.Minute // a session nobody uses ends
	webMax         = 12 * time.Hour   // and none lives longer than this
	maxWebSessions = 1000
	setupTTL       = 24 * time.Hour
	minPassword    = 12
	maxPassword    = 256
	// Wrong passwords or codes for one name before it is locked for a while.
	// Whoever knows a name can lock it that way; the admin token and the other
	// accounts stay usable.
	loginFails = 5
	loginLock  = 15 * time.Minute
	maxFails   = 10000
)

// Rounds of PBKDF2-HMAC-SHA256 for a new password. A variable so the tests do
// not spend their time here.
var passIter = 600000

var (
	errLogin  = errors.New("sign-in failed")
	userNames = regexp.MustCompile(`^[a-z0-9][a-z0-9._@-]{1,63}$`)
	b32       = base32.StdEncoding.WithPadding(base32.NoPadding)
)

type principal struct {
	Name string
	Kind string // kindToken or store.UserLocal
	Role string // store.RoleAdmin or store.RoleMember; looked up anew for every request
}

type webSession struct {
	who           principal
	created, seen time.Time
}

type failCount struct {
	n     int
	until time.Time
}

type ctxKey int

const (
	ctxPrincipal ctxKey = iota
	ctxNote
)

// auditNote lets a handler say more about what it did, or that it is not
// worth an entry (the keystrokes of a remote console).
type auditNote struct {
	target, detail string
	skip           bool
}

func note(r *http.Request) *auditNote {
	if n, ok := r.Context().Value(ctxNote).(*auditNote); ok {
		return n
	}
	return &auditNote{}
}

func who(r *http.Request) principal {
	p, _ := r.Context().Value(ctxPrincipal).(principal)
	return p
}

func (sv *Server) audit(r *http.Request, who, action, target, detail string, ok bool) {
	e := store.AuditEntry{Who: who, IP: sv.clientIP(r), Action: action, Target: target, Detail: detail, OK: ok}
	if err := sv.st.Audit(e); err != nil {
		log.Printf("audit: %v", err)
	}
	log.Printf("audit: %s from %s: %s %s %s ok=%v", who, e.IP, action, target, detail, ok)
}

// ---------------------------------------------------------------- sessions

func (sv *Server) cookieName() string {
	if sv.cfg.SecureCookies {
		return "__Host-c2" // the browser takes it only over HTTPS, for this host alone
	}
	return "c2"
}

func (sv *Server) setCookie(w http.ResponseWriter, name, value string, maxAge int, sameSite http.SameSite) {
	http.SetCookie(w, &http.Cookie{Name: name, Value: value, Path: "/", MaxAge: maxAge, HttpOnly: true,
		Secure: sv.cfg.SecureCookies, SameSite: sameSite})
}

func (sv *Server) startSession(w http.ResponseWriter, p principal) {
	raw := make([]byte, 32)
	rand.Read(raw)
	now := sv.now()
	sv.authMu.Lock()
	if len(sv.web) >= maxWebSessions {
		for k, s := range sv.web {
			if now.Sub(s.seen) > webIdle || now.Sub(s.created) > webMax {
				delete(sv.web, k)
			}
		}
	}
	if len(sv.web) < maxWebSessions {
		sv.web[sha256.Sum256(raw)] = &webSession{who: p, created: now, seen: now}
	}
	sv.authMu.Unlock()
	sv.setCookie(w, sv.cookieName(), base64.RawURLEncoding.EncodeToString(raw), 0, http.SameSiteStrictMode)
}

func (sv *Server) sessionKey(r *http.Request) (key [32]byte, ok bool) {
	c, err := r.Cookie(sv.cookieName())
	if err != nil {
		return key, false
	}
	raw, err := base64.RawURLEncoding.DecodeString(c.Value)
	if err != nil || len(raw) != 32 {
		return key, false
	}
	return sha256.Sum256(raw), true
}

// authenticate says who sent the request: the admin token, a signed-in
// account, or nobody.
func (sv *Server) authenticate(r *http.Request) (principal, bool) {
	if h := r.Header.Get("Authorization"); h != "" {
		if sv.cfg.AdminToken != "" && subtle.ConstantTimeCompare([]byte(h), []byte("Bearer "+sv.cfg.AdminToken)) == 1 {
			return principal{Name: kindToken, Kind: kindToken}, true
		}
		return principal{}, false
	}
	key, ok := sv.sessionKey(r)
	if !ok {
		return principal{}, false
	}
	now := sv.now()
	sv.authMu.Lock()
	defer sv.authMu.Unlock()
	s := sv.web[key]
	if s == nil {
		return principal{}, false
	}
	if now.Sub(s.seen) > webIdle || now.Sub(s.created) > webMax {
		delete(sv.web, key)
		return principal{}, false
	}
	s.seen = now
	return s.who, true
}

func (sv *Server) endSessions(name string) {
	sv.authMu.Lock()
	for k, s := range sv.web {
		if s.who.Name == name && s.who.Kind != kindToken {
			delete(sv.web, k)
		}
	}
	sv.authMu.Unlock()
}

type statusWriter struct {
	http.ResponseWriter
	code int
}

// Unwrap lets http.ResponseController reach the connection (images.go).
func (w *statusWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

func (w *statusWriter) WriteHeader(code int) {
	if w.code == 0 {
		w.code = code
	}
	w.ResponseWriter.WriteHeader(code)
}

// admin guards the admin API and writes down every call that changes something.
// It is for administrators only.
func (sv *Server) admin(h http.HandlerFunc) http.HandlerFunc { return sv.adminMax(h, 64*1024) }

// adminMax is admin() for the rare call that takes a large body.
func (sv *Server) adminMax(h http.HandlerFunc, maxBody int64) http.HandlerFunc {
	return sv.guard(h, maxBody, func(p principal, _ *http.Request) int {
		if p.Role != store.RoleAdmin {
			return http.StatusForbidden
		}
		return 0
	})
}

// signed lets every signed-in account through: what a member of a group
// needs to see. The handler decides what it shows to whom.
func (sv *Server) signed(h http.HandlerFunc) http.HandlerFunc { return sv.guard(h, 64*1024, nil) }

// onDevice is for calls about the device {id}: the account needs that right
// on it (groups.go in the store). A device one has no right on does not exist.
func (sv *Server) onDevice(right string, h http.HandlerFunc) http.HandlerFunc {
	return sv.guard(h, 64*1024, func(p principal, r *http.Request) int {
		switch sv.st.Right(p.Name, p.Role, r.PathValue("id")) {
		case store.RightManage:
			return 0
		case store.RightUse:
			if right == store.RightUse {
				return 0
			}
			return http.StatusForbidden
		}
		return http.StatusNotFound
	})
}

func (sv *Server) guard(h http.HandlerFunc, maxBody int64, allow func(principal, *http.Request) int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		p, ok := sv.authenticate(r)
		if ok && p.Kind != kindToken {
			u, err := sv.st.User(p.Name)
			if ok = err == nil && !u.Disabled; ok {
				p.Role = u.Role
			}
		}
		if !ok {
			if sv.limited(sv.limFail, w, r) {
				return
			}
			w.Header().Set("WWW-Authenticate", "Bearer")
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		// A cookie travels with any request the browser makes to this server,
		// also one that another site set off. The header does not: other
		// sites cannot add it. The cookie is SameSite=Strict on top of that.
		if p.Kind != kindToken && r.Method != http.MethodGet && r.Header.Get("X-C2") == "" {
			http.Error(w, "missing X-C2 header", http.StatusForbidden)
			return
		}
		if allow != nil {
			if code := allow(p, r); code != 0 {
				if r.Method != http.MethodGet {
					sv.audit(r, p.Name, strings.Replace(r.Pattern, "/admin/v1", "", 1), r.PathValue("id")+r.PathValue("name"), "not allowed", false)
				}
				writeJSON(w, code, map[string]string{"error": map[int]string{http.StatusForbidden: "your account may not do this", http.StatusNotFound: "not found"}[code]})
				return
			}
		}
		r.Body = http.MaxBytesReader(w, r.Body, maxBody)
		n := &auditNote{}
		ctx := context.WithValue(context.WithValue(r.Context(), ctxPrincipal, p), ctxNote, n)
		sw := &statusWriter{ResponseWriter: w}
		h(sw, r.WithContext(ctx))
		if r.Method == http.MethodGet || n.skip {
			return
		}
		target := n.target
		if target == "" {
			target = r.PathValue("id") + r.PathValue("name")
		}
		sv.audit(r, p.Name, strings.Replace(r.Pattern, "/admin/v1", "", 1), target, n.detail, sw.code == 0 || sw.code < 400)
	}
}

// ---------------------------------------------------------------- passwords and codes

func hashPassword(password string, salt []byte, iter int) []byte {
	h, _ := pbkdf2.Key(sha256.New, password, salt, iter, 32)
	return h
}

// totpAt is the six-digit code of RFC 6238 (HMAC-SHA1, 30 s) for one time step.
func totpAt(secret []byte, step int64) string {
	var msg [8]byte
	for i := range msg {
		msg[7-i] = byte(step >> (8 * i))
	}
	m := hmac.New(sha1.New, secret)
	m.Write(msg[:])
	sum := m.Sum(nil)
	off := sum[len(sum)-1] & 0x0f
	v := (uint32(sum[off])<<24 | uint32(sum[off+1])<<16 | uint32(sum[off+2])<<8 | uint32(sum[off+3])) & 0x7fffffff
	return fmt.Sprintf("%06d", v%1000000)
}

// totpMatch finds the time step code belongs to: the current one, or the one
// before or after (clocks differ, and typing takes a moment).
func totpMatch(secret []byte, code string, now time.Time) (int64, bool) {
	code = strings.ReplaceAll(strings.TrimSpace(code), " ", "")
	if len(secret) == 0 || len(code) != 6 {
		return 0, false
	}
	cur := now.Unix() / 30
	for _, step := range []int64{cur, cur - 1, cur + 1} {
		if subtle.ConstantTimeCompare([]byte(totpAt(secret, step)), []byte(code)) == 1 {
			return step, true
		}
	}
	return 0, false
}

func normName(s string) string { return strings.ToLower(strings.TrimSpace(s)) }

// locked reports whether name has to wait after too many wrong tries.
func (sv *Server) locked(name string) bool {
	sv.authMu.Lock()
	defer sv.authMu.Unlock()
	f := sv.fails[name]
	return f != nil && sv.now().Before(f.until)
}

func (sv *Server) loginFailed(name string) {
	now := sv.now()
	sv.authMu.Lock()
	defer sv.authMu.Unlock()
	if len(sv.fails) >= maxFails {
		for k, f := range sv.fails {
			if now.After(f.until.Add(loginLock)) {
				delete(sv.fails, k)
			}
		}
		if len(sv.fails) >= maxFails {
			return
		}
	}
	f := sv.fails[name]
	if f == nil {
		f = &failCount{}
		sv.fails[name] = f
	}
	f.n++
	if f.n >= loginFails {
		f.n, f.until = 0, now.Add(loginLock)
		log.Printf("login: %q locked for %s after %d wrong tries", name, loginLock, loginFails)
	} else if f.until.IsZero() {
		f.until = now.Add(-time.Second) // not locked; the entry is cleaned up from here
	}
}

func (sv *Server) loginOK(name string) {
	sv.authMu.Lock()
	delete(sv.fails, name)
	sv.authMu.Unlock()
}

// checkLogin is true if name, password and code belong together. It takes the
// same time whether the account exists or not.
func (sv *Server) checkLogin(name, password, code string) bool {
	u, err := sv.st.User(name)
	usable := err == nil && u.Kind == store.UserLocal && !u.Disabled && u.TOTPOk && len(u.PassHash) > 0 && !sv.locked(name)
	if !usable || len(password) > maxPassword {
		hashPassword(password[:min(len(password), maxPassword)], sv.dummySalt, passIter)
		return false
	}
	if subtle.ConstantTimeCompare(hashPassword(password, u.PassSalt, u.PassIter), u.PassHash) != 1 {
		return false
	}
	now := sv.now()
	return sv.st.UpdateUser(name, func(u *store.User) error {
		step, ok := totpMatch(u.TOTP, code, now)
		if !ok || step <= u.TOTPStep { // wrong, or used before
			return errLogin
		}
		u.TOTPStep, u.LastLogin = step, &now
		return nil
	}) == nil
}

func decode(w http.ResponseWriter, r *http.Request, v any) bool {
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16*1024)).Decode(v); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return false
	}
	return true
}

// ---------------------------------------------------------------- signing in

// methods tells the login page what it can offer, and who is signed in.
func (sv *Server) methods(w http.ResponseWriter, r *http.Request) {
	out := map[string]any{"token": sv.cfg.AdminToken != "", "user": "", "kind": ""}
	if p, ok := sv.authenticate(r); ok {
		out["user"], out["kind"] = p.Name, p.Kind
	}
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) login(w http.ResponseWriter, r *http.Request) {
	if sv.limited(sv.limAuth, w, r) {
		return
	}
	var in struct{ Name, Password, Code string }
	if !decode(w, r, &in) {
		return
	}
	name := normName(in.Name)
	if !sv.checkLogin(name, in.Password, in.Code) {
		sv.loginFailed(name)
		sv.audit(r, "-", "login", name, "", false)
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": errLogin.Error()})
		return
	}
	sv.loginOK(name)
	sv.audit(r, name, "login", name, "", true)
	sv.startSession(w, principal{Name: name, Kind: store.UserLocal})
	writeJSON(w, http.StatusOK, map[string]string{"user": name})
}

func (sv *Server) logout(w http.ResponseWriter, r *http.Request) {
	if key, ok := sv.sessionKey(r); ok {
		sv.authMu.Lock()
		s := sv.web[key]
		delete(sv.web, key)
		sv.authMu.Unlock()
		if s != nil {
			sv.audit(r, s.who.Name, "logout", s.who.Name, "", true)
		}
	}
	sv.setCookie(w, sv.cookieName(), "", -1, http.SameSiteStrictMode)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// setupUser returns the account if code is its setup code and still good.
func (sv *Server) setupUser(name, code string) (*store.User, bool) {
	u, err := sv.st.User(name)
	sum := sha256.Sum256([]byte(strings.ToUpper(strings.ReplaceAll(strings.TrimSpace(code), "-", ""))))
	if err != nil || u.Kind != store.UserLocal || u.Disabled || u.SetupExpires == nil || sv.now().After(*u.SetupExpires) ||
		sv.locked(name) || subtle.ConstantTimeCompare(sum[:], u.SetupHash) != 1 {
		return nil, false
	}
	return u, true
}

// setup is the first half of setting up an account: the owner shows the setup
// code, chooses a password and gets the secret for the authenticator app.
func (sv *Server) setup(w http.ResponseWriter, r *http.Request) {
	if sv.limited(sv.limAuth, w, r) {
		return
	}
	var in struct{ Name, Setup, Password string }
	if !decode(w, r, &in) {
		return
	}
	name := normName(in.Name)
	if _, ok := sv.setupUser(name, in.Setup); !ok {
		sv.loginFailed(name)
		sv.audit(r, "-", "setup", name, "", false)
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "name or setup code is wrong, or the code has expired"})
		return
	}
	if len(in.Password) < minPassword || len(in.Password) > maxPassword {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": fmt.Sprintf("password: at least %d characters", minPassword)})
		return
	}
	salt, secret := make([]byte, 16), make([]byte, 20)
	rand.Read(salt)
	rand.Read(secret)
	hash := hashPassword(in.Password, salt, passIter)
	if err := sv.st.UpdateUser(name, func(u *store.User) error {
		u.PassSalt, u.PassHash, u.PassIter = salt, hash, passIter
		u.TOTP, u.TOTPOk, u.TOTPStep = secret, false, 0
		return nil
	}); err != nil {
		fail(w, err)
		return
	}
	s := b32.EncodeToString(secret)
	uri := "otpauth://totp/" + url.PathEscape("RS232-Leitstelle:"+name) + "?secret=" + s + "&issuer=RS232-Leitstelle"
	writeJSON(w, http.StatusOK, map[string]string{"secret": s, "uri": uri})
}

// setupConfirm is the second half: a code from the authenticator app proves
// that it has the secret. Only then does the account work, and the setup code
// stops working.
func (sv *Server) setupConfirm(w http.ResponseWriter, r *http.Request) {
	if sv.limited(sv.limAuth, w, r) {
		return
	}
	var in struct{ Name, Setup, Code string }
	if !decode(w, r, &in) {
		return
	}
	name := normName(in.Name)
	now := sv.now()
	_, ok := sv.setupUser(name, in.Setup)
	if ok {
		ok = sv.st.UpdateUser(name, func(u *store.User) error {
			step, match := totpMatch(u.TOTP, in.Code, now)
			if !match || len(u.PassHash) == 0 {
				return errLogin
			}
			u.TOTPOk, u.TOTPStep, u.LastLogin = true, step, &now
			u.SetupHash, u.SetupExpires = nil, nil
			return nil
		}) == nil
	}
	if !ok {
		sv.loginFailed(name)
		sv.audit(r, "-", "setup", name, "", false)
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "the code does not match"})
		return
	}
	sv.loginOK(name)
	sv.audit(r, name, "setup", name, "", true)
	sv.startSession(w, principal{Name: name, Kind: store.UserLocal})
	writeJSON(w, http.StatusOK, map[string]string{"user": name})
}

// ---------------------------------------------------------------- accounts

type userView struct {
	Name      string     `json:"name"`
	Kind      string     `json:"kind"`
	Disabled  bool       `json:"disabled"`
	Ready     bool       `json:"ready"`         // can sign in
	Setup     *time.Time `json:"setup_expires"` // a setup code is out until then
	Created   time.Time  `json:"created"`
	LastLogin *time.Time `json:"last_login,omitempty"`
	Role      string     `json:"role"` // "admin" or "member"
}

func roleName(role string) string {
	if role == store.RoleMember {
		return "member"
	}
	return "admin"
}

func (sv *Server) listUsers(w http.ResponseWriter, _ *http.Request) {
	out := []userView{}
	now := sv.now()
	for _, u := range sv.st.Users() {
		v := userView{Name: u.Name, Kind: u.Kind, Disabled: u.Disabled,
			Created: u.Created, LastLogin: u.LastLogin, Role: roleName(u.Role)}
		v.Ready = !u.Disabled && u.Kind == store.UserLocal && u.TOTPOk
		if u.SetupExpires != nil && now.Before(*u.SetupExpires) {
			v.Setup = u.SetupExpires
		}
		out = append(out, v)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	writeJSON(w, http.StatusOK, out)
}

// newSetupCode makes the code an account's owner sets it up with: 80 bits,
// written in groups of four.
func newSetupCode() (code string, hash []byte) {
	raw := make([]byte, 10)
	rand.Read(raw)
	plain := b32.EncodeToString(raw)
	sum := sha256.Sum256([]byte(plain))
	return plain[:4] + "-" + plain[4:8] + "-" + plain[8:12] + "-" + plain[12:], sum[:]
}

func (sv *Server) addUser(w http.ResponseWriter, r *http.Request) {
	var in struct{ Name, Kind, Role string }
	if !decode(w, r, &in) {
		return
	}
	u := &store.User{Name: normName(in.Name), Kind: in.Kind}
	if in.Role == "member" {
		u.Role = store.RoleMember
	}
	note(r).target = u.Name
	note(r).detail = in.Kind
	if !userNames.MatchString(u.Name) || u.Name == kindToken {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "name: 2 to 64 characters from a–z, 0–9, dot, hyphen, underscore, @"})
		return
	}
	out := map[string]any{"name": u.Name, "kind": u.Kind}
	switch u.Kind {
	case store.UserLocal:
		code, hash := newSetupCode()
		exp := sv.now().Add(setupTTL)
		u.SetupHash, u.SetupExpires = hash, &exp
		out["setup"], out["setup_expires"] = code, exp
	default:
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "kind: local"})
		return
	}
	if err := sv.st.AddUser(u); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, out)
}

// roleUser makes an account an administrator or a member of groups only.
// Nobody changes the own role: the last administrator cannot lock everyone out.
func (sv *Server) roleUser(w http.ResponseWriter, r *http.Request) {
	var in struct{ Role string }
	if !decode(w, r, &in) {
		return
	}
	name := r.PathValue("name")
	note(r).detail = in.Role
	if in.Role != "admin" && in.Role != "member" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "role: admin or member"})
		return
	}
	if p := who(r); p.Kind != kindToken && p.Name == name {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "nobody changes the own role"})
		return
	}
	err := sv.st.UpdateUser(name, func(u *store.User) error {
		u.Role = store.RoleAdmin
		if in.Role == "member" {
			u.Role = store.RoleMember
		}
		return nil
	})
	if err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// me tells the page who is signed in and what that account may see.
func (sv *Server) me(w http.ResponseWriter, r *http.Request) {
	p := who(r)
	writeJSON(w, http.StatusOK, map[string]any{"name": p.Name, "kind": p.Kind, "admin": p.Role == store.RoleAdmin})
}

// resetUser takes password and authenticator away from a local account and
// hands out a new setup code: for a lost phone or a forgotten password.
func (sv *Server) resetUser(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	code, hash := newSetupCode()
	exp := sv.now().Add(setupTTL)
	err := sv.st.UpdateUser(name, func(u *store.User) error {
		if u.Kind != store.UserLocal {
			return store.ErrState
		}
		u.PassSalt, u.PassHash, u.PassIter, u.TOTP, u.TOTPOk, u.TOTPStep = nil, nil, 0, nil, false, 0
		u.SetupHash, u.SetupExpires = hash, &exp
		return nil
	})
	if err != nil {
		fail(w, err)
		return
	}
	sv.endSessions(name)
	sv.loginOK(name)
	writeJSON(w, http.StatusOK, map[string]any{"name": name, "setup": code, "setup_expires": exp})
}

func (sv *Server) disableUser(w http.ResponseWriter, r *http.Request) {
	var in struct{ Disabled bool }
	if !decode(w, r, &in) {
		return
	}
	name := r.PathValue("name")
	note(r).detail = "disabled=" + strconv.FormatBool(in.Disabled)
	if name == who(r).Name && who(r).Kind != kindToken {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "your own account cannot be disabled"})
		return
	}
	if err := sv.st.UpdateUser(name, func(u *store.User) error { u.Disabled = in.Disabled; return nil }); err != nil {
		fail(w, err)
		return
	}
	if in.Disabled {
		sv.endSessions(name)
	}
	writeJSON(w, http.StatusOK, map[string]bool{"disabled": in.Disabled})
}

func (sv *Server) deleteUser(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if name == who(r).Name && who(r).Kind != kindToken {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "your own account cannot be deleted"})
		return
	}
	if err := sv.st.DeleteUser(name); err != nil {
		fail(w, err)
		return
	}
	sv.endSessions(name)
	writeJSON(w, http.StatusOK, map[string]bool{"deleted": true})
}

func (sv *Server) auditLog(w http.ResponseWriter, r *http.Request) {
	n, _ := strconv.Atoi(r.URL.Query().Get("n"))
	if n <= 0 || n > 1000 {
		n = 200
	}
	writeJSON(w, http.StatusOK, sv.st.AuditLog(n))
}
