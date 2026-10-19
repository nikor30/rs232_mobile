package server

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"strconv"
	"time"

	"rs232c2/internal/store"
)

// The remote console: an administrator's browser on one side, a device's
// serial port on the other, this server in between (KONZEPT.md §4.5).
//
// The device asks, as always; while a console is open it asks without pause on
// a connection it keeps open, and the server holds an empty question for a
// moment in case something is typed. Both directions are byte streams with
// offsets, so a question or an answer lost on the way is repeated without
// anything arriving twice.
const (
	conHold      = 200 * time.Millisecond // how long an empty question from the device waits for input
	conChunkIn   = 512                    // bytes of input handed to the device per answer
	conMaxIn     = 64 * 1024              // typed and not yet taken by the device
	conMaxOut    = 256 * 1024             // output kept for the browser
	conPollS     = 5                      // seconds between polls while a console is open (the firmware's least)
	conMaxChunk  = 8 * 1024               // output accepted from the device per message
	conSilence   = 45 * time.Second       // a device that stops asking has lost its link
	conKeep      = 10 * time.Minute       // how long the end of a session stays readable
	conGrantAge  = 2 * time.Minute        // time between the yes at the device and its first console message
	conAdminWait = 20 * time.Second       // long poll of the browser
)

type console struct {
	id       uint64 // the command that asked for it
	deviceID string
	port     int
	opened   time.Time
	lastDev  time.Time
	leftS    int // what the device says is left of the time it was allowed

	out     []byte // output of the serial port, for the browser
	outBase int64  // offset of out[0] in the stream
	in      []byte // input not yet confirmed by the device
	inBase  int64
	brk     int // BREAKs asked for so far

	e2e string // the device's one-time key: the streams are end to end between it and the browser (c2_e2e.h)

	// recording to a text file (recordings.go)
	rec       bool   // switched on: the device records on its SD card, the browser sends the text here
	recFile   string // the server's copy, once it has been switched on
	recHave   int64  // bytes in it
	devRec    string // the device's file, as it reports it
	devRecErr string // or why it records nothing

	closing string // set by the administrator: the device is told with its next question
	ended   *time.Time
	reason  string
	wake    chan struct{} // closed and replaced whenever something changes
}

func (c *console) changed() {
	close(c.wake)
	c.wake = make(chan struct{})
}

type conMsg struct {
	ID     uint64 `json:"id"`
	Off    int64  `json:"off"`  // offset of D in the port's output since the session began
	D      []byte `json:"d"`    // base64 in JSON
	IAck   int64  `json:"iack"` // input taken so far
	LeftS  int    `json:"left_s"`
	End    string `json:"end"`     // the device has closed the session, and why
	E2E    string `json:"e2e"`     // its one-time key, when the browser sent one with the request
	Rec    string `json:"rec"`     // the file on its SD card that the session is recorded to
	RecErr string `json:"rec_err"` // or why it cannot record
}

type conReply struct {
	IOff int64  `json:"ioff"`
	D    []byte `json:"d,omitempty"`
	Brk  int    `json:"brk,omitempty"`
	End  string `json:"end,omitempty"`
	Rec  bool   `json:"rec,omitempty"` // record the session on the SD card
}

// end closes a console. Callers hold sv.mu.
func (sv *Server) conEnd(c *console, reason string) {
	if c.ended != nil {
		return
	}
	now := sv.now()
	c.ended, c.reason = &now, reason
	in, out := c.inBase, c.outBase+int64(len(c.out))
	if err := sv.st.ConsoleClose(c.deviceID, c.id, reason, in, out); err != nil {
		log.Printf("console %d of %s: %v", c.id, c.deviceID, err)
	}
	log.Printf("console %d of %s closed after %s: %s (%d bytes in, %d out)", c.id, c.deviceID,
		now.Sub(c.opened).Round(time.Second), reason, in, out)
	c.changed()
}

// conLive tells whether a console of the device is open or on its way.
func (sv *Server) conLive(deviceID string) bool {
	sv.mu.Lock()
	defer sv.mu.Unlock()
	c := sv.cons[deviceID]
	return c != nil && c.ended == nil
}

// conGet returns the device's console, closing or forgetting it if its time
// is up. Callers hold sv.mu.
func (sv *Server) conGet(deviceID string) *console {
	c := sv.cons[deviceID]
	if c == nil {
		return nil
	}
	now := sv.now()
	if c.ended == nil && now.Sub(c.lastDev) > conSilence {
		sv.conEnd(c, "the device no longer answers")
	}
	if c.ended != nil && now.Sub(*c.ended) > conKeep {
		delete(sv.cons, deviceID)
		return nil
	}
	return c
}

// conDevice handles one console message of a device.
func (sv *Server) conDevice(deviceID string, m *conMsg) (*conReply, error) {
	if len(m.D) > conMaxChunk || m.Off < 0 || m.IAck < 0 {
		return nil, errors.New("bad console message")
	}
	sv.mu.Lock()
	c := sv.conGet(deviceID)
	if c == nil || c.id != m.ID {
		if c != nil && c.ended == nil {
			sv.mu.Unlock()
			return &conReply{End: "eine andere Sitzung ist offen"}, nil
		}
		if m.End != "" { // nothing to open only to close it again
			sv.mu.Unlock()
			return &conReply{End: m.End}, nil
		}
		port, err := sv.st.ConsoleOpen(deviceID, m.ID, conGrantAge)
		if err != nil {
			sv.mu.Unlock()
			return &conReply{End: "dem Server unbekannt"}, nil
		}
		now := sv.now()
		c = &console{id: m.ID, deviceID: deviceID, port: port, opened: now, lastDev: now, wake: make(chan struct{})}
		sv.cons[deviceID] = c
		log.Printf("console %d of %s opened, port %d", c.id, deviceID, port)
	}
	if c.ended != nil {
		reason := c.reason
		sv.mu.Unlock()
		return &conReply{End: reason}, nil
	}
	c.lastDev, c.leftS = sv.now(), m.LeftS
	touched := false
	if c.e2e == "" && m.E2E != "" && len(m.E2E) <= 100 {
		c.e2e = m.E2E // from here on the server carries what it cannot read
		touched = true
		log.Printf("console %d of %s: end to end", c.id, deviceID)
	}
	if c.devRec != m.Rec || c.devRecErr != m.RecErr {
		c.devRec, c.devRecErr = clip(m.Rec, 64), clip(m.RecErr, 80)
		touched = true
	}
	// output: take what is new, skip what a repeated message brings again
	if next := c.outBase + int64(len(c.out)); m.Off <= next && m.Off+int64(len(m.D)) > next {
		c.out = append(c.out, m.D[next-m.Off:]...)
		if over := len(c.out) - conMaxOut; over > 0 {
			c.out = append([]byte(nil), c.out[over:]...)
			c.outBase += int64(over)
		}
		touched = true
	}
	if n := m.IAck - c.inBase; n > 0 && n <= int64(len(c.in)) {
		c.in = c.in[n:]
		c.inBase = m.IAck
	}
	if m.End != "" {
		sv.conEnd(c, m.End)
		sv.mu.Unlock()
		return &conReply{End: m.End}, nil
	}
	if touched {
		c.changed()
	}
	// Nothing came and nothing is waiting: hold the question for a moment.
	// What the administrator types then leaves at once instead of after the
	// device's next round.
	if len(m.D) == 0 && len(c.in) == 0 && c.closing == "" && m.IAck == c.inBase {
		wake, brk, rec := c.wake, c.brk, c.rec
		sv.mu.Unlock()
		t := time.NewTimer(conHold)
	wait:
		for {
			select {
			case <-wake:
				sv.mu.Lock()
				if len(c.in) > 0 || c.closing != "" || c.brk != brk || c.rec != rec || c.ended != nil {
					break wait
				}
				wake = c.wake // only output moved; keep waiting
				sv.mu.Unlock()
			case <-t.C:
				sv.mu.Lock()
				break wait
			}
		}
		t.Stop()
	}
	defer sv.mu.Unlock()
	if c.closing != "" {
		sv.conEnd(c, c.closing)
	}
	if c.ended != nil {
		return &conReply{End: c.reason}, nil
	}
	r := &conReply{IOff: c.inBase, Brk: c.brk, Rec: c.rec}
	r.D = append(r.D, c.in[:min(len(c.in), conChunkIn)]...)
	return r, nil
}

// ---------------------------------------------------------------- administrator

type conView struct {
	ID        uint64     `json:"id"`
	State     string     `json:"state"` // requested | granted | open | ended | refused
	Port      int        `json:"port,omitempty"`
	Opened    *time.Time `json:"opened,omitempty"`
	LeftS     int        `json:"left_s,omitempty"`
	Reason    string     `json:"reason,omitempty"`
	E2E       string     `json:"e2e,omitempty"` // the device's one-time key: the browser derives the session from it
	Rec       bool       `json:"rec,omitempty"` // recorded to a text file (recordings.go)
	RecHave   int64      `json:"rec_have,omitempty"`
	DevRec    string     `json:"dev_rec,omitempty"`
	DevRecErr string     `json:"dev_rec_err,omitempty"`
}

// conStatus tells the administrator where the remote console of a device
// stands, and lists the sessions there have been.
func (sv *Server) conStatus(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	var cur *conView
	sv.mu.Lock()
	if c := sv.conGet(id); c != nil {
		cur = &conView{ID: c.id, State: "open", Port: c.port, Opened: &c.opened, LeftS: c.leftS, E2E: c.e2e,
			Rec: c.rec, RecHave: c.recHave, DevRec: c.devRec, DevRecErr: c.devRecErr}
		if c.ended != nil {
			cur.State, cur.Reason, cur.LeftS = "ended", c.reason, 0
		}
	}
	sv.mu.Unlock()
	d, err := sv.st.Device(id) // after conGet, which may just have closed a session
	if err != nil {
		fail(w, err)
		return
	}
	// the most recent request counts if it is newer than the session we know
	for i := len(d.Commands) - 1; i >= 0; i-- {
		c := d.Commands[i]
		if c.Type != store.CmdConsole {
			continue
		}
		if cur != nil && c.ID <= cur.ID {
			break
		}
		var a struct {
			Port int `json:"port"`
		}
		json.Unmarshal(c.Args, &a)
		v := &conView{ID: c.ID, Port: a.Port}
		switch c.State {
		case store.CmdQueued, store.CmdSent:
			v.State = "requested"
		case store.CmdDone:
			v.State = "granted"
			for _, l := range d.Consoles {
				if l.ID == c.ID {
					v.State, v.Reason = "ended", l.Reason
				}
			}
			if v.State == "granted" && sv.now().Sub(*c.Done) > conGrantAge {
				v.State, v.Reason = "ended", "the device did not open the session"
			}
		default:
			v.State = "refused"
			if c.State == store.CmdExpired {
				v.Reason = "no answer from the device"
			} else if json.Unmarshal(c.Result, &v.Reason) != nil {
				v.Reason = string(c.Result)
			}
		}
		cur = v
		break
	}
	logs := d.Consoles
	if logs == nil {
		logs = []*store.ConsoleLog{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"console": cur, "sessions": logs})
}

// conFor finds the open console a request names. On failure it has answered.
// The caller gets it with sv.mu held.
func (sv *Server) conFor(w http.ResponseWriter, r *http.Request, mustBeOpen bool) *console {
	sess, _ := strconv.ParseUint(r.PathValue("sess"), 10, 64)
	sv.mu.Lock()
	c := sv.conGet(r.PathValue("id"))
	if c == nil || c.id != sess {
		sv.mu.Unlock()
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such console session"})
		return nil
	}
	if mustBeOpen && c.ended != nil {
		sv.mu.Unlock()
		writeJSON(w, http.StatusConflict, map[string]string{"error": "the session has ended: " + c.reason})
		return nil
	}
	return c
}

// conOutput is the browser's long poll: what the serial port has sent from
// offset "from" on. It answers at once if there is something, and otherwise
// waits for it.
func (sv *Server) conOutput(w http.ResponseWriter, r *http.Request) {
	from, _ := strconv.ParseInt(r.URL.Query().Get("from"), 10, 64)
	wait := conAdminWait
	if s, err := strconv.Atoi(r.URL.Query().Get("wait")); err == nil && s >= 0 && time.Duration(s)*time.Second < wait {
		wait = time.Duration(s) * time.Second
	}
	c := sv.conFor(w, r, false)
	if c == nil {
		return
	}
	t := time.NewTimer(wait)
	defer t.Stop()
	for c.ended == nil && from >= c.outBase+int64(len(c.out)) {
		wake := c.wake
		sv.mu.Unlock()
		select {
		case <-wake:
		case <-t.C:
			wait = 0
		case <-r.Context().Done():
			return
		}
		sv.mu.Lock()
		sv.conGet(c.deviceID) // notices a device that has gone silent
		if wait == 0 {
			break
		}
	}
	defer sv.mu.Unlock()
	out := c.recView()
	out["state"], out["left_s"], out["port"] = "open", c.leftS, c.port
	if from < c.outBase { // the browser was away for longer than the buffer holds
		out["lost"] = c.outBase - from
		from = c.outBase
	}
	end := c.outBase + int64(len(c.out))
	if from > end {
		from = end
	}
	out["from"], out["next"] = from, end
	out["d"] = c.out[from-c.outBase:]
	if c.ended != nil {
		out["state"], out["reason"] = "ended", c.reason
	}
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) conInput(w http.ResponseWriter, r *http.Request) {
	var in struct {
		D   []byte `json:"d"`
		Brk bool   `json:"brk"`
		At  *int64 `json:"at"` // where D goes in the input stream: a request sent again is taken once
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	note(r).skip = true // keystrokes: the session is on record, not every one of them
	c := sv.conFor(w, r, true)
	if c == nil {
		return
	}
	defer sv.mu.Unlock()
	if in.At != nil && len(in.D) > 0 {
		// The browser does not know whether a request whose answer was lost got
		// here. What is here already is confirmed again; a gap is refused - an
		// encrypted stream with a hole in it is useless to the device.
		have := c.inBase + int64(len(c.in))
		if *in.At+int64(len(in.D)) <= have {
			writeJSON(w, http.StatusOK, map[string]any{"queued": len(c.in), "again": true})
			return
		}
		if *in.At != have {
			writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"error": "input is missing in between"})
			return
		}
	}
	if len(c.in)+len(in.D) > conMaxIn {
		writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "the device is not taking input that fast"})
		return
	}
	c.in = append(c.in, in.D...)
	if in.Brk {
		c.brk++
	}
	c.changed()
	writeJSON(w, http.StatusOK, map[string]any{"queued": len(c.in)})
}

func (sv *Server) conClose(w http.ResponseWriter, r *http.Request) {
	c := sv.conFor(w, r, false)
	if c == nil {
		return
	}
	defer sv.mu.Unlock()
	if c.ended == nil && c.closing == "" {
		c.closing = "closed by the administrator"
		c.changed()
	}
	writeJSON(w, http.StatusOK, map[string]string{"state": "closing"})
}
