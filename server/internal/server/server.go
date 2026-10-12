// Package server is the HTTP face of the command-and-control server: three
// endpoints for devices (binary, speaking internal/proto) and a small JSON API
// for the administrator.
package server

import (
	"bytes"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"mime"
	"net/http"
	"path"
	"slices"
	"sort"
	"strconv"
	"sync"
	"time"

	"rs232c2/internal/proto"
	"rs232c2/internal/store"
)

type Config struct {
	AdminToken string   // bearer token of the admin API; empty: accounts only
	PublicURL  string   // how devices reach this server; goes into enrollment tokens
	Commands   []string // command types an administrator may queue
	PollS      int      // seconds a device should wait between polls
	// SecureCookies marks the session cookie as HTTPS-only. Off only where the
	// browser really talks plain HTTP to this server (tests).
	SecureCookies bool
	// TrustProxy takes the client's address from X-Forwarded-For. Only behind
	// a reverse proxy that sets it; otherwise anybody could choose an address.
	TrustProxy bool
	Entra      *EntraConfig // sign-in with Microsoft Entra ID, if set
}

const (
	sessionTTL = time.Hour // then the device has to shake hands again (fresh keys)
	// A handshake nobody followed up on. Not shorter: on a weak WLAN a device
	// needs several tries of 20 s each to get its first message through.
	unauthTTL   = 2 * time.Minute
	maxSessions = 4096
)

type session struct {
	mu       sync.Mutex
	s        *proto.Session
	deviceID string
	pub      []byte // the device's static key this session was built with
	tokenID  string // enrollment handshakes only
	enroll   bool   // still waiting for the "enroll" message
	authed   bool   // the device has proved its key with a valid record
	oldKey   bool   // made with the server's previous key: the device has to be told the new one
	expires  time.Time
}

// What an administrator can ask of a device. The device has the last word: it
// carries out only what its own list knows, and "play" and "reboot" only if
// that was allowed at the device itself.
var commandArgs = map[string]func(args json.RawMessage) error{
	"ping":     noArgs,
	"status":   noArgs, // ports, counters, network, battery
	"settings": noArgs, // settings without passwords or keys
	"configs":  noArgs, // stored configurations
	"reboot":   noArgs,
	// Remote console on a serial port. Somebody at the device has to agree on
	// its touch display, each time; minutes is the most they are asked for.
	"console": func(args json.RawMessage) error {
		var a struct {
			Port    int `json:"port"`
			Minutes int `json:"minutes"`
		}
		dec := json.NewDecoder(bytes.NewReader(args))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&a); err != nil {
			return errors.New(`console needs {"port": 1, "minutes": 15}`)
		}
		if a.Port < 1 || a.Port > 4 {
			return errors.New("port: 1 to 4")
		}
		if a.Minutes < 1 || a.Minutes > 60 {
			return errors.New("minutes: 1 to 60")
		}
		return nil
	},
	"play": func(args json.RawMessage) error { // play a stored configuration to a serial port
		var a struct {
			Name string `json:"name"`
			Port int    `json:"port"`
		}
		dec := json.NewDecoder(bytes.NewReader(args))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&a); err != nil {
			return errors.New(`play needs {"name": "...", "port": 1}`)
		}
		if a.Name == "" || len(a.Name) > 48 {
			return errors.New("name: 1 to 48 characters")
		}
		if a.Port < 1 || a.Port > 4 {
			return errors.New("port: 1 to 4")
		}
		return nil
	},
}

func noArgs(args json.RawMessage) error {
	if t := bytes.TrimSpace(args); len(t) != 0 && string(t) != "null" && string(t) != "{}" {
		return errors.New("this command takes no arguments")
	}
	return nil
}

// AllCommands lists every command type the server knows.
func AllCommands() []string {
	out := make([]string, 0, len(commandArgs))
	for k := range commandArgs {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

type Server struct {
	cfg   Config
	st    *store.Store
	now   func() time.Time
	mu    sync.Mutex
	sess  map[[proto.IDSize]byte]*session
	cons  map[string]*console // remote consoles by device ID, open or just ended
	sweep time.Time

	authMu    sync.Mutex
	web       map[[32]byte]*webSession // signed-in browsers, by the hash of their cookie
	fails     map[string]*failCount    // wrong sign-ins, by account name
	dummySalt []byte
	entra     *entra
	// Requests per client address. A handshake costs the server a key
	// exchange, a sign-in a password hash; neither is free for the asking.
	limHandshake, limAuth, limFail *limiter

	started   time.Time
	healthMu  sync.Mutex
	healthAt  time.Time // when the state was last checked
	healthErr error
}

func New(cfg Config, st *store.Store) *Server {
	if cfg.PollS <= 0 {
		cfg.PollS = 30
	}
	if cfg.Commands == nil {
		cfg.Commands = AllCommands()
	}
	sv := &Server{cfg: cfg, st: st, now: time.Now, sess: map[[proto.IDSize]byte]*session{}, cons: map[string]*console{},
		web: map[[32]byte]*webSession{}, fails: map[string]*failCount{}, dummySalt: make([]byte, 16),
		limHandshake: newLimiter(30, time.Second), limAuth: newLimiter(10, 6*time.Second), limFail: newLimiter(30, 2*time.Second)}
	rand.Read(sv.dummySalt)
	sv.started = time.Now()
	if cfg.Entra != nil {
		sv.entra = newEntra(*cfg.Entra, cfg.PublicURL)
	}
	return sv
}

// SetClock replaces the time source (tests).
func (sv *Server) SetClock(now func() time.Time) { sv.now = now; sv.st.SetClock(now) }

func (sv *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", sv.healthz)
	mux.HandleFunc("GET /v1/server-key", sv.serverKey)
	mux.HandleFunc("POST /v1/handshake", sv.handshake)
	mux.HandleFunc("POST /v1/msg", sv.msg)
	mux.HandleFunc("GET /{$}", sv.ui("index.html"))
	mux.HandleFunc("GET /ui/{file}", func(w http.ResponseWriter, r *http.Request) { sv.ui(r.PathValue("file"))(w, r) })
	mux.HandleFunc("GET /auth/methods", sv.methods)
	mux.HandleFunc("POST /auth/login", sv.login)
	mux.HandleFunc("POST /auth/logout", sv.logout)
	mux.HandleFunc("POST /auth/setup", sv.setup)
	mux.HandleFunc("POST /auth/setup/confirm", sv.setupConfirm)
	mux.HandleFunc("GET /auth/entra/login", sv.entraLogin)
	mux.HandleFunc("GET /auth/entra/callback", sv.entraCallback)
	mux.HandleFunc("GET /admin/v1/users", sv.admin(sv.listUsers))
	mux.HandleFunc("POST /admin/v1/users", sv.admin(sv.addUser))
	mux.HandleFunc("POST /admin/v1/users/{name}/reset", sv.admin(sv.resetUser))
	mux.HandleFunc("POST /admin/v1/users/{name}/disable", sv.admin(sv.disableUser))
	mux.HandleFunc("DELETE /admin/v1/users/{name}", sv.admin(sv.deleteUser))
	mux.HandleFunc("GET /admin/v1/audit", sv.admin(sv.auditLog))
	mux.HandleFunc("GET /admin/v1/server", sv.admin(sv.serverInfo))
	mux.HandleFunc("GET /admin/v1/status", sv.admin(sv.status))
	mux.HandleFunc("POST /admin/v1/server/rotate-key", sv.admin(sv.rotateKey))
	mux.HandleFunc("POST /admin/v1/server/rotate-key/end", sv.admin(sv.endKeyChange))
	mux.HandleFunc("POST /admin/v1/enroll-tokens", sv.admin(sv.newToken))
	mux.HandleFunc("GET /admin/v1/devices", sv.admin(sv.listDevices))
	mux.HandleFunc("POST /admin/v1/devices/{id}/approve", sv.admin(sv.approve))
	mux.HandleFunc("POST /admin/v1/devices/{id}/revoke", sv.admin(sv.revoke))
	mux.HandleFunc("POST /admin/v1/devices/{id}/commands", sv.admin(sv.enqueue))
	mux.HandleFunc("GET /admin/v1/devices/{id}/commands", sv.admin(sv.listCommands))
	mux.HandleFunc("GET /admin/v1/devices/{id}/console", sv.admin(sv.conStatus))
	mux.HandleFunc("GET /admin/v1/devices/{id}/console/{sess}/output", sv.admin(sv.conOutput))
	mux.HandleFunc("POST /admin/v1/devices/{id}/console/{sess}/input", sv.admin(sv.conInput))
	mux.HandleFunc("POST /admin/v1/devices/{id}/console/{sess}/close", sv.admin(sv.conClose))
	return mux
}

// ---------------------------------------------------------------- devices

func readBody(w http.ResponseWriter, r *http.Request, max int64) ([]byte, bool) {
	b, err := io.ReadAll(http.MaxBytesReader(w, r.Body, max))
	if err != nil {
		http.Error(w, "", http.StatusRequestEntityTooLarge)
		return nil, false
	}
	return b, true
}

// The length is always stated. Left alone, net/http switches to chunked
// encoding above 2 kB, and a device should not need a chunk parser.
func binary(w http.ResponseWriter, b []byte) {
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	w.Write(b)
}

// The key is public. A device trusts it only if it matches the fingerprint in
// its enrollment token.
func (sv *Server) serverKey(w http.ResponseWriter, _ *http.Request) {
	binary(w, sv.st.ServerKey().Public())
}

// Devices get no explanation for a refusal: 400 malformed, 403 not welcome.
func (sv *Server) handshake(w http.ResponseWriter, r *http.Request) {
	if sv.limited(sv.limHandshake, w, r) {
		return
	}
	req, ok := readBody(w, r, proto.ReqEnroll)
	if !ok {
		return
	}
	h, err := proto.ParseHello(req)
	if err != nil {
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	e := &session{deviceID: proto.IDString(h.DeviceID)}
	var devPub, psk []byte
	key := sv.st.ServerKey()
	if h.Kind == proto.KindEnroll {
		e.enroll, e.tokenID = true, hex.EncodeToString(h.TokenID[:])
		devPub = h.DevicePub
		psk, err = sv.st.TokenSecret(e.tokenID)
	} else {
		var d *store.Device
		if d, err = sv.st.Device(e.deviceID); err == nil {
			devPub = d.Pub
			if d.State == store.StateRevoked {
				err = store.ErrState
			}
		}
		if err == nil { // during a key change: the key this device still expects
			key, e.oldKey, err = sv.st.KeyFor(e.deviceID)
		}
	}
	if err != nil {
		log.Printf("handshake %s refused: %v", e.deviceID, err)
		http.Error(w, "", http.StatusForbidden)
		return
	}
	resp, s, err := proto.ServerHandshake(key, req, devPub, psk)
	if err != nil {
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	e.s, e.pub = s, devPub
	now := sv.now()
	e.expires = now.Add(unauthTTL)
	sv.mu.Lock()
	if now.After(sv.sweep) {
		for id, x := range sv.sess {
			if now.After(x.expires) {
				delete(sv.sess, id)
			}
		}
		sv.sweep = now.Add(10 * time.Second)
	}
	full := len(sv.sess) >= maxSessions
	if !full {
		sv.sess[s.ID] = e
	}
	sv.mu.Unlock()
	if full {
		http.Error(w, "", http.StatusServiceUnavailable)
		return
	}
	binary(w, resp)
}

type deviceMsg struct {
	T       string          `json:"t"` // "enroll", "poll" or "con"
	Name    string          `json:"name,omitempty"`
	Info    json.RawMessage `json:"info,omitempty"`
	Status  json.RawMessage `json:"status,omitempty"`
	Results []store.Result  `json:"results,omitempty"`
	SK      string          `json:"sk,omitempty"` // "poll": the server key the device holds (store.KeyTag)
	conMsg                  // "con": the remote console
}

type serverCmd struct {
	ID   uint64          `json:"id"`
	Type string          `json:"type"`
	Args json.RawMessage `json:"args,omitempty"`
}

type serverMsg struct {
	State string      `json:"state"`
	Cmds  []serverCmd `json:"cmds,omitempty"`
	PollS int         `json:"poll_s"`
	Rekey []byte      `json:"rekey,omitempty"` // the server's new public key (base64), to a device that is still on the old one
}

func (sv *Server) drop(id [proto.IDSize]byte) {
	sv.mu.Lock()
	delete(sv.sess, id)
	sv.mu.Unlock()
}

// 401 tells the device to shake hands again; 403 that it is not welcome.
func (sv *Server) msg(w http.ResponseWriter, r *http.Request) {
	body, ok := readBody(w, r, proto.IDSize+8+proto.MaxPlaintext+16)
	if !ok {
		return
	}
	if len(body) < proto.IDSize {
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	var sid [proto.IDSize]byte
	copy(sid[:], body)
	sv.mu.Lock()
	e := sv.sess[sid]
	sv.mu.Unlock()
	if e == nil || sv.now().After(e.expires) {
		http.Error(w, "", http.StatusUnauthorized)
		return
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	pt, err := e.s.Open(body[proto.IDSize:])
	if err != nil {
		if !e.authed { // whoever shook hands does not have the device's key
			sv.drop(sid)
		}
		http.Error(w, "", http.StatusUnauthorized)
		return
	}
	if !e.authed {
		e.authed = true
		e.expires = sv.now().Add(sessionTTL)
	}
	var m deviceMsg
	if err := json.Unmarshal(pt, &m); err != nil {
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	out := serverMsg{PollS: sv.cfg.PollS}
	var reply any = &out
	switch {
	case e.enroll && m.T == "enroll":
		if len(m.Name) > 64 || len(m.Info) > 2048 {
			http.Error(w, "", http.StatusBadRequest)
			return
		}
		if err := sv.st.Enroll(e.tokenID, e.pub, m.Name, m.Info, e.s.SAS()); err != nil {
			log.Printf("enroll %s refused: %v", e.deviceID, err)
			sv.drop(sid)
			http.Error(w, "", http.StatusForbidden)
			return
		}
		e.enroll = false
		out.State = store.StatePending
		log.Printf("device %s (%q) enrolled, waiting for approval", e.deviceID, m.Name)
	case !e.enroll && m.T == "poll":
		if e.oldKey {
			// This session was made with the old server key, so the device can
			// believe what it is told here about the new one. Once it says it
			// has it, the server answers its handshakes with the new key.
			out.Rekey = sv.st.ServerKey().Public()
			if m.SK != "" {
				if err := sv.st.ConfirmKey(e.deviceID, m.SK); err != nil {
					log.Printf("key change of %s: %v", e.deviceID, err)
				} else if m.SK == store.KeyTag(out.Rekey) {
					log.Printf("device %s has the new server key", e.deviceID)
					e.oldKey, out.Rekey = false, nil
				}
			}
		}
		state, cmds, err := sv.st.Poll(e.deviceID, m.Status, m.Results)
		if err != nil || state == store.StateRevoked {
			sv.drop(sid)
			http.Error(w, "", http.StatusForbidden)
			return
		}
		out.State = state
		for _, c := range cmds {
			out.Cmds = append(out.Cmds, serverCmd{c.ID, c.Type, c.Args})
		}
	case !e.enroll && m.T == "con":
		if d, err := sv.st.Device(e.deviceID); err != nil || d.State != store.StateActive {
			sv.drop(sid)
			http.Error(w, "", http.StatusForbidden)
			return
		}
		r, err := sv.conDevice(e.deviceID, &m.conMsg)
		if err != nil {
			http.Error(w, "", http.StatusBadRequest)
			return
		}
		reply = r
	default:
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	b, _ := json.Marshal(reply)
	rec, err := e.s.Seal(b)
	if err != nil {
		http.Error(w, "", http.StatusInternalServerError)
		return
	}
	binary(w, rec)
}

// ---------------------------------------------------------------- admin API

// The guard in front of these handlers is admin() in auth.go.

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, err error) {
	code := http.StatusInternalServerError
	switch {
	case errors.Is(err, store.ErrNotFound):
		code = http.StatusNotFound
	case errors.Is(err, store.ErrState), errors.Is(err, store.ErrFull), errors.Is(err, store.ErrBusy), errors.Is(err, store.ErrExists):
		code = http.StatusConflict
	case errors.Is(err, store.ErrSAS):
		code = http.StatusForbidden
	}
	writeJSON(w, code, map[string]string{"error": err.Error()})
}

//go:embed web
var webFiles embed.FS

// ui serves one file of the administrator's web interface. The page itself is
// public and holds no data; everything it shows comes from the admin API, with
// the token the administrator types in.
func (sv *Server) ui(name string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		b, err := webFiles.ReadFile("web/" + path.Base(name))
		if err != nil {
			http.NotFound(w, r)
			return
		}
		h := w.Header()
		h.Set("Content-Type", mime.TypeByExtension(path.Ext(name)))
		h.Set("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("Cache-Control", "no-cache")
		w.Write(b)
	}
}

func (sv *Server) serverInfo(w http.ResponseWriter, _ *http.Request) {
	fp := proto.Fingerprint(sv.st.ServerKey().Public())
	writeJSON(w, http.StatusOK, map[string]any{
		"fingerprint": hex.EncodeToString(fp[:]),
		"public_url":  sv.cfg.PublicURL,
		"commands":    sv.cfg.Commands,
		"poll_s":      sv.cfg.PollS,
		"key_change":  sv.keyChange(),
	})
}

// keyChange is what an administrator sees of a change of the server key:
// nil if none is going on.
func (sv *Server) keyChange() map[string]any {
	old, until, waiting := sv.st.KeyChange()
	if old == nil {
		return nil
	}
	fp := proto.Fingerprint(old)
	return map[string]any{"old_fingerprint": hex.EncodeToString(fp[:]), "old_until": until, "devices_waiting": waiting}
}

// rotateKey gives the server a new key (KONZEPT.md §4.6).
func (sv *Server) rotateKey(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Days int `json:"days"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil && err != io.EOF {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	keep := store.KeepOldKeyDefault
	if in.Days != 0 {
		keep = time.Duration(in.Days) * 24 * time.Hour
	}
	switch err := sv.st.RotateKey(keep); {
	case errors.Is(err, store.ErrArgs):
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "days: 1 to 365"})
	case errors.Is(err, store.ErrState):
		writeJSON(w, http.StatusConflict, map[string]string{"error": "a key change is still going on"})
	case err != nil:
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "cannot save"})
	default:
		// Every device shakes hands again, with the key it knows, and hears
		// about the new one in that session.
		sv.mu.Lock()
		clear(sv.sess)
		sv.mu.Unlock()
		log.Print("the server has a new key; devices are told as they come")
		sv.serverInfo(w, r)
	}
}

// endKeyChange forgets the old key before its time is up.
func (sv *Server) endKeyChange(w http.ResponseWriter, r *http.Request) {
	switch err := sv.st.EndKeyChange(); {
	case errors.Is(err, store.ErrState):
		writeJSON(w, http.StatusConflict, map[string]string{"error": "no key change is going on"})
	case err != nil:
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "cannot save"})
	default:
		sv.serverInfo(w, r)
	}
}

// ---------------------------------------------------------------- monitoring

const healthEvery = 10 * time.Second

// Check says whether the server can do its work, which is to say whether its
// state can be written. Anybody may ask (/healthz), so the backend is asked at
// most every healthEvery.
func (sv *Server) Check() error {
	sv.healthMu.Lock()
	defer sv.healthMu.Unlock()
	if now := time.Now(); sv.healthAt.IsZero() || now.Sub(sv.healthAt) >= healthEvery {
		sv.healthAt, sv.healthErr = now, sv.st.Check()
	}
	return sv.healthErr
}

// healthz is for whatever watches the server from outside: 200 or 503, and
// not a word about why - that is in the log and in /admin/v1/status.
func (sv *Server) healthz(w http.ResponseWriter, _ *http.Request) {
	if sv.Check() != nil {
		http.Error(w, "failing", http.StatusServiceUnavailable)
		return
	}
	io.WriteString(w, "ok\n")
}

// silentAfter is how long an active device may stay away before it counts as
// silent: several of its polls, and a handshake that had to be repeated.
func (sv *Server) silentAfter() time.Duration {
	return max(5*time.Duration(sv.cfg.PollS)*time.Second, 5*time.Minute)
}

func (sv *Server) status(w http.ResponseWriter, _ *http.Request) {
	now := sv.now()
	devices := map[string]int{store.StatePending: 0, store.StateActive: 0, store.StateRevoked: 0, "silent": 0, "old_key": 0}
	for _, d := range sv.st.Devices() {
		devices[d.State]++
		if d.OldKey && d.State != store.StateRevoked {
			devices["old_key"]++
		}
		if d.State == store.StateActive && (d.LastSeen == nil || now.Sub(*d.LastSeen) > sv.silentAfter()) {
			devices["silent"]++
		}
	}
	sv.mu.Lock()
	consoles := 0
	for _, c := range sv.cons {
		if c.ended == nil {
			consoles++
		}
	}
	sv.mu.Unlock()
	sv.authMu.Lock()
	signedIn := len(sv.web)
	sv.authMu.Unlock()
	denied := func(l *limiter) int64 {
		l.mu.Lock()
		defer l.mu.Unlock()
		return l.denied
	}
	state := map[string]any{"ok": true}
	sv.Check()
	if since, err := sv.st.Health(); err != nil {
		state = map[string]any{"ok": false, "error": err.Error(), "since": since}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"started":        sv.started.UTC(),
		"uptime_s":       int(time.Since(sv.started) / time.Second),
		"state":          state,
		"devices":        devices,
		"silent_after_s": int(sv.silentAfter() / time.Second),
		"consoles_open":  consoles,
		"signed_in":      signedIn,
		"turned_away":    map[string]int64{"handshake": denied(sv.limHandshake), "sign_in": denied(sv.limAuth), "admin": denied(sv.limFail)},
	})
}

func (sv *Server) newToken(w http.ResponseWriter, r *http.Request) {
	var in struct {
		TTLS int    `json:"ttl_s"`
		Note string `json:"note"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil && err != io.EOF {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	if in.TTLS <= 0 {
		in.TTLS = 600
	}
	if in.TTLS > 24*3600 {
		in.TTLS = 24 * 3600
	}
	t, err := sv.st.NewToken(time.Duration(in.TTLS)*time.Second, in.Note)
	if err != nil {
		fail(w, err)
		return
	}
	id, _ := hex.DecodeString(t.ID)
	fp := proto.Fingerprint(sv.st.ServerKey().Public())
	tok := proto.EnrollToken{URL: sv.cfg.PublicURL, Fingerprint: fp[:], ID: id, Secret: t.Secret}
	writeJSON(w, http.StatusCreated, map[string]any{"token": tok.String(), "expires": t.Expires})
}

// deviceView is what the admin API shows of a device. The confirmation code is
// deliberately missing: it has to be read from the device's own display.
type deviceView struct {
	ID          string          `json:"id"`
	Fingerprint string          `json:"fingerprint"`
	Name        string          `json:"name"`
	State       string          `json:"state"`
	Info        json.RawMessage `json:"info,omitempty"`
	Enrolled    time.Time       `json:"enrolled"`
	LastSeen    *time.Time      `json:"last_seen,omitempty"`
	Status      json.RawMessage `json:"status,omitempty"`
	Open        int             `json:"open_commands"`
	OldKey      bool            `json:"old_key,omitempty"` // has not confirmed the server's new key yet
}

func (sv *Server) listDevices(w http.ResponseWriter, _ *http.Request) {
	out := []deviceView{}
	for _, d := range sv.st.Devices() {
		fp := proto.Fingerprint(d.Pub)
		v := deviceView{ID: d.ID, Fingerprint: hex.EncodeToString(fp[:]), Name: d.Name, State: d.State,
			Info: d.Info, Enrolled: d.Enrolled, LastSeen: d.LastSeen, Status: d.Status, OldKey: d.OldKey && d.State != store.StateRevoked}
		for _, c := range d.Commands {
			if c.State == store.CmdQueued || c.State == store.CmdSent {
				v.Open++
			}
		}
		out = append(out, v)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Enrolled.Before(out[j].Enrolled) })
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) approve(w http.ResponseWriter, r *http.Request) {
	var in struct {
		SAS string `json:"sas"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	if err := sv.st.Approve(r.PathValue("id"), in.SAS); err != nil {
		fail(w, err)
		return
	}
	log.Printf("device %s approved", r.PathValue("id"))
	writeJSON(w, http.StatusOK, map[string]string{"state": store.StateActive})
}

func (sv *Server) revoke(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := sv.st.Revoke(id); err != nil {
		fail(w, err)
		return
	}
	sv.mu.Lock()
	if c := sv.cons[id]; c != nil {
		sv.conEnd(c, "Gerät gesperrt")
	}
	for sid, e := range sv.sess { // its open sessions end now, not at the next handshake
		if e.deviceID == id {
			delete(sv.sess, sid)
		}
	}
	sv.mu.Unlock()
	log.Printf("device %s revoked", id)
	writeJSON(w, http.StatusOK, map[string]string{"state": store.StateRevoked})
}

func (sv *Server) enqueue(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Type string          `json:"type"`
		Args json.RawMessage `json:"args"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	note(r).detail = in.Type
	check := commandArgs[in.Type]
	if check == nil || !slices.Contains(sv.cfg.Commands, in.Type) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "unknown command type"})
		return
	}
	if len(in.Args) > 4096 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "args too large"})
		return
	}
	if err := check(in.Args); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	c, err := sv.st.Enqueue(r.PathValue("id"), in.Type, in.Args)
	if err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, c)
}

func (sv *Server) listCommands(w http.ResponseWriter, r *http.Request) {
	d, err := sv.st.Device(r.PathValue("id"))
	if err != nil {
		fail(w, err)
		return
	}
	cmds := d.Commands
	if cmds == nil {
		cmds = []*store.Command{}
	}
	writeJSON(w, http.StatusOK, cmds)
}
