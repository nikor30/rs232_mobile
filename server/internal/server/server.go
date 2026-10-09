// Package server is the HTTP face of the command-and-control server: three
// endpoints for devices (binary, speaking internal/proto) and a small JSON API
// for the administrator.
package server

import (
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"slices"
	"sort"
	"sync"
	"time"

	"rs232c2/internal/proto"
	"rs232c2/internal/store"
)

type Config struct {
	AdminToken string   // bearer token of the admin API
	PublicURL  string   // how devices reach this server; goes into enrollment tokens
	Commands   []string // command types an administrator may queue
	PollS      int      // seconds a device should wait between polls
}

const (
	sessionTTL  = time.Hour        // then the device has to shake hands again (fresh keys)
	unauthTTL   = 30 * time.Second // a handshake nobody followed up on
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
	expires  time.Time
}

type Server struct {
	cfg   Config
	st    *store.Store
	now   func() time.Time
	mu    sync.Mutex
	sess  map[[proto.IDSize]byte]*session
	sweep time.Time
}

func New(cfg Config, st *store.Store) *Server {
	if cfg.PollS <= 0 {
		cfg.PollS = 30
	}
	if cfg.Commands == nil {
		cfg.Commands = []string{"ping", "status"}
	}
	return &Server{cfg: cfg, st: st, now: time.Now, sess: map[[proto.IDSize]byte]*session{}}
}

// SetClock replaces the time source (tests).
func (sv *Server) SetClock(now func() time.Time) { sv.now = now; sv.st.SetClock(now) }

func (sv *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) { io.WriteString(w, "ok\n") })
	mux.HandleFunc("GET /v1/server-key", sv.serverKey)
	mux.HandleFunc("POST /v1/handshake", sv.handshake)
	mux.HandleFunc("POST /v1/msg", sv.msg)
	mux.HandleFunc("POST /admin/v1/enroll-tokens", sv.admin(sv.newToken))
	mux.HandleFunc("GET /admin/v1/devices", sv.admin(sv.listDevices))
	mux.HandleFunc("POST /admin/v1/devices/{id}/approve", sv.admin(sv.approve))
	mux.HandleFunc("POST /admin/v1/devices/{id}/revoke", sv.admin(sv.revoke))
	mux.HandleFunc("POST /admin/v1/devices/{id}/commands", sv.admin(sv.enqueue))
	mux.HandleFunc("GET /admin/v1/devices/{id}/commands", sv.admin(sv.listCommands))
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

func binary(w http.ResponseWriter, b []byte) {
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Write(b)
}

// The key is public. A device trusts it only if it matches the fingerprint in
// its enrollment token.
func (sv *Server) serverKey(w http.ResponseWriter, _ *http.Request) {
	binary(w, sv.st.ServerKey().Public())
}

// Devices get no explanation for a refusal: 400 malformed, 403 not welcome.
func (sv *Server) handshake(w http.ResponseWriter, r *http.Request) {
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
	}
	if err != nil {
		log.Printf("handshake %s refused: %v", e.deviceID, err)
		http.Error(w, "", http.StatusForbidden)
		return
	}
	resp, s, err := proto.ServerHandshake(sv.st.ServerKey(), req, devPub, psk)
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
	T       string          `json:"t"` // "enroll" or "poll"
	Name    string          `json:"name,omitempty"`
	Info    json.RawMessage `json:"info,omitempty"`
	Status  json.RawMessage `json:"status,omitempty"`
	Results []store.Result  `json:"results,omitempty"`
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
	default:
		http.Error(w, "", http.StatusBadRequest)
		return
	}
	b, _ := json.Marshal(out)
	rec, err := e.s.Seal(b)
	if err != nil {
		http.Error(w, "", http.StatusInternalServerError)
		return
	}
	binary(w, rec)
}

// ---------------------------------------------------------------- admin API

func (sv *Server) admin(h http.HandlerFunc) http.HandlerFunc {
	want := []byte("Bearer " + sv.cfg.AdminToken)
	return func(w http.ResponseWriter, r *http.Request) {
		if subtle.ConstantTimeCompare([]byte(r.Header.Get("Authorization")), want) != 1 {
			w.Header().Set("WWW-Authenticate", "Bearer")
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, 64*1024)
		h(w, r)
	}
}

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
	case errors.Is(err, store.ErrState), errors.Is(err, store.ErrFull):
		code = http.StatusConflict
	case errors.Is(err, store.ErrSAS):
		code = http.StatusForbidden
	}
	writeJSON(w, code, map[string]string{"error": err.Error()})
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
}

func (sv *Server) listDevices(w http.ResponseWriter, _ *http.Request) {
	out := []deviceView{}
	for _, d := range sv.st.Devices() {
		fp := proto.Fingerprint(d.Pub)
		v := deviceView{ID: d.ID, Fingerprint: hex.EncodeToString(fp[:]), Name: d.Name, State: d.State,
			Info: d.Info, Enrolled: d.Enrolled, LastSeen: d.LastSeen, Status: d.Status}
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
	if !slices.Contains(sv.cfg.Commands, in.Type) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "unknown command type"})
		return
	}
	if len(in.Args) > 4096 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "args too large"})
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
