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

Das Waveshare-Board meldet sich am USB-Kabel als `/dev/ttyACM0` (nach einem Aus-/Einschalten auch `ttyACM1`; `lcd_debug.py` sucht den Port selbst). `lcd_debug.py` braucht `pyserial` — mit dem Python aus PlatformIOs eigener Umgebung starten, wenn das System-Python es nicht hat.

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

Phase 4, die Fernkonsole (seit 11. Oktober 2026, `server/KONZEPT.md` §4.5): Der Administrator bedient einen seriellen Port aus dem Browser (xterm.js in der Admin-Weboberfläche); jede Sitzung braucht einen Tipp am Touch-Display, läuft höchstens 60 Minuten und ist von beiden Seiten zu beenden. Server: `internal/server/console.go`; Firmware: `src/c2_console.cpp` und der Konsolenteil von `src/c2.cpp`. Nicht Ende-zu-Ende — der Server leitet die Zeichen im Klartext durch. Geprüft am Server (Go-Unittests, `server/test/ui.sh` gegen den Simulator) und am 11. Oktober 2026 am Waveshare-Board über Admin-API, Admin-Weboberfläche in Chromium und `lcd_debug.py`: Anfragen, Ablehnen, Erlauben, Eingabe bis zum Port, Beenden von allen Seiten, Zeitablauf. **Nicht geprüft:** die Richtung Port → Server am Board (nichts am Port angeschlossen), BREAK auf der Leitung. Ungeklärt: In einem von zwei Browserläufen meldete sich das Board nach einem abgeholten `ping` rund 50 s nicht (Funkstrecke danach in Ordnung). Der Strom-Code (`c2_console.cpp`) läuft auch in `tests/c2/run.sh` auf dem PC gegen den Server. `/api/c2/enroll` per curl braucht `-H 'Content-Type: application/json'`.

Phase 5 ist begonnen (seit 11. Oktober 2026, `server/KONZEPT.md` §5.1): Konten mit Passwort und TOTP, Anmeldung über Microsoft Entra ID (OIDC, nur gegen einen nachgebauten Anbieter getestet, nicht gegen ein echtes Entra), Protokoll aller verändernden Admin-Aufrufe, Begrenzung der Anfragerate je Adresse; `C2_ADMIN_TOKEN` ist jetzt freiwillig, sobald es ein Konto gibt. Seit 12. Oktober 2026 dazu: Zustand wahlweise in **PostgreSQL** (`C2_DATABASE_URL`, `internal/store/postgres.go`) und **Caddy** davor (`docker-compose.cloud.yml`, `Caddyfile`, `.env.example`); die lokale `docker-compose.yml` bleibt beim Server allein mit der JSON-Datei. Geprüft mit `server/test/pg.sh` (alle Go-Tests gegen ein echtes PostgreSQL), `server/test/cloud.sh` (ganzer Aufbau samt Umzug von der Datei, Caddy mit eigener Zertifizierungsstelle) und mit dem Waveshare-Board über Caddy im lokalen Netz (Anmelden, Aufträge). **Nicht geprüft:** echtes Zertifikat von Let's Encrypt, Betrieb im Internet, Fernkonsole am Board über Caddy, Ausfall der Datenbank im Betrieb. Seit 12. Oktober 2026 dazu die **Sicherung** (`server/backup.sh`, Dienst `backup` in `docker-compose.cloud.yml`): täglich ein `pg_dump` ins Volume `backups`, die neuesten 14 bleiben; zurückgespielt wird nur bei gestopptem Server. Geprüft mit `server/test/backup.sh` (auch nach dem Verlust aller Volumes), nicht über einen echten Tag und nicht mit einem echten Gerät. Die Archive sind unverschlüsselt und bleiben auf dem Rechner — Wegbringen ist nicht eingebaut. Ebenfalls seit 12. Oktober 2026 die **Überwachung**: `/healthz` antwortet 503, wenn der Zustand nicht geschrieben werden kann, `GET /admin/v1/status` nennt Grund, Geräte je Zustand, stille Geräte und abgewiesene Anfragen; Healthchecks der Container (`c2server -health`); der Server prüft alle 30 s selbst (`watch` in `cmd/c2server/main.go`), nimmt nach einem Neustart der Datenbank die Sperre neu und beendet sich, wenn ein anderer Prozess sie hat (`store.ErrTaken`). Geprüft mit den Go-Tests und `cloud.sh` (Datenbank im Betrieb gestoppt und gestartet). Benachrichtigt wird niemand — ein Wächter von außen fehlt. Und der **Wechsel des Serverschlüssels** (`server/KONZEPT.md` §4.6, `POST /admin/v1/server/rotate-key`): Der Server behält den alten Schlüssel für eine Frist, kündigt den neuen in der Antwort auf eine Abfrage an (`rekey`), das Gerät legt ihn im NVS neben den alten (`spub2`), bestätigt mit `sk` und pinnt ihn, sobald ein Handshake mit dem alten scheitert und mit dem neuen gelingt (`sessionHandshake()` in `src/c2.cpp`; portabel: `C2Proto::announcedKey`, `keyTag`). Geprüft mit Go-Tests, `cloud.sh` und `tests/c2/run.sh` — **nicht am Board** (Firmware nur übersetzt, nicht geflasht), in der Admin-Weboberfläche unter „Server“ (`ui.sh`). Schützt nicht nach einem Diebstahl des alten Schlüssels. Damit sind die Bausteine von Phase 5 gebaut; es fehlen der Lauf am Board und der mit echtem Zertifikat. Einziges fremdes Go-Paket ist der Treiber `pgx`.

Regeln daraus: Aufträge führt die Hauptschleife aus (`C2::loop()`), nicht der Netz-Task; neue Auftragsarten an zwei Stellen eintragen (`commandArgs` am Server, `execute()` am Gerät) und alles Verändernde an `c2Control` binden. TLS (45 kB Heap) und ML-KEM (28 kB Stack) nie gleichzeitig — der Schlüsseltausch läuft in `big()` vor und nach der Anfrage. Der Server nennt immer `Content-Length` (Go schickt über 2 kB sonst chunked). Protokolländerungen erst mit `tests/c2/run.sh` auf dem PC prüfen, dann aufs Gerät. Das Board hängt am Testplatz an einem schwachen WLAN (um −80 dBm): große Pakete gehen dort zeitweise verloren — erst `ping -s 1400` prüfen, bevor man im Code sucht. Diagnose am Gerät: `lcd_debug.py c2`. Die Content-Security-Policy der Admin-Weboberfläche bleibt bei `style-src 'self'`: xterm.js läuft darunter nur mit `web/xterm-csp.js` (vor `xterm.js` laden), nicht `unsafe-inline` erlauben. Neue Admin-Aufrufe über `sv.admin(...)` anmelden — das prüft Anmeldung und `X-C2` und schreibt das Protokoll; was nicht ins Protokoll soll, setzt `note(r).skip`. Der Zustand: `store.go`/`users.go` ändern nur den Speicher und merken in `s.ch` vor, was sich geändert hat (`s.ch.device(id)`, `.cmd`, `.con`, `.token`, `.user`, `.meta`) — wer ein neues Feld oder eine neue Änderung einbaut, merkt sie dort vor und trägt die Spalte in `postgres.go` an drei Stellen ein (neue Migration anhängen, nie eine ausgelieferte ändern; `load`; `save`), sonst steht sie nur in der JSON-Datei. Wer die Nachrichten zwischen Gerät und Server erweitert, tut es an drei Stellen: `deviceMsg`/`serverMsg` am Server, `internal/device` (Simulator, Go-Tests) und `src/c2.cpp` — und baut es in `tests/c2/host_client.cpp` nach, sonst prüft `run.sh` es nicht. Ein neuer Speicherort braucht neben `load`/`save`/`close` auch `check` (`backend.go`). Text von Geräten oder aus Formularen geht über `txt()` in die Datenbank. Ein Serverprozess je Datenbank. Zurückspielen einer Sicherung dreht auch Sperren zurück (danach Geräteliste durchgehen); `docker compose run` startet abhängige Dienste mit — der Dienst `backup` hängt deshalb mit Absicht nicht am Server. Caddy darf nicht komprimieren (`encode`), sonst fehlt den Geräten `Content-Length`. Die Go-Tests laufen mit fester Uhr: Wer viele Anmeldungen oder Handshakes in einem Test braucht, stellt die Uhr vor, sonst greift die Anfragerate. Im Browsertest kein `full_page`-Bild, solange das Terminal den Fokus hat — der nächste Klick verfehlt sonst seinen Knopf.

## Gestaltung (seit 11. Oktober 2026)

Alle drei Oberflächen — Admin-Seite der Leitstelle (`server/internal/server/web/style.css`), Weboberfläche des Geräts (`firmware/web/style.css`) und Touch-Display (`src/lcd_ui.cpp`, Farbtabelle oben) — folgen derselben, an Apples Gestaltungssprache angelehnten Linie: Systemschrift, schwarzer bzw. ruhiger Grund mit grauen Flächen in klaren Stufen, **eine** Akzentfarbe (Blau `#0a84ff`/`#0071e3`) für alles, was irgendwohin führt, Grün/Orange/Rot nur für Zustände. Regeln, die bei Änderungen gelten:

- Was sich drücken lässt, antwortet beim Drücken, nicht beim Loslassen (`:active` ohne Verzögerung, am Display die Akzentfarbe beim Berühren).
- Was hereinkommt, geht auf demselben Weg wieder: Das Einstellungsblatt der Geräteseite fährt von unten herein und dorthin zurück, als Transition (`#panel.open`), damit es sich mitten im Schließen wieder öffnen lässt; `hidden` wird erst gesetzt, wenn es draußen ist.
- Ziele für den Daumen mindestens 44 px hoch; Zustände nie nur als Farbe, immer mit Wort.
- `prefers-reduced-motion`, `prefers-reduced-transparency` und `prefers-contrast` sind in beiden Stylesheets bedient — bei neuen Animationen mitpflegen.
- Die Admin-Seite hat vier Bereiche (Geräte, Benutzer, Protokoll, Server), der Bereich steht im `#`-Teil der Adresse (`route()` in `app.js`). **Eine** Navigation im Markup, zweimal gezeigt: oben in der Leiste, bis 760 px Breite als Leiste am unteren Rand. Die Leiste oben trägt ihr Milchglas auf `header::before` — ein `backdrop-filter` auf `header` selbst würde die unten festgemachte Navigation an die Leiste binden. Tabellen mit Klasse `stack` werden schmal zu einem Block je Zeile; dafür nach dem Füllen `label(tabelle)` aufrufen. Fragen vor Unumkehrbarem über `ask()`, nicht `confirm()`. Neue Bereiche und Dialoge in `ui_test.py` auch im Abschnitt `phone` prüfen (`fits`, `targets`).
- Die Geräteseite lässt sich ohne Board ansehen: `tools/mock_device.js` (braucht `npm install ws`; ist Port 8080 belegt: `HTTP_PORT=18090 WS_PORT=18091`).

Geprüft per Screenshot: Admin-Seite über `server/test/ui.sh` (seit 12. Oktober 2026 auch als Handy, 390 × 844, hell und dunkel; auf dem neuen Rechner unter einer Minute und stabil), Geräteseite im Simulator und vom echten Board (Chromium, Handy-Format), Display über `lcd_debug.py shot`. Nicht geprüft: echte Handys, Safari, heller Modus der Admin-Seite im Browser, die Medienabfragen für weniger Bewegung/Transparenz/mehr Kontrast, und wie sich das Drücken unter einem echten Finger anfühlt.

## Übergabe auf einen neuen Entwicklungsrechner (12. Oktober 2026)

Bis hierher wurde auf einem Raspberry Pi entwickelt; der ist aufgeräumt (kein Checkout, kein PlatformIO, keine Container mehr). Der Stand ist die Vorabversion `v1.8.0-dev.20261012` auf GitHub. Angaben in den Dokumenten wie „auf dem Pi“, `192.168.10.232` (der Pi) und `192.168.10.163` (das Board im WLAN) beschreiben den alten Testplatz, nicht den neuen.

**Einrichten:** PlatformIO (Firmware), Docker mit Compose (Server; Go muss nicht installiert sein), dazu `curl`, `openssl`, `python3`, `g++` für die Tests. Erster Lauf zum Prüfen der Umgebung: `tests/` nach `tests/README.md`, dann `server/test/e2e.sh`, `tests/c2/run.sh`, `server/test/pg.sh`, `server/test/cloud.sh`, `server/test/backup.sh`; `server/test/ui.sh` zieht ein 3-GB-Image und war auf dem Pi langsam und wackelig (Zeitüberschreitungen) — auf einem größeren Rechner neu beurteilen.

**Das Board:** trägt die Firmware dieses Stands (neue Gestaltung, Fernkonsole), ist bei keiner Leitstelle angemeldet und kennt nur das WLAN des alten Testplatzes — am neuen Platz über den Hotspot `RS232-FE0C` und `http://192.168.4.1` neu einrichten.

**Stand am neuen Rechner (12. Oktober 2026, CachyOS, x86-64):** eingerichtet und alle genannten Läufe außer `ui.sh` grün. Nötig waren die Pakete `docker-buildx` (das Dockerfile braucht BuildKit), `platformio-core` und `python-pip`. Ein Firmware-Build schreibt `src/web_assets.h` dort neu, obwohl sich an `web/` nichts geändert hat (anderes zlib, andere gzip-Bytes) — nicht mit committen, `git checkout firmware/src/web_assets.h`. Das Board hängt am USB (`/dev/ttyACM0`), ist aber noch nicht neu eingerichtet.

**Als Nächstes** (Phase 5 zu Ende): Board am neuen Platz einrichten, Firmware flashen und den Schlüsselwechsel am Board prüfen; die Admin-Weboberfläche auf einem echten Handy ansehen; danach ein Lauf mit echtem Zertifikat (öffentlicher Name nötig). Vorher gehört der Server nicht ins Internet.

**Entscheidungen des Besitzers, die gelten:**
- Speicher PostgreSQL, TLS über Caddy, zweiter Faktor TOTP, dazu Anmeldung über Microsoft Entra ID als Möglichkeit.
- Die strenge Content-Security-Policy bleibt (`style-src 'self'`, kein `unsafe-inline`).
- Die Werkzeuge bleiben in Python (kein Umschreiben nach Rust).
- Kein Firmware-Update über die Leitstelle ohne Signaturprüfung.
- Committen und pushen nur auf Ansage. Commit-Nachrichten deutsch, im Stil „Leitstelle Phase N: …“; Dokumentation deutsch, Code-Kommentare englisch.

**Ungeklärt aus der letzten Phase:** Das Board meldete sich in einem Browserlauf rund 50 s nicht (Funk danach in Ordnung). Über Caddy dauerte sein Handshake 6,3 s statt gut einer Sekunde — vermutet wird der Sparmodus bei 80 MHz, nachgeprüft ist es nicht. Entra ist nie gegen einen echten Mandanten gelaufen, TOTP nie mit einer echten Authenticator-App.

## Offene Punkte (Priorität)

0. Leitstelle Phase 5 abschließen: Schlüsselwechsel am Board prüfen (siehe Übergabe); Admin-Weboberfläche auf echtem Handy und in Safari ansehen; Sicherungen vom Rechner wegbringen und ein Wächter von außen auf `/healthz` (wohin und womit, entscheidet der Besitzer)
0. Fernkonsole (Phase 4): Ausgabe des Ports am Board prüfen, sobald der MAX3232 dran ist; Fernkonsole am Board über Caddy
1. Waveshare: MAX3232 anschließen, echter Konsolenzugriff; SD-Karte; Akkubetrieb — Liste in `firmware/WAVESHARE.md` („Offen“)
2. 802.1X gegen echten RADIUS, danach PKCS#12 und CSR
3. Gehäuse für das Waveshare-Board (es gibt noch keins)
4. Docs-Artifact `2f26e902-4f1e-4797-bfbc-4037495d8121` beschreibt noch das LCDwiki-Basisboard — auf Waveshare umschreiben
