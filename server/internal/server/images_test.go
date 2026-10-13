package server

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func (e *env) image(name, folder string, b []byte, out any) int {
	e.t.Helper()
	return e.imageSum(name, folder, "", b, out)
}

func (e *env) imageSum(name, folder, sum string, b []byte, out any) int {
	e.t.Helper()
	req, _ := http.NewRequest("POST", e.ts.URL+"/admin/v1/images?name="+url.QueryEscape(name)+"&folder="+url.QueryEscape(folder)+"&md5="+sum, bytes.NewReader(b))
	req.Header.Set("Authorization", "Bearer "+adminToken)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if out != nil && resp.StatusCode < 300 {
		if err := json.Unmarshal(body, out); err != nil {
			e.t.Fatalf("image: %v (%s)", err, body)
		}
	}
	return resp.StatusCode
}

type imageList struct {
	Images []ImageInfo `json:"images"`
	Used   int64       `json:"used"`
	Total  int64       `json:"total"`
	Max    int64       `json:"max"`
}

func TestImageShelf(t *testing.T) {
	e := newEnv(t) // 1 MB a file, 1.5 MB together
	data := bytes.Repeat([]byte("switch image "), 45000)
	sum := sha256.Sum256(data)

	// with the maker's MD5 sum: a file that does not have it is not kept
	five := md5.Sum(data)
	want := hex.EncodeToString(five[:])
	for sum, code := range map[string]int{"abc": 400, strings.Repeat("0", 32): 400} {
		if got := e.imageSum("x.bin", "", sum, data, nil); got != code {
			t.Fatalf("MD5 %q: %d", sum, got)
		}
	}
	if files, _ := filepath.Glob(filepath.Join(e.dir, "images", "*")); len(files) != 0 {
		t.Fatalf("a refused file was kept: %v", files)
	}
	var info ImageInfo
	if code := e.imageSum("../../etc/c9300-universal.17.09.bin", " Cisco ", strings.ToUpper(want), data, &info); code != 201 || info.MD5 != want {
		t.Fatalf("upload: %d %+v", code, info)
	}
	if info.Name != "c9300-universal.17.09.bin" || info.Folder != "Cisco" || info.Size != int64(len(data)) || info.SHA256 != hex.EncodeToString(sum[:]) || !imgIDPattern.MatchString(info.ID) {
		t.Fatalf("stored: %+v", info)
	}
	for name, code := range map[string]int{"": 400, strings.Repeat("n", 121): 400} {
		if got := e.image(name, "", data, nil); got != code {
			t.Fatalf("name %q: %d", name, got)
		}
	}
	if code := e.image("empty", "", nil, nil); code != 400 {
		t.Fatalf("empty file: %d", code)
	}
	if code := e.image("big", "", make([]byte, 1<<20+1), nil); code != 413 {
		t.Fatalf("too large: %d", code)
	}
	// together they would be more than the library holds
	if code := e.image("second", "", make([]byte, 1<<20), nil); code != 409 {
		t.Fatalf("no room: %d", code)
	}
	var list imageList
	if e.admin("GET", "/admin/v1/images", nil, &list); len(list.Images) != 1 || list.Used != info.Size || list.Max != 1<<20 || list.Total != 3<<19 {
		t.Fatalf("list: %+v", list)
	}
	if files, _ := filepath.Glob(filepath.Join(e.dir, "images", "*")); len(files) != 2 {
		t.Fatalf("left on disk: %v", files)
	}

	// rename and sort
	if code := e.admin("PUT", "/admin/v1/images/"+info.ID, map[string]string{"name": "cat9k.bin", "folder": "Catalyst"}, nil); code != 200 {
		t.Fatalf("edit: %d", code)
	}
	if code := e.admin("PUT", "/admin/v1/images/"+info.ID, map[string]string{"name": " "}, nil); code != 400 {
		t.Fatalf("edit without a name: %d", code)
	}
	if code := e.admin("PUT", "/admin/v1/images/0123456789abcdef", map[string]string{"name": "x"}, nil); code != 404 {
		t.Fatalf("edit of nothing: %d", code)
	}

	// a download needs a ticket, and only a signed-in administrator gets one
	resp, err := http.Post(e.ts.URL+"/admin/v1/images/"+info.ID+"/link", "", nil)
	if err != nil || resp.StatusCode != 401 {
		t.Fatalf("link without sign-in: %v %v", resp.StatusCode, err)
	}
	resp.Body.Close()
	var link struct {
		URL string `json:"url"`
	}
	if code := e.admin("POST", "/admin/v1/images/"+info.ID+"/link", nil, &link); code != 200 || !strings.HasPrefix(link.URL, "/admin/v1/images/file/") {
		t.Fatalf("link: %d %+v", code, link)
	}
	get := func(u, rng string) (int, []byte, http.Header) {
		req, _ := http.NewRequest("GET", e.ts.URL+u, nil)
		if rng != "" {
			req.Header.Set("Range", rng)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, b, resp.Header
	}
	code, body, head := get(link.URL, "")
	if code != 200 || !bytes.Equal(body, data) {
		t.Fatalf("download: %d, %d bytes", code, len(body))
	}
	if d := head.Get("Content-Disposition"); !strings.Contains(d, `filename="cat9k.bin"`) || head.Get("X-Content-Type-Options") != "nosniff" {
		t.Fatalf("headers: %v", head)
	}
	if code, body, _ := get(link.URL, "bytes=100-199"); code != 206 || !bytes.Equal(body, data[100:200]) {
		t.Fatalf("resumed download: %d, %d bytes", code, len(body))
	}
	if code, _, _ := get("/admin/v1/images/file/"+strings.Repeat("0", 48), ""); code != 404 {
		t.Fatalf("made-up ticket: %d", code)
	}
	e.now = e.now.Add(3 * time.Minute)
	if code, _, _ := get(link.URL, ""); code != 404 {
		t.Fatalf("old ticket: %d", code)
	}

	// to a device's SD card: only as a command it has fetched, and byte for byte
	c := e.enrolled()
	cmdURL := "/admin/v1/devices/" + c.ID() + "/commands"
	for _, args := range []any{nil, map[string]string{"img": "nope"}, map[string]string{"img": "0123456789abcdef"}} {
		if code := e.admin("POST", cmdURL, map[string]any{"type": "sd_put", "args": args}, nil); code != 400 {
			t.Fatalf("sd_put with %v: %d", args, code)
		}
	}
	var cmd struct{ ID uint64 }
	if code := e.admin("POST", cmdURL, map[string]any{"type": "sd_put", "args": map[string]string{"img": info.ID}}, &cmd); code != 201 {
		t.Fatalf("sd_put: %d", code)
	}
	if _, _, err := c.Image(cmd.ID); err == nil {
		t.Fatal("image before the command was fetched")
	}
	if r, err := c.Poll(nil, nil); err != nil || len(r.Cmds) != 1 || r.Cmds[0].Type != "sd_put" {
		t.Fatalf("poll: %+v %v", r, err)
	}
	if name, got, err := c.Image(cmd.ID); err != nil || name != "cat9k.bin" || !bytes.Equal(got, data) {
		t.Fatalf("delivery: %v %q %d bytes", err, name, len(got))
	}
	var devs []deviceView
	if e.admin("GET", "/admin/v1/devices", nil, &devs); devs[0].Update == nil || devs[0].Update.Sent != len(data) {
		t.Fatalf("progress: %+v", devs[0].Update)
	}

	// the shelf survives a restart, and deleting empties it
	sh := newImageShelf(filepath.Join(e.dir, "images"), 0, 0)
	if got := sh.get(info.ID); got == nil || got.Name != "cat9k.bin" || got.Folder != "Catalyst" {
		t.Fatalf("after a restart: %+v", got)
	}
	if code := e.admin("DELETE", "/admin/v1/images/"+info.ID, nil, nil); code != 200 {
		t.Fatalf("delete: %d", code)
	}
	if code := e.admin("DELETE", "/admin/v1/images/"+info.ID, nil, nil); code != 404 {
		t.Fatalf("second delete: %d", code)
	}
	if files, _ := filepath.Glob(filepath.Join(e.dir, "images", "*")); len(files) != 0 {
		t.Fatalf("left on disk: %v", files)
	}
	if code := e.image("second", "", make([]byte, 1<<20), nil); code != 201 {
		t.Fatalf("room again: %d", code)
	}
}
