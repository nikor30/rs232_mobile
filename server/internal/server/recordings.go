package server

import (
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Recording a remote console to a text file (the switch under the terminal).
// Two copies are made side by side: the device writes what its port sends to
// its SD card (/logs/portN-nnn.log, the same recording as REC at the display),
// and this server keeps one under RecordDir. The server cannot read an end to
// end session, so its copy comes from the browser, which has the plain text:
// whoever switches the recording on decides that the server may keep it. The
// files are not in the database and not in its backup.
const (
	recMax = 32 << 20 // bytes per recording on the server
)

type recordingInfo struct {
	Name    string    `json:"name"`
	Session uint64    `json:"session"`
	Size    int64     `json:"size"`
	Time    time.Time `json:"time"`
}

// recPath is the server's file for a session: <device>-<session>.txt.
func (sv *Server) recPath(deviceID string, sess uint64) string {
	return filepath.Join(sv.cfg.RecordDir, deviceID+"-"+strconv.FormatUint(sess, 10)+".txt")
}

// conRecord switches the recording of an open session on or off.
func (sv *Server) conRecord(w http.ResponseWriter, r *http.Request) {
	var in struct {
		On bool `json:"on"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	if sv.cfg.RecordDir == "" {
		writeJSON(w, http.StatusNotImplemented, map[string]string{"error": "this server keeps no recordings"})
		return
	}
	c := sv.conFor(w, r, true)
	if c == nil {
		return
	}
	defer sv.mu.Unlock()
	if in.On && c.recFile == "" {
		if err := os.MkdirAll(sv.cfg.RecordDir, 0o700); err != nil {
			fail(w, err)
			return
		}
		c.recFile = sv.recPath(c.deviceID, c.id)
		if fi, err := os.Stat(c.recFile); err == nil { // switched on again: the file goes on
			c.recHave = fi.Size()
		}
	}
	if c.rec != in.On {
		c.rec = in.On
		c.changed()
	}
	writeJSON(w, http.StatusOK, c.recView())
}

// conTranscript takes plain text of the session from the browser for the
// server's copy. At is where it goes in the file: a request sent again is
// written once.
func (sv *Server) conTranscript(w http.ResponseWriter, r *http.Request) {
	var in struct {
		D  []byte `json:"d"`
		At int64  `json:"at"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil || in.At < 0 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	note(r).skip = true // the switch is on record, not every piece of text
	c := sv.conFor(w, r, false)
	if c == nil {
		return
	}
	defer sv.mu.Unlock()
	if c.recFile == "" || (!c.rec && c.ended == nil) {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "the recording is off"})
		return
	}
	d := in.D
	if in.At < c.recHave { // partly or wholly here already
		skip := c.recHave - in.At
		if skip >= int64(len(d)) {
			writeJSON(w, http.StatusOK, map[string]any{"have": c.recHave})
			return
		}
		d = d[skip:]
	}
	if c.recHave+int64(len(d)) > recMax {
		writeJSON(w, http.StatusRequestEntityTooLarge, map[string]string{"error": "the recording on the server is full"})
		return
	}
	f, err := os.OpenFile(c.recFile, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o600)
	if err != nil {
		fail(w, err)
		return
	}
	n, err := f.Write(d)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	c.recHave += int64(n)
	if err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"have": c.recHave})
}

// recView is what the browser learns about the recording. Callers hold sv.mu.
func (c *console) recView() map[string]any {
	v := map[string]any{"rec": c.rec, "rec_have": c.recHave}
	if c.devRec != "" {
		v["dev_rec"] = c.devRec
	}
	if c.devRecErr != "" {
		v["dev_rec_err"] = c.devRecErr
	}
	return v
}

// recordings lists the server's recordings of a device, newest first.
func (sv *Server) recordings(deviceID string) []recordingInfo {
	out := []recordingInfo{}
	if sv.cfg.RecordDir == "" {
		return out
	}
	files, _ := filepath.Glob(filepath.Join(sv.cfg.RecordDir, deviceID+"-*.txt"))
	for _, f := range files {
		fi, err := os.Stat(f)
		if err != nil {
			continue
		}
		name := filepath.Base(f)
		sess, err := strconv.ParseUint(strings.TrimSuffix(strings.TrimPrefix(name, deviceID+"-"), ".txt"), 10, 64)
		if err != nil {
			continue
		}
		out = append(out, recordingInfo{Name: name, Session: sess, Size: fi.Size(), Time: fi.ModTime().UTC()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Session > out[j].Session })
	return out
}

func (sv *Server) listRecordings(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"recordings": sv.recordings(r.PathValue("id"))})
}

// recFile finds the file a request names, among the device's own.
func (sv *Server) recFile(r *http.Request) (string, error) {
	name := r.PathValue("name")
	for _, i := range sv.recordings(r.PathValue("id")) {
		if i.Name == name {
			return filepath.Join(sv.cfg.RecordDir, name), nil
		}
	}
	return "", errors.New("no such recording")
}

func (sv *Server) getRecording(w http.ResponseWriter, r *http.Request) {
	p, err := sv.recFile(r)
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": err.Error()})
		return
	}
	note(r).skip = true
	f, err := os.Open(p)
	if err != nil {
		fail(w, err)
		return
	}
	defer f.Close()
	fi, _ := f.Stat()
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="`+filepath.Base(p)+`"`)
	http.ServeContent(w, r, "", fi.ModTime(), f)
}

func (sv *Server) deleteRecording(w http.ResponseWriter, r *http.Request) {
	p, err := sv.recFile(r)
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": err.Error()})
		return
	}
	sv.mu.Lock()
	for _, c := range sv.cons {
		if c.recFile == p && c.ended == nil {
			sv.mu.Unlock()
			writeJSON(w, http.StatusConflict, map[string]string{"error": "the session is still being recorded"})
			return
		}
	}
	sv.mu.Unlock()
	if err := os.Remove(p); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"deleted": filepath.Base(p)})
}

// clip keeps what a device says about its recording printable and short.
func clip(s string, max int) string {
	s = strings.Map(func(r rune) rune {
		if r < ' ' || r == 0x7f {
			return -1
		}
		return r
	}, s)
	if len(s) > max {
		s = s[:max]
	}
	return s
}
