package server

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"rs232c2/internal/store"
)

// The library: what administrators keep at the server to use on many devices -
// configuration templates (in the store) and firmware packages (firmware.go).
// Both are sorted into folders, which are nothing but a name.

func clean(s string) string { return strings.TrimSpace(s) }

// place says where a device belongs: region and site, free text.
func (sv *Server) place(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Region  string `json:"region"`
		Country string `json:"country"`
		Site    string `json:"site"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	in.Region, in.Country, in.Site = clean(in.Region), clean(in.Country), clean(in.Site)
	note(r).detail = strings.Trim(in.Region+" / "+in.Country+" / "+in.Site, " /")
	if err := sv.st.Place(r.PathValue("id"), in.Region, in.Country, in.Site); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (sv *Server) listTemplates(w http.ResponseWriter, _ *http.Request) {
	out := sv.st.Templates()
	sort.Slice(out, func(i, j int) bool {
		if out[i].Folder != out[j].Folder {
			return out[i].Folder < out[j].Folder
		}
		return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name)
	})
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) putTemplate(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Name   string `json:"name"`
		Folder string `json:"folder"`
		Text   string `json:"text"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	var id uint64
	if v := r.PathValue("id"); v != "" {
		id, _ = strconv.ParseUint(v, 10, 64)
		if id == 0 {
			fail(w, store.ErrNotFound)
			return
		}
	}
	n := note(r)
	n.target, n.detail = clean(in.Name), clean(in.Folder)
	t, err := sv.st.PutTemplate(id, clean(in.Name), clean(in.Folder), in.Text, who(r).Name)
	if err == store.ErrArgs {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "a template needs a name (up to 48 characters) and a text of up to 6000 bytes"})
		return
	} else if err != nil {
		fail(w, err)
		return
	}
	code := http.StatusOK
	if id == 0 {
		code = http.StatusCreated
	}
	writeJSON(w, code, t)
}

func (sv *Server) deleteTemplate(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if t, err := sv.st.Template(id); err == nil {
		note(r).target = t.Name
	}
	if err := sv.st.DeleteTemplate(id); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// sendTemplate writes a template to a device as a stored configuration of the
// same name. The device takes it only if changes were allowed there.
func (sv *Server) sendTemplate(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Device string `json:"device"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	id, _ := strconv.ParseUint(r.PathValue("id"), 10, 64)
	t, err := sv.st.Template(id)
	if err != nil {
		fail(w, err)
		return
	}
	n := note(r)
	n.target, n.detail = in.Device, t.Name
	if p := who(r); sv.st.Right(p.Name, p.Role, in.Device) != store.RightManage {
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "your account may not do this"})
		return
	}
	args, _ := json.Marshal(map[string]string{"name": t.Name, "text": t.Text})
	c, err := sv.st.EnqueueBy(in.Device, "config_put", args, who(r).Name)
	if err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, c)
}

// fileFirmware sorts a firmware package into a folder. The folder is kept
// beside the package; the package itself is never touched.
func (sv *Server) fileFirmware(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Folder string `json:"folder"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil || len(in.Folder) > 48 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	note(r).detail = clean(in.Folder)
	if !sv.fw.file(r.PathValue("id"), clean(in.Folder)) {
		fail(w, store.ErrNotFound)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (sh *firmwareShelf) file(id, folder string) bool {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	p := sh.pkgs[id]
	if p == nil {
		return false
	}
	p.info.Folder = folder
	name := filepath.Join(sh.dir, id+".folder")
	if folder == "" {
		os.Remove(name)
	} else {
		os.WriteFile(name, []byte(folder), 0o600)
	}
	return true
}
