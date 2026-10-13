package server

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"rs232c2/internal/store"
)

// Firmware for the devices. The server is a shelf and a courier, nothing more:
// an image arrives as a package that was signed on the owner's computer, is
// kept as it came and handed to a device piece by piece. The device checks the
// signature against the key it was built with and the image against the signed
// hash - a server that has been taken over can withhold an update or offer an
// old package, but it cannot make a device run anything the owner did not sign.
//
//	package = "RSFW1\n" manifest "\n" base64(signature) "\n" image
//
// The manifest is the JSON text that was signed, byte for byte.

const (
	fwMagic    = "RSFW1\n"
	fwMaxImage = 6 << 20 // the devices' application partition holds 6.5 MB
	fwMaxPkg   = fwMaxImage + 16<<10
	fwSigSize  = 3309 // ML-DSA-65
	fwChunk    = 4080 // per answer; the device takes 8 kB per message
	fwMaxKept  = 12
)

// FirmwareInfo is what the manifest of a package says. None of it is verified
// here beyond size and hash; the signature is the device's business.
type FirmwareInfo struct {
	ID      string    `json:"id"`
	Product string    `json:"product"`
	Board   string    `json:"board"`
	FW      string    `json:"fw"`
	Lang    string    `json:"lang"`
	Seq     int64     `json:"seq"` // when it was signed (Unix time); devices refuse to go back
	Size    int       `json:"size"`
	SHA256  string    `json:"sha256"`
	Added   time.Time `json:"added"`
	Folder  string    `json:"folder,omitempty"` // where an administrator has sorted it (library.go)
}

type fwPackage struct {
	info     FirmwareInfo
	manifest []byte
	sig      []byte
	image    []byte
}

type fwProgress struct {
	Cmd  uint64    `json:"cmd"`
	Sent int       `json:"sent"`
	Size int       `json:"size"`
	At   time.Time `json:"at"`
}

type firmwareShelf struct {
	dir  string
	mu   sync.Mutex
	pkgs map[string]*fwPackage
	prog map[string]*fwProgress // by device ID: how far the update under way has got
}

var fwIDPattern = regexp.MustCompile(`^[0-9a-f]{16}$`)

func parseFirmware(b []byte) (*fwPackage, error) {
	if !bytes.HasPrefix(b, []byte(fwMagic)) {
		return nil, errors.New("not a firmware package")
	}
	rest := b[len(fwMagic):]
	nl := bytes.IndexByte(rest, '\n')
	if nl < 2 || nl > 1024 {
		return nil, errors.New("not a firmware package")
	}
	manifest := rest[:nl]
	rest = rest[nl+1:]
	nl = bytes.IndexByte(rest, '\n')
	if nl < 0 || nl > 8192 {
		return nil, errors.New("not a firmware package")
	}
	sig, err := base64.StdEncoding.DecodeString(string(rest[:nl]))
	if err != nil || len(sig) != fwSigSize {
		return nil, errors.New("the package carries no usable signature")
	}
	image := rest[nl+1:]
	var info FirmwareInfo
	if err := json.Unmarshal(manifest, &info); err != nil {
		return nil, errors.New("the manifest of the package is not readable")
	}
	if info.Product != "rs232-console" || info.Board == "" || info.FW == "" || info.Seq <= 0 {
		return nil, errors.New("the manifest of the package is incomplete")
	}
	if len(info.Board) > 40 || len(info.FW) > 24 || len(info.Lang) > 8 {
		return nil, errors.New("the manifest of the package is not readable")
	}
	if len(image) == 0 || len(image) > fwMaxImage || info.Size != len(image) {
		return nil, errors.New("the image does not have the size the manifest names")
	}
	sum := sha256.Sum256(image)
	if info.SHA256 != hex.EncodeToString(sum[:]) {
		return nil, errors.New("the image does not match the manifest (damaged file?)")
	}
	id := sha256.Sum256(manifest)
	info.ID = hex.EncodeToString(id[:8])
	return &fwPackage{info: info, manifest: append([]byte(nil), manifest...), sig: sig, image: append([]byte(nil), image...)}, nil
}

// newShelf reads the packages kept in dir. Without a directory there is no
// firmware update.
func newShelf(dir string) *firmwareShelf {
	sh := &firmwareShelf{dir: dir, pkgs: map[string]*fwPackage{}, prog: map[string]*fwProgress{}}
	if dir == "" {
		return sh
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.rsfw"))
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		p, err := parseFirmware(b)
		if err != nil || p.info.ID+".rsfw" != filepath.Base(f) {
			continue
		}
		if st, err := os.Stat(f); err == nil {
			p.info.Added = st.ModTime().UTC()
		}
		if folder, err := os.ReadFile(strings.TrimSuffix(f, ".rsfw") + ".folder"); err == nil && len(folder) <= 48 {
			p.info.Folder = string(folder)
		}
		sh.pkgs[p.info.ID] = p
	}
	return sh
}

func (sh *firmwareShelf) get(id string) *fwPackage {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	return sh.pkgs[id]
}

func (sh *firmwareShelf) list() []FirmwareInfo {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	out := []FirmwareInfo{}
	for _, p := range sh.pkgs {
		out = append(out, p.info)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Seq > out[j].Seq })
	return out
}

func (sh *firmwareShelf) add(b []byte, now time.Time) (*FirmwareInfo, error) {
	if sh.dir == "" {
		return nil, errors.New("this server keeps no firmware (no data directory)")
	}
	p, err := parseFirmware(b)
	if err != nil {
		return nil, err
	}
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if old := sh.pkgs[p.info.ID]; old != nil {
		return &old.info, nil
	}
	if len(sh.pkgs) >= fwMaxKept {
		return nil, errors.New("too many packages are kept: delete one first")
	}
	if err := os.MkdirAll(sh.dir, 0o700); err != nil {
		return nil, errors.New("the package could not be stored")
	}
	name := filepath.Join(sh.dir, p.info.ID+".rsfw")
	if err := os.WriteFile(name+".tmp", b, 0o600); err != nil {
		return nil, errors.New("the package could not be stored")
	}
	if err := os.Rename(name+".tmp", name); err != nil {
		return nil, errors.New("the package could not be stored")
	}
	p.info.Added = now.UTC()
	sh.pkgs[p.info.ID] = p
	return &p.info, nil
}

func (sh *firmwareShelf) remove(id string) bool {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if sh.pkgs[id] == nil {
		return false
	}
	delete(sh.pkgs, id)
	os.Remove(filepath.Join(sh.dir, id+".rsfw"))
	os.Remove(filepath.Join(sh.dir, id+".folder"))
	return true
}

func (sh *firmwareShelf) progress(dev string) *fwProgress {
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if p := sh.prog[dev]; p != nil {
		c := *p
		return &c
	}
	return nil
}

// ---------------------------------------------------------------- admin API

func (sv *Server) listFirmware(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, sv.fw.list())
}

func (sv *Server) addFirmware(w http.ResponseWriter, r *http.Request) {
	b, ok := readBody(w, r, fwMaxPkg)
	if !ok {
		return
	}
	info, err := sv.fw.add(b, sv.now())
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	n := note(r)
	n.target, n.detail = info.ID, info.FW+" "+info.Lang+" "+info.Board
	writeJSON(w, http.StatusCreated, info)
}

func (sv *Server) deleteFirmware(w http.ResponseWriter, r *http.Request) {
	if !sv.fw.remove(r.PathValue("id")) {
		fail(w, store.ErrNotFound)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ---------------------------------------------------------------- device side

type fwReply struct {
	M   string `json:"m,omitempty"`   // the manifest as it was signed
	Sig []byte `json:"sig,omitempty"` // base64 in JSON
	D   []byte `json:"d"`             // a piece of the image, empty at its end
}

// fwDevice answers a device that is carrying out an "update" command: first
// with manifest and signature, then with the image from the offset it names.
func (sv *Server) fwDevice(dev string, cmdID uint64, off int64, head bool) (*fwReply, error) {
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
	if cmd == nil || cmd.Type != "update" || cmd.State != store.CmdSent {
		return nil, errors.New("no update under way")
	}
	var a struct {
		FW string `json:"fw"`
	}
	json.Unmarshal(cmd.Args, &a)
	p := sv.fw.get(a.FW)
	if p == nil {
		return nil, errors.New("the package is gone")
	}
	if head {
		return &fwReply{M: string(p.manifest), Sig: p.sig, D: []byte{}}, nil
	}
	if off < 0 || off > int64(len(p.image)) {
		return nil, errors.New("offset outside the image")
	}
	end := min(int(off)+fwChunk, len(p.image))
	sv.fw.mu.Lock()
	sv.fw.prog[dev] = &fwProgress{Cmd: cmdID, Sent: end, Size: len(p.image), At: sv.now()}
	sv.fw.mu.Unlock()
	return &fwReply{D: p.image[off:end]}, nil
}
