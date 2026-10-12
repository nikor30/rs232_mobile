// devsim plays the part of a device against the server, so the protocol can be
// tried without flashing anything.
//
//	devsim -state dev.json enroll -name Labor-1 'c2e1:...'
//	devsim -state dev.json run -n 3 -interval 2s
package main

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"time"

	"rs232c2/internal/device"
)

func main() {
	log.SetFlags(0)
	statePath := flag.String("state", "device.json", "file with the device's key and the pinned server")
	insecure := flag.Bool("tls-skip-verify", false, "accept any TLS certificate (local self-signed server)")
	flag.Parse()
	if flag.NArg() < 1 {
		log.Fatal("usage: devsim [-state file] [-tls-skip-verify] enroll|run ...")
	}

	var st device.State
	if b, err := os.ReadFile(*statePath); err == nil {
		if err := json.Unmarshal(b, &st); err != nil {
			log.Fatalf("%s: %v", *statePath, err)
		}
	}
	hc := &http.Client{Timeout: 20 * time.Second}
	if *insecure {
		// The channel inside does not depend on this: the server still has to
		// prove the key that the enrollment token pinned.
		hc.Transport = &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}
	}
	c, err := device.New(hc, st)
	if err != nil {
		log.Fatal(err)
	}
	save := func() {
		b, _ := json.MarshalIndent(c.State, "", " ")
		if err := os.WriteFile(*statePath, b, 0o600); err != nil {
			log.Fatal(err)
		}
	}

	switch flag.Arg(0) {
	case "enroll":
		fs := flag.NewFlagSet("enroll", flag.ExitOnError)
		name := fs.String("name", "devsim", "name shown to the administrator")
		fs.Parse(flag.Args()[1:])
		if fs.NArg() != 1 {
			log.Fatal("usage: devsim enroll [-name N] TOKEN")
		}
		sas, err := c.Enroll(fs.Arg(0), *name, map[string]string{"fw": "devsim", "board": "simulator"})
		if err != nil {
			log.Fatalf("enroll: %v", err)
		}
		save()
		fmt.Printf("ID   %s\nCODE %s\n", c.ID(), sas)

	case "run":
		fs := flag.NewFlagSet("run", flag.ExitOnError)
		n := fs.Int("n", 0, "stop after this many polls (0 = never)")
		interval := fs.Duration("interval", 0, "time between polls (0 = what the server asks for)")
		control := fs.Bool("control", false, "allow commands that change something (play, reboot), as the switch at a real device does")
		console := fs.Bool("console", false, "say yes to every request for the remote console, as a tap on a real device's display does")
		fs.Parse(flag.Args()[1:])
		start := time.Now()
		var results []device.Result
		var conID uint64 // remote console agreed to, opened once the server has the answer
		var conMinutes int
		for i := 0; ; i++ {
			status := map[string]any{"uptime_s": int(time.Since(start).Seconds()), "polls": i}
			pinned, next := c.State.ServerPub, c.State.NextPub
			r, err := c.Poll(status, results)
			// a key change of the server: what the device has to remember
			if !bytes.Equal(pinned, c.State.ServerPub) {
				save()
				fmt.Printf("KEY changed to %s\n", c.KeyTag())
			} else if !bytes.Equal(next, c.State.NextPub) {
				save()
				fmt.Printf("KEY %s announced\n", c.KeyTag())
			}
			if errors.Is(err, device.ErrRefused) {
				log.Fatal("refused by the server (revoked or unknown)")
			}
			wait := 5 * time.Second
			if err != nil {
				log.Printf("poll: %v", err) // results stay queued for the next try
			} else {
				results = nil
				fmt.Printf("STATE %s\n", r.State)
				if conID != 0 {
					fmt.Printf("CONSOLE %d open\n", conID)
					fmt.Printf("CONSOLE %d closed: %s\n", conID, runConsole(c, conID, time.Duration(conMinutes)*time.Minute))
					conID = 0
				}
				for _, cmd := range r.Cmds {
					res := device.Result{ID: cmd.ID, OK: true}
					// what the firmware answers, in miniature
					denied := "not allowed at the device (Menu -> Network -> Control center)"
					switch cmd.Type {
					case "ping":
						res.Out = "pong"
					case "status":
						res.Out = status
					case "settings":
						res.Out = map[string]any{"hostname": "rs232", "apSsid": "RS232-SIM", "webPassSet": false}
					case "configs":
						res.Out = map[string]any{"ok": true, "configs": []map[string]any{{"name": "Demo", "size": 38}, {"name": "Uplink", "size": 211}}}
					case "play":
						var a struct {
							Name string `json:"name"`
							Port int    `json:"port"`
						}
						json.Unmarshal(cmd.Args, &a)
						switch {
						case !*control:
							res.OK, res.Out = false, denied
						case a.Name != "Demo" && a.Name != "Uplink":
							res.OK, res.Out = false, "configuration not found"
						default:
							res.Out = map[string]any{"name": a.Name, "port": a.Port, "line": 3, "lines": 3, "result": "done"}
						}
					case "reboot":
						if *control {
							res.Out = "restarting"
						} else {
							res.OK, res.Out = false, denied
						}
					case "console":
						var a struct{ Port, Minutes int }
						json.Unmarshal(cmd.Args, &a)
						if *console {
							conID, conMinutes = cmd.ID, a.Minutes
							res.Out = map[string]any{"port": a.Port, "minutes": a.Minutes}
						} else {
							res.OK, res.Out = false, "refused at the device"
						}
					default:
						res.OK, res.Out = false, "unbekannter Auftrag"
					}
					fmt.Printf("CMD %d %s %s\n", cmd.ID, cmd.Type, cmd.Args)
					results = append(results, res)
				}
				wait = time.Duration(r.PollS) * time.Second
			}
			if *interval > 0 {
				wait = *interval
			}
			// stop after n polls, but hand in outstanding results first (a few tries)
			if *n > 0 && i+1 >= *n && (len(results) == 0 || i+1 >= *n+3) {
				break
			}
			time.Sleep(wait)
		}

	default:
		log.Fatalf("unknown command %q", flag.Arg(0))
	}
}

// runConsole plays a serial port with something like a switch behind it: a
// prompt, an echo, an answer to "show" and to BREAK. It returns why the session
// ended.
func runConsole(c *device.Client, id uint64, limit time.Duration) string {
	const prompt = "\r\nSwitch>"
	out := []byte("\r\nSimulierter Switch an Port 1" + prompt)
	var off, iack int64
	var line []byte
	brk := 0
	deadline := time.Now().Add(limit)
	for failures := 0; ; {
		m := device.ConMsg{ID: id, Off: off, D: out[:min(len(out), 1024)], IAck: iack, LeftS: int(time.Until(deadline).Seconds())}
		if m.LeftS <= 0 {
			m.End = "time is up"
		}
		r, err := c.Console(m)
		if errors.Is(err, device.ErrRefused) {
			return "refused by the server"
		}
		if err != nil {
			if failures++; failures > 10 {
				return "server not reachable"
			}
			time.Sleep(500 * time.Millisecond)
			continue
		}
		failures = 0
		off += int64(len(m.D))
		out = out[len(m.D):]
		if r.End != "" {
			return r.End
		}
		if skip := iack - r.IOff; skip >= 0 && skip < int64(len(r.D)) {
			for _, b := range r.D[skip:] {
				iack++
				switch {
				case b == '\r':
					if string(line) == "show" {
						for i := 1; i <= 40; i++ {
							out = append(out, fmt.Sprintf("\r\nGigabitEthernet0/%d is up, line protocol is up", i)...)
						}
					}
					line = line[:0]
					out = append(out, prompt...)
				case b >= 0x20 && b < 0x7f:
					line = append(line, b)
					out = append(out, b)
				}
			}
		}
		if r.Brk > brk {
			brk = r.Brk
			out = append(out, "\r\n[BREAK]"+prompt...)
		}
	}
}
