// devsim plays the part of a device against the server, so the protocol can be
// tried without flashing anything.
//
//	devsim -state dev.json enroll -name Labor-1 'c2e1:...'
//	devsim -state dev.json run -n 3 -interval 2s
package main

import (
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
		fs.Parse(flag.Args()[1:])
		start := time.Now()
		var results []device.Result
		for i := 0; ; i++ {
			status := map[string]any{"uptime_s": int(time.Since(start).Seconds()), "polls": i}
			r, err := c.Poll(status, results)
			if errors.Is(err, device.ErrRefused) {
				log.Fatal("refused by the server (revoked or unknown)")
			}
			wait := 5 * time.Second
			if err != nil {
				log.Printf("poll: %v", err) // results stay queued for the next try
			} else {
				results = nil
				fmt.Printf("STATE %s\n", r.State)
				for _, cmd := range r.Cmds {
					res := device.Result{ID: cmd.ID, OK: true}
					// what the firmware answers, in miniature
					denied := "am Geraet nicht freigegeben (Menue -> Netz -> Leitstelle)"
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
							res.OK, res.Out = false, "Konfiguration nicht gefunden"
						default:
							res.Out = map[string]any{"name": a.Name, "port": a.Port, "line": 3, "lines": 3, "result": "fertig"}
						}
					case "reboot":
						if *control {
							res.Out = "Neustart"
						} else {
							res.OK, res.Out = false, denied
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
