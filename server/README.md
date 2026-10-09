# Leitstelle (Command-and-Control-Server)

Server, bei dem sich die RS232-Konsolen anmelden und Aufträge abholen. Idee, Protokoll, Phasen und Grenzen: **[KONZEPT.md](KONZEPT.md)**. Stand: Phase 1 — Server und Gerätesimulator, lokal im Container getestet. Die Firmware kann noch nichts davon.

**Nicht ins Internet stellen.** Dafür fehlt Phase 5 (KONZEPT.md §6); die Compose-Datei bindet den Port deshalb nur an `127.0.0.1`.

## Starten

```bash
cd server
echo "C2_ADMIN_TOKEN=$(head -c 24 /dev/urandom | base64)" > .env
docker compose up -d --build        # erster Bau auf dem Raspberry Pi: rund 10 Minuten
curl -k https://127.0.0.1:8443/healthz
```

Der Container enthält nur die beiden Programme (`/c2server`, `/devsim`), läuft ohne Root und mit schreibgeschütztem Dateisystem; der Zustand liegt im Volume `c2data`.

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
sim run -n 1
curl -sk $S/admin/v1/devices/<ID>/commands -H "$A"
```

`-tls-skip-verify` gilt dem selbstsignierten Zertifikat des lokalen Servers. Der innere Kanal hängt davon nicht ab: Der Server muss weiterhin den Schlüssel beweisen, dessen Fingerabdruck im Token steht.

## Admin-Schnittstelle

Alle Aufrufe mit `Authorization: Bearer <C2_ADMIN_TOKEN>`.

| Aufruf | Wirkung |
|---|---|
| `POST /admin/v1/enroll-tokens` `{"ttl_s":600,"note":""}` | Einladungs-Token, einmal verwendbar |
| `GET /admin/v1/devices` | Geräte mit Zustand (`pending`, `active`, `revoked`), zuletzt gesehen, letzter Status |
| `POST /admin/v1/devices/{id}/approve` `{"sas":"123456"}` | bestätigen mit dem Code vom Gerätedisplay; fünf falsche Eingaben verwerfen die Anmeldung |
| `POST /admin/v1/devices/{id}/revoke` | sperren, sofort und endgültig |
| `POST /admin/v1/devices/{id}/commands` `{"type":"ping","args":{}}` | Auftrag einreihen (Arten: `ping`, `status`) |
| `GET /admin/v1/devices/{id}/commands` | Aufträge mit Zustand und Ergebnis |

## Einstellungen (Umgebung)

| Variable | Vorgabe | Bedeutung |
|---|---|---|
| `C2_ADMIN_TOKEN` | — (Pflicht, mindestens 16 Zeichen) | Token der Admin-Schnittstelle |
| `C2_PUBLIC_URL` | `https://c2server:8443` (Compose) | Adresse, unter der Geräte den Server erreichen; steht im Einladungs-Token |
| `C2_TLS` | `selfsigned` | `selfsigned`, `files` (`C2_TLS_CERT`, `C2_TLS_KEY`) oder `off` (hinter einem Proxy, der TLS beendet) |
| `C2_TLS_HOSTS` | `localhost,127.0.0.1,c2server` | Namen im selbstsignierten Zertifikat |
| `C2_LISTEN` | `:8443` | Adresse und Port |
| `C2_DATA_DIR` | `/data` | Zustand: `state.json` (enthält den privaten Serverschlüssel), TLS-Zertifikat |
| `C2_POLL_S` | `30` | Abstand, in dem Geräte sich melden sollen |

Wer das Datenverzeichnis verliert, verliert den Serverschlüssel — alle Geräte müssten neu angemeldet werden.

## Tests

```bash
./test/e2e.sh                       # alles: Unit-Tests, Image, Ende-zu-Ende im Container
docker build --target test .        # nur Formatierung, go vet und Unit-Tests
```

Go muss nicht installiert sein; gebaut und getestet wird im Container.

## Aufbau

```
cmd/c2server/        der Server
cmd/devsim/          Gerätesimulator
internal/proto/      Protokoll: Handshake, Nachrichten, Einladungs-Token  ← Vorlage für die Firmware
internal/device/     Geräteseite über HTTP (Simulator, Tests)
internal/server/     HTTP: Geräte-Endpunkte und Admin-Schnittstelle
internal/store/      Zustand (JSON-Datei)
test/e2e.sh          Ende-zu-Ende-Test gegen den Container
```
