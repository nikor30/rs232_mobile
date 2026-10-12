# Leitstelle (Command-and-Control-Server)

Server, bei dem sich die RS232-Konsolen anmelden und Aufträge abholen. Idee, Protokoll, Phasen und Grenzen: **[KONZEPT.md](KONZEPT.md)**. Stand: Phasen 1 bis 3 — Server mit Weboberfläche, Gerätesimulator, Client in der Firmware —, geprüft im Container und mit dem Waveshare-Board im lokalen Netz. Phase 4, die Fernkonsole, ist am Server mit dem Simulator und am Waveshare-Board getestet; am Board fehlt die Richtung Port → Server, weil dort nichts am seriellen Port hängt (KONZEPT.md §7).

**Noch nicht ins Internet stellen.** Phase 5 ist nicht fertig (KONZEPT.md §6): Konten, Caddy und PostgreSQL gibt es, Sicherung, Wechsel des Serverschlüssels und Überwachung nicht. Die lokale Compose-Datei bindet den Port deshalb nur an `127.0.0.1`, solange `C2_BIND` nichts anderes sagt.

## Starten

```bash
cd server
echo "C2_ADMIN_TOKEN=$(head -c 24 /dev/urandom | base64)" > .env
docker compose up -d --build        # erster Bau auf dem Raspberry Pi: rund 10 Minuten
curl -k https://127.0.0.1:8443/healthz
```

Die **Weboberfläche** liegt unter <https://127.0.0.1:8443/> (selbstsigniertes Zertifikat, der Browser warnt): Geräte einladen, bestätigen, Aufträge schicken, sperren. Alles Folgende geht dort auch per Klick.

**Das erste Konto:** auf der Anmeldeseite „Mit dem Admin-Token“ aufklappen und das Token eintippen, unter „Benutzer“ ein Konto anlegen — es erscheint ein Einrichtungscode, einmal. Abmelden, „Konto einrichten“: Name, Einrichtungscode, Passwort, dann den QR-Code mit einer Authenticator-App scannen und deren Code eintippen. Danach meldet man sich mit Name, Passwort und Code an. Wer will, nimmt anschließend `C2_ADMIN_TOKEN` aus der `.env` — dann kommen nur noch Konten herein. Wie das gedacht ist: KONZEPT.md §5.1.

Der Container enthält nur die beiden Programme (`/c2server`, `/devsim`), läuft ohne Root und mit schreibgeschütztem Dateisystem; der Zustand liegt im Volume `c2data`.

## Mit Caddy und PostgreSQL

Für einen Server, den andere erreichen: `docker-compose.cloud.yml` stellt **Caddy** davor (echtes Zertifikat von Let's Encrypt, von selbst erneuert, Umleitung von http auf https, HSTS) und legt den Zustand in **PostgreSQL** statt in `state.json`. Nach außen offen sind nur die Ports 80 und 443 von Caddy; Server und Datenbank haben keinen veröffentlichten Port, die Datenbank hängt an einem Netz ohne Weg nach draußen.

```bash
cd server
cp .env.example .env                # C2_DOMAIN, C2_CADDY_TLS, C2_DB_PASSWORD, C2_ADMIN_TOKEN eintragen
docker compose -f docker-compose.cloud.yml up -d --build
```

- `C2_DOMAIN` ist der Name, unter dem der Server erreichbar ist; sein DNS-Eintrag muss auf diesen Rechner zeigen und die Ports 80 und 443 müssen von außen ankommen, sonst bekommt Caddy kein Zertifikat. `C2_CADDY_TLS` ist die E-Mail-Adresse für Let's Encrypt — oder `internal`, dann stellt Caddy das Zertifikat mit einer eigenen Zertifizierungsstelle aus (für einen Test ohne Internet; der Browser warnt).
- **Umzug von der Datei:** Findet der Server beim ersten Start mit leerer Datenbank eine `state.json` im Volume `c2data`, übernimmt er sie — Serverschlüssel, Geräte, Aufträge, Konten, Protokoll — und benennt sie in `state.json.imported` um. Wer beide Compose-Dateien im selben Verzeichnis startet, hat dasselbe Volume; die Geräte merken vom Umzug nichts, solange `C2_PUBLIC_URL` gleich bleibt. Die umbenannte Datei enthält weiter den Serverschlüssel: sicher verwahren oder löschen.
- **Ein Serverprozess je Datenbank.** Der Server rechnet im Speicher und schreibt jede Änderung in einer Transaktion in die Datenbank; ein zweiter Prozess an derselben Datenbank verweigert den Start.
- Die Datenbank enthält den privaten Serverschlüssel, die Passwort-Hashes und die TOTP-Geheimnisse. Wer das Volume `pgdata` verliert, muss alle Geräte neu anmelden. Dagegen gibt es die Sicherung (nächster Abschnitt).
- Hineinsehen: `docker compose -f docker-compose.cloud.yml exec db psql -U c2 c2` — Tabellen `devices`, `commands`, `consoles`, `users`, `audit`, `tokens`, `server`. Das Protokoll behält dort 200 000 Einträge; die Weboberfläche zeigt die neuesten.

**Stand:** auf dem Raspberry Pi mit `C2_CADDY_TLS=internal` getestet (`test/cloud.sh`, und mit dem Waveshare-Board im lokalen Netz), nicht mit einem echten Zertifikat und nicht im Internet. Der Wechsel des Serverschlüssels fehlt noch — die Warnung oben gilt weiter.

### Sicherung und Wiederherstellung

Zum Aufbau gehört der Dienst `backup` (`backup.sh`): Er schreibt alle 24 Stunden eine Sicherung der ganzen Datenbank (`pg_dump`, ein Schnappschuss, der Server läuft dabei weiter) ins Volume `backups` und behält die neuesten 14 (`C2_BACKUP_EVERY_H`, `C2_BACKUP_KEEP` in `.env`). Scheitert eine, versucht er es nach fünf Minuten wieder; gibt es zwei Abstände lang keine, meldet `docker compose ps` den Dienst als `unhealthy`.

```bash
C="docker compose -f docker-compose.cloud.yml"
$C exec backup /bin/sh /backup.sh list                 # was da ist
$C exec backup /bin/sh /backup.sh once                 # jetzt eine, z. B. vor einem Umbau
$C cp backup:/backups/c2-20261012T030000Z.dump .       # vom Rechner herunterholen
```

- **Die Sicherungen liegen auf demselben Rechner wie die Datenbank.** Gegen einen kaputten Rechner hilft nur eine Kopie woanders — das Herunterholen und Wegbringen ist nicht eingebaut. `docker compose down -v` löscht auch das Volume `backups`.
- **Eine Sicherung ist so geheim wie die Datenbank** (privater Serverschlüssel, Passwort-Hashes, TOTP-Geheimnisse) und **nicht verschlüsselt**. Vor dem Wegbringen verschlüsseln, zum Beispiel mit `age` oder `gpg`.
- Zur Sicherung gehört die `.env` (Name, Einstellungen); das Datenbankpasswort darf nach einem Verlust ein neues sein. Caddys Zertifikate braucht man nicht — er holt neue.

**Zurückspielen** — der Server muss dabei stehen, sonst schreibt er seinen Stand aus dem Speicher darüber (`restore` weigert sich, solange er läuft):

```bash
$C stop c2server
$C exec backup /bin/sh /backup.sh restore c2-20261012T030000Z.dump
$C start c2server
```

Nach dem Verlust des Rechners oder aller Volumes — den Server erst starten, wenn die Sicherung drin ist, sonst macht er sich einen neuen Schlüssel:

```bash
$C up -d db backup
$C cp c2-20261012T030000Z.dump backup:/backups/
$C exec backup /bin/sh /backup.sh restore c2-20261012T030000Z.dump
$C up -d
```

Zurückgespielt wird alles oder nichts; eine kaputte Datei lässt die Datenbank, wie sie war. **Was nach der Sicherung geschah, ist danach weg:** Später angemeldete Geräte kennt der Server nicht mehr (neu anmelden), und ein später **gesperrtes Gerät ist wieder aktiv** — nach dem Zurückspielen die Geräteliste durchgehen und erneut sperren. Dasselbe gilt für gesperrte oder zurückgesetzte Konten.

**Stand:** `test/backup.sh` (von selbst geschriebene Sicherung, Zurückspielen über einen späteren Stand, Verlust aller Volumes, Aufräumen). Nicht geprüft: eine Sicherung aus einem älteren Schema in einen neueren Server (es gibt erst ein Schema), große Datenbanken, und ob jemand nach einem echten Ausfall mit dieser Anleitung zurechtkommt.

### Überwachung

- **`GET /healthz`** (ohne Anmeldung) antwortet `200 ok`, solange der Server seinen Zustand schreiben kann, sonst `503 failing` — ohne Begründung; die steht im Log und in `/admin/v1/status`. Das ist die Adresse für einen Wächter von außen (`https://<C2_DOMAIN>/healthz`). Die Datenbank wird dafür höchstens alle 10 Sekunden gefragt.
- **Container:** `docker compose ps` zeigt `healthy`/`unhealthy` für den Server (`/c2server -health` fragt das eigene `/healthz`), die Datenbank und die Sicherung (`unhealthy`, wenn zwei Abstände lang keine Sicherung entstand).
- **Log:** Der Server prüft zweimal in der Minute selbst und schreibt `state: CANNOT BE SAVED: …` und später `state: can be saved again`; dazu wie bisher jede Anmeldung und jeden verändernden Aufruf (`audit: …`).
- **`GET /admin/v1/status`** (mit Anmeldung) für alles Weitere, zum Beispiel aktive Geräte, die sich länger als fünf Abfrageabstände (mindestens fünf Minuten) nicht gemeldet haben (`devices.silent`).
- **Fällt die Datenbank aus,** läuft der Server weiter: Geräte bekommen Antworten aus dem Speicher, Änderungen werden mit Fehler 500 abgewiesen — bleiben aber im Speicher und werden geschrieben, sobald die Datenbank wieder da ist. Die Sperre gegen einen zweiten Serverprozess nimmt der Server danach neu; hat inzwischen ein anderer Prozess die Datenbank übernommen, beendet er sich.
- **Nicht eingebaut ist das Benachrichtigen:** Niemand bekommt von selbst eine Nachricht. Dafür braucht es einen Wächter außerhalb dieses Rechners, der `/healthz` abfragt — er merkt dann auch, wenn der ganze Rechner weg ist.

**Stand:** Go-Tests und `test/cloud.sh` (Datenbank im Betrieb gestoppt und wieder gestartet). Nicht geprüft: ein echter Wächter von außen, eine Datenbank, die hängt statt zu fehlen, ein Ausfall über Stunden.

## Ein simuliertes Gerät anmelden

```bash
. ./.env; A="Authorization: Bearer $C2_ADMIN_TOKEN"; S=https://127.0.0.1:8443
sim() { docker run --rm --network server_default -v c2dev:/data --entrypoint /devsim rs232-c2 \
          -state /data/device.json -tls-skip-verify "$@"; }

# 1. einladen
curl -sk -X POST $S/admin/v1/enroll-tokens -H "$A" -d '{"ttl_s":600}'     # -> {"token":"c2e1:...",...}
# 2. das Gerät meldet sich an und zeigt seinen Code
sim enroll -name Labor-1 'c2e1:...'                                        # -> ID ... / CODE 123456
# 3. den Code vom Gerät am Server eintippen
curl -sk -X POST $S/admin/v1/devices/<ID>/approve -H "$A" -d '{"sas":"123456"}'
# 4. Auftrag einreihen, Gerät holt ihn ab, Ergebnis ansehen
curl -sk -X POST $S/admin/v1/devices/<ID>/commands -H "$A" -d '{"type":"ping"}'
sim run -n 1                          # mit -control auch "play" und "reboot", sonst lehnt der Simulator sie ab wie ein Gerät ohne Freigabe
curl -sk $S/admin/v1/devices/<ID>/commands -H "$A"
```

`-tls-skip-verify` gilt dem selbstsignierten Zertifikat des lokalen Servers. Der innere Kanal hängt davon nicht ab: Der Server muss weiterhin den Schlüssel beweisen, dessen Fingerabdruck im Token steht.

## Ein echtes Gerät anmelden

Das Gerät muss den Server erreichen, also an die LAN-Adresse dieses Rechners binden und sie als öffentliche Adresse nennen (in `.env` oder davor):

```bash
C2_BIND=192.168.10.232 C2_PUBLIC_URL=https://192.168.10.232:8443 docker compose up -d
```

Dann wie oben einladen. Das Token kommt in die Weboberfläche des Geräts (Menü → Netz → Leitstelle → Anmelden) oder per `curl -X POST http://<gerät>/api/c2/enroll -H 'Content-Type: application/json' -d '{"token":"c2e1:..."}'`; der WLAN-Client des Geräts muss verbunden sein. Der Code steht danach auf dem Display (Seite Info) und in der Weboberfläche. Details: `firmware/README.md`.

## Admin-Schnittstelle

Alle Aufrufe mit `Authorization: Bearer <C2_ADMIN_TOKEN>` oder mit dem Cookie eines angemeldeten Kontos; mit Cookie brauchen verändernde Aufrufe zusätzlich den Kopf `X-C2: 1`. Alles Verändernde steht danach im Protokoll.

| Aufruf | Wirkung |
|---|---|
| `POST /admin/v1/enroll-tokens` `{"ttl_s":600,"note":""}` | Einladungs-Token, einmal verwendbar |
| `GET /admin/v1/devices` | Geräte mit Zustand (`pending`, `active`, `revoked`), zuletzt gesehen, letzter Status |
| `POST /admin/v1/devices/{id}/approve` `{"sas":"123456"}` | bestätigen mit dem Code vom Gerätedisplay; fünf falsche Eingaben verwerfen die Anmeldung |
| `POST /admin/v1/devices/{id}/revoke` | sperren, sofort und endgültig |
| `POST /admin/v1/devices/{id}/commands` `{"type":"play","args":{"name":"Demo","port":1}}` | Auftrag einreihen. Arten: `ping`, `status`, `settings`, `configs` (lesen), `play`, `reboot` (nur mit Freigabe am Gerät) — KONZEPT.md §4.3 |
| `POST /admin/v1/devices/{id}/commands` `{"type":"console","args":{"port":1,"minutes":15}}` | Fernkonsole anfragen; am Gerät muss jemand auf dem Display zustimmen — KONZEPT.md §4.5 |
| `GET /admin/v1/devices/{id}/console` | Zustand der Fernkonsole (`requested`, `granted`, `open`, `ended`, `refused`) und das Sitzungsprotokoll |
| `GET /admin/v1/devices/{id}/console/{sitzung}/output?from=0` | Ausgabe des Ports ab einem Offset, base64; wartet bis zu 20 s auf Neues (`wait=` in Sekunden verkürzt das) |
| `POST /admin/v1/devices/{id}/console/{sitzung}/input` `{"d":"<base64>","brk":false}` | Eingabe für den Port, `brk` schickt ein BREAK; 429, wenn das Gerät nicht nachkommt |
| `POST /admin/v1/devices/{id}/console/{sitzung}/close` | Sitzung beenden |
| `GET /admin/v1/users` | Konten: Art, Zustand, letzte Anmeldung — keine Geheimnisse |
| `POST /admin/v1/users` `{"name":"anna","kind":"local"}` oder `{"name":"ben","kind":"entra","entra":"ben@firma.de"}` | Konto anlegen; für `local` kommt der Einrichtungscode zurück, einmal |
| `POST /admin/v1/users/{name}/reset` | Passwort und Authenticator-App verwerfen, neuer Einrichtungscode |
| `POST /admin/v1/users/{name}/disable` `{"disabled":true}` | sperren oder freigeben; Sperren beendet die Sitzungen |
| `DELETE /admin/v1/users/{name}` | Konto löschen (nicht das eigene) |
| `GET /admin/v1/audit?n=200` | Protokoll, neueste zuerst |
| `GET /admin/v1/server` | Fingerabdruck des Serverschlüssels, öffentliche Adresse, Auftragsarten |
| `GET /admin/v1/status` | Überwachung: Laufzeit, ob der Zustand gespeichert werden kann (sonst Fehler und seit wann), Geräte je Zustand und wie viele aktive still sind, offene Fernkonsolen, angemeldete Browser, abgewiesene Anfragen (429) seit dem Start |
| `GET /admin/v1/devices/{id}/commands` | Aufträge mit Zustand (`queued`, `sent`, `done`, `failed`, `expired`) und Ergebnis |

## Anmeldung über Microsoft Entra ID

1. Im Entra Admin Center eine **App-Registrierung** anlegen, „Nur Konten in diesem Organisationsverzeichnis“. Als Umleitungs-URI (Plattform „Web“) `<C2_PUBLIC_URL>/auth/entra/callback` eintragen.
2. Unter „Zertifikate & Geheimnisse“ einen **geheimen Clientschlüssel** erzeugen.
3. Dem Server mitgeben: `C2_ENTRA_TENANT` (Verzeichnis-ID, die GUID — nicht der Domainname), `C2_ENTRA_CLIENT_ID` (Anwendungs-ID), `C2_ENTRA_CLIENT_SECRET`.
4. In der Weboberfläche unter „Benutzer“ für jede Person ein Konto der Art „Microsoft Entra ID“ anlegen, mit ihrem Anmeldenamen (`name@firma.de`) oder ihrer Objekt-ID. Ohne ein solches Konto kommt niemand herein, auch nicht aus dem eigenen Mandanten.

Auf der Anmeldeseite erscheint dann „Mit Microsoft anmelden“. Den zweiten Faktor verlangt Entra (Conditional Access), nicht dieser Server. **Stand:** nur gegen einen nachgebauten Identitätsanbieter getestet, nicht gegen ein echtes Entra (KONZEPT.md §7).

## Einstellungen (Umgebung)

| Variable | Vorgabe | Bedeutung |
|---|---|---|
| `C2_ADMIN_TOKEN` | leer | Token der Admin-Schnittstelle, mindestens 16 Zeichen. Nötig, bis es ein Konto gibt; leer heißt: nur Konten |
| `C2_ENTRA_TENANT`, `C2_ENTRA_CLIENT_ID`, `C2_ENTRA_CLIENT_SECRET` | leer | Anmeldung über Microsoft Entra ID (siehe oben); alle drei oder keine |
| `C2_DATABASE_URL` | leer | `postgres://benutzer:passwort@host:5432/datenbank?sslmode=…`: Zustand in PostgreSQL statt in `state.json`. Die Cloud-Compose-Datei setzt das selbst aus `C2_DB_PASSWORD` |
| `C2_DOMAIN`, `C2_CADDY_TLS`, `C2_DB_PASSWORD` | – | nur `docker-compose.cloud.yml` (siehe oben); dort auch `C2_HTTP_PORT`, `C2_HTTPS_PORT` (Vorgabe 80, 443) |
| `C2_BACKUP_EVERY_H`, `C2_BACKUP_KEEP` | `24`, `14` | nur `docker-compose.cloud.yml`: Abstand der Sicherungen in Stunden und wie viele bleiben |
| `C2_TRUST_PROXY` | leer | `1`: Absenderadresse aus `X-Forwarded-For` nehmen (für Anfragerate und Protokoll). Nur hinter einem Proxy, der diesen Kopf setzt |
| `C2_PUBLIC_URL` | `https://c2server:8443` (Compose) | Adresse, unter der Geräte den Server erreichen; steht im Einladungs-Token |
| `C2_TLS` | `selfsigned` | `selfsigned`, `files` (`C2_TLS_CERT`, `C2_TLS_KEY`) oder `off` (hinter einem Proxy, der TLS beendet) |
| `C2_TLS_HOSTS` | `localhost,127.0.0.1,c2server` | Namen im selbstsignierten Zertifikat |
| `C2_LISTEN` | `:8443` | Adresse und Port im Container |
| `C2_BIND` | `127.0.0.1` | nur Compose: Adresse dieses Rechners, an die der Port gebunden wird |
| `C2_DATA_DIR` | `/data` | Zustand: `state.json` (enthält den privaten Serverschlüssel), TLS-Zertifikat. Mit `C2_DATABASE_URL` wird hier nur einmal eine `state.json` zum Übernehmen gesucht |
| `C2_POLL_S` | `30` | Abstand, in dem Geräte sich melden sollen |

Wer das Datenverzeichnis (mit PostgreSQL: die Datenbank und ihre Sicherungen) verliert, verliert den Serverschlüssel — alle Geräte müssten neu angemeldet werden. Für den Betrieb mit der Datei gibt es keine eingebaute Sicherung: `state.json` bei gestopptem Server kopieren.

## Tests

```bash
./test/e2e.sh                       # alles: Unit-Tests, Image, Ende-zu-Ende im Container
./test/ui.sh                        # die Weboberfläche in einem Browser ohne Bildschirm (Playwright-Image, 3 GB)
./test/pg.sh                        # die Unit-Tests noch einmal, mit dem Zustand in einem echten PostgreSQL
./test/cloud.sh                     # Caddy + Server + PostgreSQL: Umzug von der Datei, Zertifikat, Konto, Ausfall der Datenbank, Neustart
./test/backup.sh                    # Sicherung der Datenbank und Zurückspielen, auch nach dem Verlust aller Volumes
../tests/c2/run.sh                  # der Protokollcode der Firmware auf dem PC gegen diesen Server
docker build --target test .        # nur Formatierung, go vet und Unit-Tests
```

Go muss nicht installiert sein; gebaut und getestet wird im Container. Der Simulator sagt mit `run -console` zu jeder Anfrage nach einer Fernkonsole Ja und spielt dann einen Switch (Prompt, Echo, Antwort auf `show` und BREAK); `e2e.sh` prüft die Fernkonsole nicht, das tun die Unit-Tests und `ui.sh`.

## Aufbau

```
cmd/c2server/        der Server
cmd/devsim/          Gerätesimulator
internal/proto/      Protokoll: Handshake, Nachrichten, Einladungs-Token  ← Vorlage für die Firmware
internal/device/     Geräteseite über HTTP (Simulator, Tests)
internal/server/     HTTP: Geräte-Endpunkte und Admin-Schnittstelle
internal/server/console.go   Fernkonsole: Zeichenströme zwischen Gerät und Browser
internal/server/auth.go      Konten, Sitzungen, TOTP, Protokoll;  entra.go: Anmeldung über Microsoft Entra ID;
                             ratelimit.go: Anfragerate je Absenderadresse
internal/server/web/ Weboberfläche (eingebettet); qrcode.js ist qrcode-generator von Kazuhiko Arase, MIT, Commit 83b7e8f;
                     xterm.js, xterm.css und addon-fit.js sind unverändert übernommen (LICENSE-xterm.txt),
                     xterm-csp.js lässt xterm unter der strengen Content-Security-Policy laufen und muss vor xterm.js geladen werden
internal/store/      Zustand: store.go, users.go rechnen im Speicher; backend.go schreibt die JSON-Datei,
                     postgres.go die Datenbank (Schema als Liste von Migrationen; Treiber pgx, das einzige fremde Paket)
docker-compose.yml   lokal: der Server allein, selbstsigniert, Zustand in der Datei
docker-compose.cloud.yml, Caddyfile, .env.example   Caddy davor, PostgreSQL dahinter
backup.sh            Sicherung der Datenbank (Dienst backup) und Zurückspielen
test/e2e.sh          Ende-zu-Ende-Test gegen den Container
test/ui.sh, ui_test.py   Browsertest der Weboberfläche
test/pg.sh, cloud.sh     Zustand in PostgreSQL; der ganze Aufbau mit Caddy
test/backup.sh           Sicherung und Wiederherstellung
```
