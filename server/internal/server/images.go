package server

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode"

	"rs232c2/internal/store"
)

// Other images: what a technician needs beside the console - the operating
// system of a switch, a boot loader, a licence file. The server only keeps
// them and hands them back to an administrator's browser; nothing here ever
// goes to a device, and nothing is checked beyond the hash that is shown.
// Files are large, so they go from the connection to the disk and back
// without ever being held in memory.
//
//	<dir>/<id>.bin   the file as it came
//	<dir>/<id>.json  what is known about it (ImageInfo)

const (
	imgMaxKept   = 200
	imgTicketFor = 2 * time.Minute // how long a download may take to start
	// What goes to a device's SD card travels in pieces of 4 kB through the
	// device's own channel: slow, and the device counts the offset in 31 bits.
	imgMaxToDevice = 1<<31 - 1
)

// ImageInfo describes one kept file.
type ImageInfo struct {
	ID     string    `json:"id"`
	Name   string    `json:"name"`
	Folder string    `json:"folder,omitempty"`
	Size   int64     `json:"size"`
	SHA256 string    `json:"sha256"`
	Added  time.Time `json:"added"`
	By     string    `json:"by,omitempty"`
}

type imgTicket struct {
	id  string
	end time.Time
}

type imageShelf struct {
	dir   string
	max   int64 // largest single file
	total int64 // all files together
	mu    sync.Mutex
	imgs  map[string]*ImageInfo
	held  int64                // bytes of uploads under way
	tick  map[string]imgTicket // tickets for downloads
}

var imgIDPattern = regexp.MustCompile(`^[0-9a-f]{16}$`)

func newImageShelf(dir string, max, total int64) *imageShelf {
	if max <= 0 {
		max = 2 << 30
	}
	if total <= 0 {
		total = 20 << 30
	}
	sh := &imageShelf{dir: dir, max: max, total: total, imgs: map[string]*ImageInfo{}, tick: map[string]imgTicket{}}
	if dir == "" {
		return sh
	}
	// what an upload left behind when the server stopped under it
	if tmp, _ := filepath.Glob(filepath.Join(dir, "*.tmp")); tmp != nil {
		for _, f := range tmp {
			os.Remove(f)
		}
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.json"))
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		var info ImageInfo
		if json.Unmarshal(b, &info) != nil || info.ID+".json" != filepath.Base(f) || !imgIDPattern.MatchString(info.ID) {
			continue
		}
		st, err := os.Stat(sh.path(info.ID))
		if err != nil || st.Size() != info.Size {
			continue
		}
		sh.imgs[info.ID] = &info
	}
	return sh
}

func (sh *imageShelf) path(id string) string { return filepath.Join(sh.dir, id+".bin") }

func (sh *imageShelf) used() int64 {
	n := sh.held
	for _, i := range sh.imgs {
		n += i.Size
	}
	return n
}

func (sh *imageShelf) list() []ImageInfo {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	out := []ImageInfo{}
	for _, i := range sh.imgs {
		out = append(out, *i)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Folder != out[j].Folder {
			return out[i].Folder < out[j].Folder
		}
		return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name)
	})
	return out
}

func (sh *imageShelf) get(id string) *ImageInfo {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if i := sh.imgs[id]; i != nil {
		c := *i
		return &c
	}
	return nil
}

func (sh *imageShelf) write(info *ImageInfo) error {
	b, _ := json.Marshal(info)
	name := filepath.Join(sh.dir, info.ID+".json")
	if err := os.WriteFile(name+".tmp", b, 0o600); err != nil {
		return err
	}
	return os.Rename(name+".tmp", name)
}

// fileName keeps what is safe to show and to hand to a browser as the name of
// a download: no path, no control characters.
func fileName(s string) string {
	s = strings.ToValidUTF8(s, "")
	if i := strings.LastIndexAny(s, `/\`); i >= 0 {
		s = s[i+1:]
	}
	s = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) || r == '"' {
			return -1
		}
		return r
	}, s)
	return strings.TrimSpace(s)
}

var (
	errImgSize  = errors.New("the file is larger than this server takes")
	errImgSpace = errors.New("the library is full: delete an image first")
)

// add stores what r delivers. size is what the sender announced (-1: nothing).
func (sh *imageShelf) add(r io.Reader, size int64, name, folder, by string, now time.Time) (*ImageInfo, error) {
	if sh.dir == "" {
		return nil, errors.New("this server keeps no images (no data directory)")
	}
	if name == "" || len(name) > 120 || len(folder) > 48 {
		return nil, errors.New("an image needs a name (up to 120 characters); a folder has up to 48")
	}
	if size > sh.max {
		return nil, errImgSize
	}
	// Room is set aside before the first byte: two uploads at once must not
	// both believe the rest of the space is theirs.
	hold := size
	if hold < 0 {
		hold = sh.max
	}
	sh.mu.Lock()
	if len(sh.imgs) >= imgMaxKept || sh.used()+hold > sh.total {
		sh.mu.Unlock()
		return nil, errImgSpace
	}
	sh.held += hold
	sh.mu.Unlock()
	release := func() {
		sh.mu.Lock()
		sh.held -= hold
		sh.mu.Unlock()
	}

	var raw [8]byte
	rand.Read(raw[:])
	id := hex.EncodeToString(raw[:])
	failed := errors.New("the image could not be stored")
	if err := os.MkdirAll(sh.dir, 0o700); err != nil {
		release()
		return nil, failed
	}
	tmp := sh.path(id) + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		release()
		return nil, failed
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(r, hold+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	switch {
	case err != nil:
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			err = errImgSize
		} else if !errors.Is(err, errImgSize) {
			err = errors.New("the upload broke off")
		}
	case n > hold:
		err = errImgSize
	case n == 0:
		err = errors.New("the file is empty")
	case size >= 0 && n != size:
		err = errors.New("the upload broke off")
	}
	if err != nil {
		os.Remove(tmp)
		release()
		return nil, err
	}
	info := &ImageInfo{ID: id, Name: name, Folder: folder, Size: n, SHA256: hex.EncodeToString(h.Sum(nil)), Added: now.UTC(), By: by}
	if err := os.Rename(tmp, sh.path(id)); err == nil {
		err = sh.write(info)
	}
	sh.mu.Lock()
	defer sh.mu.Unlock()
	sh.held -= hold
	if err != nil {
		os.Remove(tmp)
		os.Remove(sh.path(id))
		return nil, failed
	}
	sh.imgs[id] = info
	c := *info
	return &c, nil
}

func (sh *imageShelf) edit(id, name, folder string) error {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	i := sh.imgs[id]
	if i == nil {
		return store.ErrNotFound
	}
	if name == "" || len(name) > 120 || len(folder) > 48 {
		return store.ErrArgs
	}
	c := *i
	c.Name, c.Folder = name, folder
	if err := sh.write(&c); err != nil {
		return errors.New("the image could not be stored")
	}
	*i = c
	return nil
}

func (sh *imageShelf) remove(id string) bool {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if sh.imgs[id] == nil {
		return false
	}
	delete(sh.imgs, id)
	os.Remove(sh.path(id))
	os.Remove(filepath.Join(sh.dir, id+".json"))
	return true
}

// ticket lets a browser fetch one image with a plain link for a short while.
// A link cannot carry the admin token, and a download of this size cannot go
// through the script's memory.
func (sh *imageShelf) ticket(id string, now time.Time) string {
	var raw [24]byte
	rand.Read(raw[:])
	k := hex.EncodeToString(raw[:])
	sh.mu.Lock()
	defer sh.mu.Unlock()
	for old, t := range sh.tick {
		if now.After(t.end) {
			delete(sh.tick, old)
		}
	}
	sh.tick[k] = imgTicket{id: id, end: now.Add(imgTicketFor)}
	return k
}

func (sh *imageShelf) redeem(k string, now time.Time) (string, bool) {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	t, ok := sh.tick[k]
	if !ok || now.After(t.end) {
		return "", false
	}
	return t.id, true
}

// ---------------------------------------------------------------- device side

type imgReply struct {
	N    string `json:"n,omitempty"`    // head: the name to store it under
	Size int64  `json:"size,omitempty"` // head
	SHA  string `json:"sha,omitempty"`  // head: SHA-256, hex
	D    []byte `json:"d"`              // a piece of the file
}

// imgDevice answers a device that is carrying out an "sd_put" command: first
// with name, size and hash, then with the file from the offset it names.
func (sv *Server) imgDevice(dev string, cmdID uint64, off int64, head bool) (*imgReply, error) {
	d, err := sv.st.Device(dev)
	if err != nil {
		return nil, err
	}
	var cmd *store.Command
	for _, c := range d.Commands {
		if c.ID == cmdID {
			cmd = c
		}
	}
	if cmd == nil || cmd.Type != "sd_put" || cmd.State != store.CmdSent {
		return nil, errors.New("no transfer under way")
	}
	var a struct {
		Img string `json:"img"`
	}
	json.Unmarshal(cmd.Args, &a)
	i := sv.img.get(a.Img)
	if i == nil {
		return nil, errors.New("the image is gone")
	}
	if head {
		return &imgReply{N: i.Name, Size: i.Size, SHA: i.SHA256, D: []byte{}}, nil
	}
	if off < 0 || off > i.Size {
		return nil, errors.New("offset outside the image")
	}
	f, err := os.Open(sv.img.path(i.ID))
	if err != nil {
		return nil, errors.New("the image is gone")
	}
	defer f.Close()
	buf := make([]byte, min(int64(fwChunk), i.Size-off))
	if _, err := f.ReadAt(buf, off); err != nil && err != io.EOF {
		return nil, errors.New("the image cannot be read")
	}
	sv.fw.mu.Lock()
	sv.fw.prog[dev] = &fwProgress{Cmd: cmdID, Sent: int(off) + len(buf), Size: int(i.Size), At: sv.now()}
	sv.fw.mu.Unlock()
	return &imgReply{D: buf}, nil
}

// ---------------------------------------------------------------- admin API

// long lifts the server's time limits for one request: an image takes longer
// than the 30 s everything else gets.
func long(w http.ResponseWriter) {
	rc := http.NewResponseController(w)
	rc.SetReadDeadline(time.Time{})
	rc.SetWriteDeadline(time.Time{})
}

func (sv *Server) listImages(w http.ResponseWriter, _ *http.Request) {
	sv.img.mu.Lock()
	used := sv.img.used()
	sv.img.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{"images": sv.img.list(), "used": used, "total": sv.img.total, "max": sv.img.max})
}

// addImage takes the file as the body of the request; name and folder come in
// the query, because the body is nothing but the file.
func (sv *Server) addImage(w http.ResponseWriter, r *http.Request) {
	long(w)
	q := r.URL.Query()
	name, folder := fileName(q.Get("name")), clean(strings.ToValidUTF8(q.Get("folder"), ""))
	n := note(r)
	n.target, n.detail = name, folder
	info, err := sv.img.add(r.Body, r.ContentLength, name, folder, who(r).Name, sv.now())
	if err != nil {
		code := http.StatusBadRequest
		if err == errImgSize {
			code = http.StatusRequestEntityTooLarge
		} else if err == errImgSpace {
			code = http.StatusConflict
		}
		writeJSON(w, code, map[string]string{"error": err.Error()})
		return
	}
	n.detail = strings.TrimSpace(folder + " " + info.SHA256[:16])
	writeJSON(w, http.StatusCreated, info)
}

func (sv *Server) editImage(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Name   string `json:"name"`
		Folder string `json:"folder"`
	}
	if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad JSON"})
		return
	}
	in.Name, in.Folder = fileName(in.Name), clean(strings.ToValidUTF8(in.Folder, ""))
	n := note(r)
	n.target, n.detail = in.Name, in.Folder
	if err := sv.img.edit(r.PathValue("id"), in.Name, in.Folder); err == store.ErrArgs {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "an image needs a name (up to 120 characters); a folder has up to 48"})
	} else if err != nil {
		fail(w, err)
	} else {
		writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
	}
}

func (sv *Server) deleteImage(w http.ResponseWriter, r *http.Request) {
	if i := sv.img.get(r.PathValue("id")); i != nil {
		note(r).target = i.Name
	}
	if !sv.img.remove(r.PathValue("id")) {
		fail(w, store.ErrNotFound)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// linkImage answers with an address the image can be fetched from for the
// next two minutes. Asking for it is what the audit log records.
func (sv *Server) linkImage(w http.ResponseWriter, r *http.Request) {
	i := sv.img.get(r.PathValue("id"))
	if i == nil {
		fail(w, store.ErrNotFound)
		return
	}
	n := note(r)
	n.target, n.detail = i.Name, "download"
	writeJSON(w, http.StatusOK, map[string]string{"url": "/admin/v1/images/file/" + sv.img.ticket(i.ID, sv.now())})
}

// sendImage hands out the file behind a ticket. No sign-in is asked for here:
// the ticket is the permission, and only a signed-in administrator gets one.
func (sv *Server) sendImage(w http.ResponseWriter, r *http.Request) {
	id, ok := sv.img.redeem(r.PathValue("ticket"), sv.now())
	var i *ImageInfo
	if ok {
		i = sv.img.get(id)
	}
	if i == nil {
		if sv.limited(sv.limFail, w, r) {
			return
		}
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	f, err := os.Open(sv.img.path(i.ID))
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	defer f.Close()
	long(w)
	ascii := strings.Map(func(r rune) rune {
		if r < 0x20 || r > 0x7e || r == '\\' {
			return '_'
		}
		return r
	}, i.Name)
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Disposition", `attachment; filename="`+ascii+`"; filename*=UTF-8''`+url.PathEscape(i.Name))
	http.ServeContent(w, r, "", i.Added, f) // answers ranges too: a broken download can be resumed
}
