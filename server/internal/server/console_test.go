package server

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"rs232c2/internal/device"
	"rs232c2/internal/store"
)

type conOut struct {
	State  string
	Reason string
	From   int64
	Next   int64
	D      []byte
	Lost   int64
}

// granted asks for a console and lets the device agree. It returns the session ID.
func (e *env) granted(c *device.Client) uint64 {
	e.t.Helper()
	var cmd store.Command
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands",
		map[string]any{"type": "console", "args": map[string]int{"port": 2, "minutes": 15}}, &cmd); code != 201 {
		e.t.Fatalf("request: HTTP %d", code)
	}
	r, err := c.Poll(nil, nil)
	if err != nil || len(r.Cmds) != 1 || r.Cmds[0].Type != "console" {
		e.t.Fatalf("poll: %+v %v", r, err)
	}
	if _, err := c.Poll(nil, []device.Result{{ID: cmd.ID, OK: true, Out: map[string]int{"port": 2, "minutes": 15}}}); err != nil {
		e.t.Fatal(err)
	}
	return cmd.ID
}

func (e *env) conState(id string) (v struct {
	Console  *conView
	Sessions []store.ConsoleLog
}) {
	e.t.Helper()
	if code := e.admin("GET", "/admin/v1/devices/"+id+"/console", nil, &v); code != 200 {
		e.t.Fatalf("console status: HTTP %d", code)
	}
	return v
}

func TestConsoleRoundTrip(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	base := "/admin/v1/devices/" + c.ID() + "/console"
	if v := e.conState(c.ID()); v.Console != nil {
		t.Fatalf("console before any request: %+v", v.Console)
	}
	id := e.granted(c)
	if v := e.conState(c.ID()); v.Console == nil || v.Console.State != "granted" {
		t.Fatalf("after the yes: %+v", v.Console)
	}
	sess := fmt.Sprintf("%s/%d", base, id)
	if code := e.admin("GET", sess+"/output?wait=0", nil, nil); code != 404 {
		t.Fatalf("output before the device opened: HTTP %d", code)
	}

	// the device opens it with its first output
	r, err := c.Console(device.ConMsg{ID: id, D: []byte("Switch>"), LeftS: 900})
	if err != nil || r.End != "" || len(r.D) != 0 {
		t.Fatalf("open: %+v %v", r, err)
	}
	if v := e.conState(c.ID()); v.Console.State != "open" || v.Console.Port != 2 || v.Console.LeftS != 900 || len(v.Sessions) != 1 {
		t.Fatalf("open: %+v %+v", v.Console, v.Sessions)
	}
	// while it is open the device asks more often: a script sent besides does not wait half a minute
	if p, err := c.Poll(nil, nil); err != nil || p.PollS != conPollS {
		t.Fatalf("poll during the console: %+v %v", p, err)
	}
	var out conOut
	e.admin("GET", sess+"/output?from=0&wait=0", nil, &out)
	if string(out.D) != "Switch>" || out.Next != 7 || out.State != "open" {
		t.Fatalf("output: %+v", out)
	}

	// the same message again (its answer was lost): nothing doubles
	if _, err := c.Console(device.ConMsg{ID: id, D: []byte("Switch>")}); err != nil {
		t.Fatal(err)
	}
	// overlapping: only the new part counts
	c.Console(device.ConMsg{ID: id, Off: 4, D: []byte("ch>en")})
	e.admin("GET", sess+"/output?from=0&wait=0", nil, &out)
	if string(out.D) != "Switch>en" {
		t.Fatalf("after repeats: %q", out.D)
	}

	// input reaches the device, and is repeated until the device confirms it
	if code := e.admin("POST", sess+"/input", map[string]any{"d": []byte("show\r")}, nil); code != 200 {
		t.Fatalf("input: HTTP %d", code)
	}
	r, _ = c.Console(device.ConMsg{ID: id, Off: 9})
	if string(r.D) != "show\r" || r.IOff != 0 {
		t.Fatalf("input at the device: %+v", r)
	}
	r, _ = c.Console(device.ConMsg{ID: id, Off: 9, IAck: 2})
	if string(r.D) != "ow\r" || r.IOff != 2 {
		t.Fatalf("partly confirmed: %+v", r)
	}
	e.admin("POST", sess+"/input", map[string]any{"brk": true}, nil)
	r, _ = c.Console(device.ConMsg{ID: id, Off: 9, IAck: 5})
	if len(r.D) != 0 || r.IOff != 5 || r.Brk != 1 {
		t.Fatalf("confirmed, with a BREAK: %+v", r)
	}
	// input with its place in the stream: sent again, it is taken once; a gap is refused
	for i := 0; i < 2; i++ {
		if code := e.admin("POST", sess+"/input", map[string]any{"d": []byte("ab"), "at": 5}, nil); code != 200 {
			t.Fatalf("input at 5 (%d): HTTP %d", i, code)
		}
	}
	if code := e.admin("POST", sess+"/input", map[string]any{"d": []byte("x"), "at": 9}, nil); code != 422 {
		t.Fatalf("input with a gap: HTTP %d", code)
	}
	r, _ = c.Console(device.ConMsg{ID: id, Off: 9, IAck: 5})
	if string(r.D) != "ab" || r.IOff != 5 {
		t.Fatalf("input sent twice: %+v", r)
	}
	c.Console(device.ConMsg{ID: id, Off: 9, IAck: 7})
	if code := e.admin("POST", sess+"/input", map[string]any{"d": []byte("ab"), "at": 5}, nil); code != 200 {
		t.Fatalf("input confirmed already: HTTP %d", code)
	}

	// a second console for the same device has to wait
	if code := e.admin("POST", "/admin/v1/devices/"+c.ID()+"/commands",
		map[string]any{"type": "console", "args": map[string]int{"port": 1, "minutes": 5}}, nil); code != 409 {
		t.Fatalf("second console: HTTP %d", code)
	}

	// the administrator closes; the device learns it with its next message
	if code := e.admin("POST", sess+"/close", nil, nil); code != 200 {
		t.Fatalf("close: HTTP %d", code)
	}
	r, _ = c.Console(device.ConMsg{ID: id, Off: 9, D: []byte("bye"), IAck: 7})
	if r.End == "" {
		t.Fatalf("device not told: %+v", r)
	}
	v := e.conState(c.ID())
	if v.Console.State != "ended" || len(v.Sessions) != 1 || v.Sessions[0].End == nil || v.Sessions[0].Out != 12 || v.Sessions[0].In != 7 || v.Sessions[0].Port != 2 {
		t.Fatalf("log: %+v %+v", v.Console, v.Sessions)
	}
	if code := e.admin("POST", sess+"/input", map[string]any{"d": []byte("x")}, nil); code != 409 {
		t.Fatalf("input after the end: HTTP %d", code)
	}
	// the tail stays readable, with the reason
	e.admin("GET", sess+"/output?from=9", nil, &out)
	if string(out.D) != "bye" || out.State != "ended" || out.Reason == "" {
		t.Fatalf("tail: %+v", out)
	}
	// and the session cannot be opened a second time
	if r, _ := c.Console(device.ConMsg{ID: id}); r.End == "" {
		t.Fatal("ended session answered as if open")
	}
	e.now = e.now.Add(conKeep + time.Minute)
	if r, _ := c.Console(device.ConMsg{ID: id}); r.End == "" {
		t.Fatal("old session reopened")
	}
}

func TestConsoleNeedsTheDevicesYes(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	cmds := "/admin/v1/devices/" + c.ID() + "/commands"
	open := func(id uint64) string {
		t.Helper()
		r, err := c.Console(device.ConMsg{ID: id, D: []byte("x")})
		if err != nil {
			t.Fatal(err)
		}
		return r.End
	}
	// no request at all, and a command of another kind
	if open(999) == "" {
		t.Fatal("console without a request")
	}
	var ping store.Command
	e.admin("POST", cmds, map[string]string{"type": "ping"}, &ping)
	c.Poll(nil, nil)
	c.Poll(nil, []device.Result{{ID: ping.ID, OK: true, Out: "pong"}})
	if open(ping.ID) == "" {
		t.Fatal("a ping opened a console")
	}
	// requested, not yet answered - and then refused at the device
	var cmd store.Command
	e.admin("POST", cmds, map[string]any{"type": "console", "args": map[string]int{"port": 1, "minutes": 5}}, &cmd)
	c.Poll(nil, nil)
	if e.conState(c.ID()).Console.State != "requested" || open(cmd.ID) == "" {
		t.Fatal("console before the answer")
	}
	c.Poll(nil, []device.Result{{ID: cmd.ID, OK: false, Out: "refused at the device"}})
	if v := e.conState(c.ID()).Console; v.State != "refused" || v.Reason != "refused at the device" || open(cmd.ID) == "" {
		t.Fatalf("after the no: %+v", v)
	}
	// agreed, but too long ago
	id := e.granted(c)
	e.now = e.now.Add(conGrantAge + time.Second)
	if open(id) == "" {
		t.Fatal("stale yes accepted")
	}
	if v := e.conState(c.ID()).Console; v.State != "ended" {
		t.Fatalf("stale yes: %+v", v)
	}
	// a request nobody fetches is gone after two minutes, not a day
	e.admin("POST", cmds, map[string]any{"type": "console", "args": map[string]int{"port": 1, "minutes": 5}}, &cmd)
	e.now = e.now.Add(store.ConsoleQueueTTL + time.Second)
	if r, _ := c.Poll(nil, nil); len(r.Cmds) != 0 {
		t.Fatal("stale console request delivered")
	}
	if v := e.conState(c.ID()).Console; v.State != "refused" {
		t.Fatalf("stale request: %+v", v)
	}
	// bad arguments never reach a device
	for _, args := range []map[string]int{{"port": 0, "minutes": 5}, {"port": 1, "minutes": 61}, {"port": 1}} {
		if code := e.admin("POST", cmds, map[string]any{"type": "console", "args": args}, nil); code != 400 {
			t.Fatalf("%v: HTTP %d", args, code)
		}
	}
}

func TestConsoleEndsFromTheDeviceAndOnSilence(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	id := e.granted(c)
	c.Console(device.ConMsg{ID: id, D: []byte("a")})
	if r, err := c.Console(device.ConMsg{ID: id, Off: 1, D: []byte("b"), End: "closed at the device"}); err != nil || r.End == "" {
		t.Fatalf("%+v %v", r, err)
	}
	if v := e.conState(c.ID()); v.Console.State != "ended" || v.Console.Reason != "closed at the device" || v.Sessions[0].Out != 2 {
		t.Fatalf("%+v %+v", v.Console, v.Sessions)
	}

	id = e.granted(c)
	c.Console(device.ConMsg{ID: id})
	e.now = e.now.Add(conSilence + time.Second)
	if v := e.conState(c.ID()); v.Console.State != "ended" || len(v.Sessions) != 2 || v.Sessions[1].End == nil {
		t.Fatalf("silent device: %+v %+v", v.Console, v.Sessions)
	}

	// revoking the device closes its console
	c2 := e.enrolled()
	id = e.granted(c2)
	c2.Console(device.ConMsg{ID: id})
	e.admin("POST", "/admin/v1/devices/"+c2.ID()+"/revoke", nil, nil)
	if v := e.conState(c2.ID()); v.Console.State != "ended" {
		t.Fatalf("revoked: %+v", v.Console)
	}
	if _, err := c2.Console(device.ConMsg{ID: id}); err == nil {
		t.Fatal("revoked device still served")
	}
}

// What is typed while the device's question is being held goes out at once,
// and the browser's long poll returns as soon as the port says something.
func TestConsoleWaitsOnBothSides(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	id := e.granted(c)
	c.Console(device.ConMsg{ID: id})
	sess := fmt.Sprintf("/admin/v1/devices/%s/console/%d", c.ID(), id)

	start := time.Now()
	if r, _ := c.Console(device.ConMsg{ID: id}); len(r.D) != 0 || time.Since(start) < conHold/2 {
		t.Fatalf("an empty question was not held: %v", time.Since(start))
	}
	go func() {
		time.Sleep(30 * time.Millisecond)
		e.admin("POST", sess+"/input", map[string]any{"d": []byte("x")}, nil)
	}()
	start = time.Now()
	if r, _ := c.Console(device.ConMsg{ID: id}); string(r.D) != "x" || time.Since(start) > conHold-20*time.Millisecond {
		t.Fatalf("input did not cut the wait short: %q after %v", r.D, time.Since(start))
	}

	go func() {
		time.Sleep(50 * time.Millisecond)
		c.Console(device.ConMsg{ID: id, D: []byte("hello"), IAck: 1})
	}()
	var out conOut
	start = time.Now()
	e.admin("GET", sess+"/output?from=0&wait=5", nil, &out)
	if string(out.D) != "hello" || time.Since(start) > 2*time.Second {
		t.Fatalf("long poll: %q after %v", out.D, time.Since(start))
	}

	// more than the buffer holds: the oldest goes, and the browser is told
	big := bytes.Repeat([]byte("0123456789abcdef"), conMaxChunk/16)
	off := int64(5)
	for i := 0; i < conMaxOut/len(big)+2; i++ {
		if _, err := c.Console(device.ConMsg{ID: id, Off: off, D: big, IAck: 1}); err != nil {
			t.Fatal(err)
		}
		off += int64(len(big))
	}
	e.admin("GET", sess+"/output?from=0&wait=0", nil, &out)
	if out.Lost == 0 || out.Next != off || len(out.D) != conMaxOut || out.From != off-conMaxOut {
		t.Fatalf("overflow: lost %d from %d next %d len %d", out.Lost, out.From, out.Next, len(out.D))
	}
}

func TestConsoleRecording(t *testing.T) {
	e := newEnv(t)
	c := e.enrolled()
	id := e.granted(c)
	base := "/admin/v1/devices/" + c.ID()
	sess := fmt.Sprintf("%s/console/%d", base, id)
	if r, _ := c.Console(device.ConMsg{ID: id, D: []byte("Switch>"), LeftS: 900}); r.Rec {
		t.Fatal("recording before anybody switched it on")
	}
	if code := e.admin("POST", sess+"/transcript", map[string]any{"d": []byte("x"), "at": 0}, nil); code != 409 {
		t.Fatalf("text while the recording is off: HTTP %d", code)
	}
	var v map[string]any
	if code := e.admin("POST", sess+"/record", map[string]bool{"on": true}, &v); code != 200 || v["rec"] != true {
		t.Fatalf("switch on: HTTP %d %v", code, v)
	}
	// the device is told, and says where it records
	r, _ := c.Console(device.ConMsg{ID: id, Off: 7})
	if !r.Rec {
		t.Fatal("device not told to record")
	}
	c.Console(device.ConMsg{ID: id, Off: 7, Rec: "/logs/port2-001.log"})
	if st := e.conState(c.ID()).Console; !st.Rec || st.DevRec != "/logs/port2-001.log" {
		t.Fatalf("status: %+v", st)
	}
	// the browser's text: a request sent again is written once
	for _, p := range []struct {
		d  string
		at int
	}{{"Switch>", 0}, {"Switch>", 0}, {"h>show", 5}, {"\r\n", 11}} {
		if code := e.admin("POST", sess+"/transcript", map[string]any{"d": []byte(p.d), "at": p.at}, nil); code != 200 {
			t.Fatalf("text %q: HTTP %d", p.d, code)
		}
	}
	var list struct{ Recordings []recordingInfo }
	e.admin("GET", base+"/recordings", nil, &list)
	if len(list.Recordings) != 1 || list.Recordings[0].Session != id || list.Recordings[0].Size != 13 {
		t.Fatalf("list: %+v", list)
	}
	name := list.Recordings[0].Name
	if b, _ := os.ReadFile(filepath.Join(e.dir, "recordings", name)); string(b) != "Switch>show\r\n" {
		t.Fatalf("file: %q", b)
	}
	if code := e.admin("POST", base+"/recordings/"+name+"/delete", nil, nil); code != 409 {
		t.Fatalf("delete while recording: HTTP %d", code)
	}
	// off: the device is told, the browser's text is refused
	e.admin("POST", sess+"/record", map[string]bool{"on": false}, nil)
	if r, _ := c.Console(device.ConMsg{ID: id, Off: 7}); r.Rec {
		t.Fatal("device still told to record")
	}
	if code := e.admin("POST", sess+"/transcript", map[string]any{"d": []byte("more"), "at": 13}, nil); code != 409 {
		t.Fatalf("text after off: HTTP %d", code)
	}
	if code := e.admin("GET", base+"/recordings/"+name, nil, nil); code != 200 {
		t.Fatalf("download: HTTP %d", code)
	}
	if code := e.admin("GET", base+"/recordings/..%2fstate.json", nil, nil); code != 404 {
		t.Fatalf("a file outside: HTTP %d", code)
	}
	c.Console(device.ConMsg{ID: id, Off: 7, End: "closed at the device"})
	if code := e.admin("POST", base+"/recordings/"+name+"/delete", nil, nil); code != 200 {
		t.Fatalf("delete: HTTP %d", code)
	}
}
