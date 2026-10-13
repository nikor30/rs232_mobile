package server

import (
	"encoding/json"
	"net/http"
	"strconv"

	"rs232c2/internal/store"
)

// Groups: which devices belong together, and which accounts may work with
// them (the rules are in the store, groups.go).

// useCommands are the commands an account may send that only uses a device:
// they read, or they open a remote console - which the device still decides on.
var useCommands = []string{"ping", "status", "configs", "console"}

type groupView struct {
	ID      uint64            `json:"id"`
	Name    string            `json:"name"`
	Note    string            `json:"note,omitempty"`
	Members map[string]string `json:"members,omitempty"` // administrators only
	Mine    string            `json:"mine,omitempty"`    // the asking account's right in it
}

// listGroups shows an administrator everything and a member the groups it is
// in, without the other members.
func (sv *Server) listGroups(w http.ResponseWriter, r *http.Request) {
	p := who(r)
	out := []groupView{}
	for _, g := range sv.st.Groups() {
		v := groupView{ID: g.ID, Name: g.Name, Note: g.Note, Mine: g.Members[p.Name]}
		if p.Role == store.RoleAdmin {
			v.Members = g.Members
		} else if v.Mine == "" {
			continue
		}
		out = append(out, v)
	}
	writeJSON(w, http.StatusOK, out)
}

func (sv *Server) putGroup(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Name    string            `json:"name"`
		Note    string            `json:"note"`
		Members map[string]string `json:"members"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	var id uint64
	if v := r.PathValue("id"); v != "" {
		if id, _ = strconv.ParseUint(v, 10, 64); id == 0 {
			fail(w, store.ErrNotFound)
			return
		}
	}
	n := note(r)
	n.target = clean(in.Name)
	n.detail = strconv.Itoa(len(in.Members)) + " members"
	g, err := sv.st.PutGroup(id, clean(in.Name), clean(in.Note), in.Members)
	if err == store.ErrArgs {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "a group needs a name (up to 48 characters); a member has the right use or manage"})
		return
	} else if err == store.ErrExists {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "a group of this name exists already"})
		return
	} else if err != nil {
		fail(w, err)
		return
	}
	code := http.StatusOK
	if id == 0 {
		code = http.StatusCreated
	}
	writeJSON(w, code, groupView{ID: g.ID, Name: g.Name, Note: g.Note, Members: g.Members})
}

func (sv *Server) deleteGroup(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err := sv.st.DeleteGroup(id); err != nil {
		fail(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// groupDevice says which groups a device is in.
func (sv *Server) groupDevice(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Groups []uint64 `json:"groups"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil || len(in.Groups) > 50 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	b, _ := json.Marshal(in.Groups)
	note(r).detail = string(b)
	if err := sv.st.SetGroups(r.PathValue("id"), in.Groups); err == store.ErrArgs {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "unknown group"})
	} else if err != nil {
		fail(w, err)
	} else {
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	}
}
