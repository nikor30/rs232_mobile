package server

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"testing"

	"rs232c2/internal/device"
	"rs232c2/internal/store"
)

// testPackage builds a package the way firmware/tools/fw_sign.py does. The
// signature is made up: the server does not look at it.
func testPackage(size int, fw string, seq int64) (pkg, image, sig []byte, manifest string) {
	image = make([]byte, size)
	for i := range image {
		image[i] = byte(i*7 + i>>8)
	}
	sum := sha256.Sum256(image)
	manifest = fmt.Sprintf(`{"product":"rs232-console","board":"waveshare-s3-lcd2","fw":%q,"lang":"de","seq":%d,"size":%d,"sha256":%q}`,
		fw, seq, size, hex.EncodeToString(sum[:]))
	sig = bytes.Repeat([]byte{0x5a}, fwSigSize)
	pkg = []byte(fwMagic + manifest + "\n" + base64.StdEncoding.EncodeToString(sig) + "\n")
	pkg = append(pkg, image...)
	return
}

func (e *env) upload(pkg []byte, out any) int {
	e.t.Helper()
	req, _ := http.NewRequest("POST", e.ts.URL+"/admin/v1/firmware", bytes.NewReader(pkg))
	req.Header.Set("Authorization", "Bearer "+adminToken)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if out != nil && resp.StatusCode < 300 {
		if err := json.Unmarshal(b, out); err != nil {
			e.t.Fatalf("upload: %v (%s)", err, b)
		}
	}
	return resp.StatusCode
}

func TestFirmwareShelfAndDelivery(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	pkg, image, sig, manifest := testPackage(3*fwChunk+17, "1.9.0", 1791900000)

	// what is not a package, or does not match its own manifest, stays out
	bad := append([]byte(nil), pkg...)
	bad[len(bad)-1] ^= 1
	for name, b := range map[string][]byte{"garbage": []byte("hello"), "damaged image": bad, "cut short": pkg[:len(pkg)-5]} {
		if code := e.upload(b, nil); code != 400 {
			t.Fatalf("%s: %d", name, code)
		}
	}
	var info FirmwareInfo
	if code := e.upload(pkg, &info); code != 201 || info.FW != "1.9.0" || info.Size != len(image) || !fwIDPattern.MatchString(info.ID) {
		t.Fatalf("upload: %d %+v", code, info)
	}
	if code := e.upload(pkg, nil); code != 201 { // the same package again is the same package
		t.Fatalf("second upload: %d", code)
	}
	var list []FirmwareInfo
	if e.admin("GET", "/admin/v1/firmware", nil, &list); len(list) != 1 || list[0].ID != info.ID {
		t.Fatalf("list: %+v", list)
	}

	cmdURL := "/admin/v1/devices/" + c.ID() + "/commands"
	for _, args := range []any{nil, map[string]string{"fw": "nope"}, map[string]string{"fw": "0123456789abcdef"}} {
		if code := e.admin("POST", cmdURL, map[string]any{"type": "update", "args": args}, nil); code != 400 {
			t.Fatalf("update with %v: %d", args, code)
		}
	}
	// nothing is handed out before the command has been fetched
	if _, _, _, err := c.Firmware(1); err == nil {
		t.Fatal("firmware without a command")
	}
	// a device that says it is other hardware does not get this package
	if _, err := c.Poll(map[string]string{"hw": "another-board"}, nil); err != nil {
		t.Fatal(err)
	}
	if code := e.admin("POST", cmdURL, map[string]any{"type": "update", "args": map[string]string{"fw": info.ID}}, nil); code != 400 {
		t.Fatalf("package for other hardware queued: HTTP %d", code)
	}
	if _, err := c.Poll(map[string]string{"hw": "waveshare-s3-lcd2"}, nil); err != nil {
		t.Fatal(err)
	}
	var cmd store.Command
	if code := e.admin("POST", cmdURL, map[string]any{"type": "update", "args": map[string]string{"fw": info.ID}}, &cmd); code != 201 {
		t.Fatalf("update: %d", code)
	}
	if _, _, _, err := c.Firmware(cmd.ID); err == nil {
		t.Fatal("firmware before the command was fetched")
	}
	if r, err := c.Poll(map[string]string{"hw": "waveshare-s3-lcd2"}, nil); err != nil || len(r.Cmds) != 1 || r.Cmds[0].Type != "update" {
		t.Fatalf("poll: %+v %v", r, err)
	}
	m, s, img, err := c.Firmware(cmd.ID)
	if err != nil || m != manifest || !bytes.Equal(s, sig) || !bytes.Equal(img, image) {
		t.Fatalf("delivery: %v, manifest %v, signature %v, image %v", err, m == manifest, bytes.Equal(s, sig), bytes.Equal(img, image))
	}
	var devs []deviceView
	if e.admin("GET", "/admin/v1/devices", nil, &devs); devs[0].Update == nil || devs[0].Update.Sent != len(image) || devs[0].Update.Size != len(image) {
		t.Fatalf("progress: %+v", devs[0].Update)
	}
	// answered: the package is no longer handed out for this command
	if _, err := c.Poll(nil, []device.Result{{ID: cmd.ID, OK: true, Out: "updated"}}); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := c.Firmware(cmd.ID); err == nil {
		t.Fatal("firmware after the answer")
	}
	devs = nil
	if e.admin("GET", "/admin/v1/devices", nil, &devs); devs[0].Update != nil {
		t.Fatalf("progress after the answer: %+v", devs[0].Update)
	}

	// the shelf survives a restart of the server, and deleting empties it
	if sh := newShelf(filepath.Join(e.dir, "firmware")); sh.get(info.ID) == nil {
		t.Fatal("package not on disk")
	}
	if code := e.admin("DELETE", "/admin/v1/firmware/"+info.ID, nil, nil); code != 200 {
		t.Fatalf("delete: %d", code)
	}
	if code := e.admin("DELETE", "/admin/v1/firmware/"+info.ID, nil, nil); code != 404 {
		t.Fatalf("second delete: %d", code)
	}
	if sh := newShelf(filepath.Join(e.dir, "firmware")); len(sh.list()) != 0 {
		t.Fatal("package still on disk")
	}
}
