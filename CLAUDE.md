# RS232-WLAN-Konsole (rs232_mobile)

Projektgedächtnis für Claude-Sitzungen. Die ausführliche Fassung ist `HANDOVER.md` — zuerst lesen.

## Was das ist

Mobiler serieller Konsolenserver auf dem Waveshare ESP32-S3-Touch-LCD-2 (2″-Touch-LCD): WLAN-Hotspot, `http://192.168.4.1`, xterm.js-Terminal auf Switch/Router/Firewall. Bis zu vier Ports, Raw-TCP/Telnet, XMODEM/YMODEM, 802.1X, HTTPS. Firmware-Stand **v1.8.0** (7. Oktober 2026).

## Aufbau

- `firmware/` — PlatformIO-Projekt (`src/`, `web/`, `tools/`)
- `tests/` — Host-Unittests (g++, laufen auf dem PC), Anleitung in `tests/README.md`; `tests/c2/run.sh` prüft den Protokollcode der Firmware gegen den Server
- `server/` — Leitstelle (Command-and-Control-Server) in Go, läuft im Docker-Container. Konzept, Protokoll und Phasen: `server/KONZEPT.md`; Start und Admin-API: `server/README.md`. Test: `server/test/e2e.sh` (Go ist nicht installiert, gebaut wird im Container; erster Bau auf dem Pi rund 10 Minuten)
- Doku: `firmware/WAVESHARE.md` (das Board, meistgebraucht), `firmware/README.md` (Weboberfläche, Netz, 802.1X/HTTPS, Diagnose)

## Bauen

```bash
cd firmware
pio run -e waveshare-s3-lcd2 -t upload   # einzige Umgebung, Arduino-Core 2.0.17
```

Auf dem Raspberry Pi liegt PlatformIO in `/root/.local/share/pio-venv/bin/pio`; das Waveshare-Board hängt dort an `/dev/ttyACM0` (nach einem Aus-/Einschalten auch `ttyACM1`; `lcd_debug.py` sucht den Port selbst).

`src/web_assets.h` wird vor jedem Build von `tools/embed_web.py` aus `web/` erzeugt — nicht von Hand ändern. Pins und Defaults stehen in `src/config.h`.

Seit 9. Oktober 2026 gibt es nur noch das Waveshare-Board: DevKit-Mockup (OLED), LilyGO T-RSS3 und VIEWE-5″-Panel samt OLED-Treiber, LVGL-GUI, Status-LED und fertigen Images sind entfernt (letzter Stand mit allen Boards: Commit `3bbc16c`).

## Verifikationsstand

- Web-Terminal, Raw-TCP und Konsolenzugriff auf einen echten Cisco-Switch wurden am früheren DevKit-Mockup erprobt, nicht am Waveshare-Board.
- Waveshare ESP32-S3-Touch-LCD-2 (seit 1. Oktober 2026): Boot, Hotspot, LCD, Touch, Lagesensor und Bluetooth-Kopplung am Gerät geprüft; Oberfläche per Screenshot und simulierten Taps getestet; Akkumessung an IO5 bestätigt, Ladeerkennung nur am USB-Port eines Rechners gesehen (kein Statuspin, aus USB-Frames und Spannung geschlossen); SD-Karte fällt nach jedem Reset aus (offen); serieller Port ohne MAX3232. Details und offene Punkte: `firmware/WAVESHARE.md`.
- Das Waveshare-Display lässt sich ohne Hinsehen prüfen: `firmware/tools/lcd_debug.py shot bild.png`, `status`, `tap X Y` (Screenshot ansehen statt raten).
- Waveshare seit 6. Oktober 2026: Port 1 sendet auf IO21 statt IO43 (ROM-Startmeldungen); Seiten scrollen, Tasten größer; Sparmodus im Akkubetrieb (80 MHz, Abdunkeln) und Ausschalten (Tiefschlaf, BOOT-Taste weckt). Per Debug-Konsole am USB-Kabel geprüft — echter Akkubetrieb, BOOT-Wecken und Stromaufnahme nicht. `lcd_debug.py poweroff` **ohne Zeitangabe** lässt sich nur am Gerät rückgängig machen.
- Akku-Kalibrierung (7. Oktober 2026, 0-%-/100-%-Punkt, `src/bat_curve.h`): Host-Test, Debug-Konsole, Touch-Seite Setup und `/api/batcal` (curl) am Waveshare geprüft; Web-Block im Browser ungeprüft.
- 802.1X gegen einen echten RADIUS steht aus. Die Firmware baut nur noch auf Arduino-Core 2.0.17 / mbedTLS 2: Core-3-Kompatibilität (`compat_eap.h`, `compat_mbedtls.h`) und die LBO-Warnleitung sind seit 9. Oktober 2026 entfernt, ebenso `hardware/` (KiCad-Tochterplatine, Gehäuse fürs LCDwiki-Board) und `diagramme/` — letzter Stand damit: Commit `43e7939`.

## Regeln aus Erfahrung (Details: HANDOVER.md §8)

- RX/TX-Häkchen tauscht nur ESP32-GPIOs am MAX3232, es ersetzt kein Nullmodem. Kein automatischer Tausch im Auto-Baud.
- Diagnoseausgaben müssen den tatsächlichen Zustand nennen (Boot-Log, `[TCP]`-Zeilen).
- Rückgabewert von `WiFiClient::write()` nie ignorieren; TCP läuft über Sendewarteschlange mit `MSG_DONTWAIT`.
- Telnet/Raw wird am ersten Byte (`0xFF`) unterschieden; Raw bleibt byte-genau.
- Pinbelegungen aus KiCad-Symbolbibliothek, nicht aus Datenblatt-PDFs.
- Byte-Logik erst auf dem Host testen, dann auf den Mikrocontroller.

## Leitstelle (seit 9. Oktober 2026)

Geräte melden sich ausgehend per HTTPS bei einem Server; darin ein eigener hybrider Kanal (X25519 + ML-KEM-768, nur KEMs, keine Signaturen), Registrierung mit Einladungs-Token und sechsstelligem Code vom Gerätedisplay. Phasen 1 bis 3 sind fertig: Server mit Admin-Weboberfläche (`server/internal/server/web/`), Gerätesimulator, Client in der Firmware (`src/c2.cpp`, `src/c2_proto.cpp`, ML-KEM in `lib/mlkem_native/`), Aufträge `ping`/`status`/`settings`/`configs` (lesen) und `play`/`reboot` (nur mit dem Haken in der Weboberfläche des Geräts, `settings.c2Control`). Am Waveshare-Board gegen den Server im Container auf dem Pi geprüft, beide Weboberflächen in Chromium über Playwright (`server/test/ui.sh`), nicht im Internet und nicht mit einem echten Handy. Firmware-Update über die Leitstelle gibt es bewusst nicht (erst mit Signaturprüfung). Nicht ins Internet stellen, bevor Phase 5 steht.

Phase 4, die Fernkonsole (seit 11. Oktober 2026, `server/KONZEPT.md` §4.5): Der Administrator bedient einen seriellen Port aus dem Browser (xterm.js in der Admin-Weboberfläche); jede Sitzung braucht einen Tipp am Touch-Display, läuft höchstens 60 Minuten und ist von beiden Seiten zu beenden. Server: `internal/server/console.go`; Firmware: `src/c2_console.cpp` und der Konsolenteil von `src/c2.cpp`. Nicht Ende-zu-Ende — der Server leitet die Zeichen im Klartext durch. Geprüft am Server (Go-Unittests, `server/test/ui.sh` gegen den Simulator) und am 11. Oktober 2026 am Waveshare-Board über Admin-API und `lcd_debug.py`: Anfragen, Ablehnen, Erlauben, Eingabe bis zum Port, Beenden von allen Seiten, Zeitablauf. **Nicht geprüft:** die Richtung Port → Server am Board (nichts am Port angeschlossen), BREAK auf der Leitung, die Admin-Weboberfläche im Browser gegen das echte Gerät. Der Strom-Code (`c2_console.cpp`) läuft auch in `tests/c2/run.sh` auf dem PC gegen den Server. `/api/c2/enroll` per curl braucht `-H 'Content-Type: application/json'`.

Regeln daraus: Aufträge führt die Hauptschleife aus (`C2::loop()`), nicht der Netz-Task; neue Auftragsarten an zwei Stellen eintragen (`commandArgs` am Server, `execute()` am Gerät) und alles Verändernde an `c2Control` binden. TLS (45 kB Heap) und ML-KEM (28 kB Stack) nie gleichzeitig — der Schlüsseltausch läuft in `big()` vor und nach der Anfrage. Der Server nennt immer `Content-Length` (Go schickt über 2 kB sonst chunked). Protokolländerungen erst mit `tests/c2/run.sh` auf dem PC prüfen, dann aufs Gerät. Das Board hängt am Testplatz an einem schwachen WLAN (um −80 dBm): große Pakete gehen dort zeitweise verloren — erst `ping -s 1400` prüfen, bevor man im Code sucht. Diagnose am Gerät: `lcd_debug.py c2`. Die Content-Security-Policy der Admin-Weboberfläche bleibt bei `style-src 'self'`: xterm.js läuft darunter nur mit `web/xterm-csp.js` (vor `xterm.js` laden), nicht `unsafe-inline` erlauben. Im Browsertest kein `full_page`-Bild, solange das Terminal den Fokus hat — der nächste Klick verfehlt sonst seinen Knopf.

## Offene Punkte (Priorität)

0. Fernkonsole (Phase 4): Ausgabe des Ports am Board prüfen, sobald der MAX3232 dran ist
1. Waveshare: MAX3232 anschließen, echter Konsolenzugriff; SD-Karte; Akkubetrieb — Liste in `firmware/WAVESHARE.md` („Offen“)
2. 802.1X gegen echten RADIUS, danach PKCS#12 und CSR
3. Gehäuse für das Waveshare-Board (es gibt noch keins)
4. Docs-Artifact `2f26e902-4f1e-4797-bfbc-4037495d8121` beschreibt noch das LCDwiki-Basisboard — auf Waveshare umschreiben
