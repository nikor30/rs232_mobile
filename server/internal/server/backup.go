package server

import (
	"encoding/base64"
	"encoding/json"
	"net/http"
	"time"
)

// The device's encrypted backup on its own SD card (firmware src/sdbackup.cpp):
// the device writes it, the key is here. A stolen card is worth nothing
// without this server; a device that broke can be replaced by another one
// that gets the card and the old device's key.

const backupEvery = 24 * time.Hour

// autoBackup queues the daily backup for a device that can take it: a card in
// it, not lent out as a USB stick, and changes allowed (the backup holds its
// passwords, so the device asks for the same permission).
func (sv *Server) autoBackup(id string, status json.RawMessage) {
	var st struct {
		Ctl bool `json:"ctl"`
		SD  struct {
			On    bool `json:"on"`
			Stick bool `json:"stick"`
		} `json:"sd"`
	}
	if json.Unmarshal(status, &st) != nil || !st.Ctl || !st.SD.On || st.SD.Stick || !sv.st.BackupDue(id, backupEvery) {
		return
	}
	if err := sv.queueBackup(id, id, "sd_backup", "daily backup"); err != nil {
		return
	}
}

func (sv *Server) queueBackup(id, keyOf, typ, by string) error {
	key, err := sv.st.BackupKey(keyOf)
	if err != nil {
		return err
	}
	b, _ := json.Marshal(map[string]string{"key": base64.StdEncoding.EncodeToString(key)})
	_, err = sv.st.EnqueueBy(id, typ, b, by)
	return err
}

func (sv *Server) backupNow(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := sv.queueBackup(id, id, "sd_backup", who(r).Name); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// restoreBackup brings a backup from the device's card back: its own, or -
// with "from" - the one of another device whose card it now has.
func (sv *Server) restoreBackup(w http.ResponseWriter, r *http.Request) {
	var in struct {
		From string `json:"from"`
	}
	json.NewDecoder(r.Body).Decode(&in)
	id := r.PathValue("id")
	if in.From == "" {
		in.From = id
	}
	if _, err := sv.st.Device(in.From); err != nil {
		fail(w, err)
		return
	}
	note(r).detail = "key of " + in.From
	if err := sv.queueBackup(id, in.From, "sd_restore", who(r).Name); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
