package server

import (
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strconv"
	"time"
)

// Registration without an invitation. Normally a device needs a token: its
// secret decides who may enroll at all, and it names the server's key. A
// device that was only given the server's address has neither - it takes the
// key the server shows it and asks to be let in. What is left to tell the
// right device from anything else is the six-digit code on its display, which
// an administrator types in as always.
//
// So this is closed unless an administrator opens it, and it closes by
// itself: 15 minutes to 3 hours, in steps of 15 minutes.
const (
	openStep = 15 * time.Minute
	openMax  = 3 * time.Hour
)

func (sv *Server) openLeft() time.Duration {
	sv.mu.Lock()
	defer sv.mu.Unlock()
	if left := sv.openUntil.Sub(sv.now()); left > 0 {
		return left
	}
	return 0
}

func (sv *Server) openEnrollment(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Minutes int `json:"minutes"` // 0 closes
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil && err != io.EOF {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	d := time.Duration(in.Minutes) * time.Minute
	if d < 0 || d > openMax || d%openStep != 0 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "open registration runs for 15 minutes to 3 hours, in steps of 15 minutes"})
		return
	}
	sv.mu.Lock()
	sv.openUntil = time.Time{}
	if d > 0 {
		sv.openUntil = sv.now().Add(d)
	}
	sv.mu.Unlock()
	note(r).detail = strconv.Itoa(in.Minutes) + " min"
	log.Printf("registration without invitation: %d min", in.Minutes)
	writeJSON(w, http.StatusOK, map[string]any{"open_s": int(d / time.Second)})
}
