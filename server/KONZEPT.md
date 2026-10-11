# Leitstelle (Command-and-Control-Server) — Konzept

**Stand:** 9. Oktober 2026 · Phasen 1 bis 3 umgesetzt (Server, Firmware-Client, Aufträge und Weboberfläche) · am Waveshare-Board gegen den Server im lokalen Netz geprüft, nicht im Internet

Ein Server im Internet, bei dem sich die RS232-Konsolen anmelden. Er weiß, welche Geräte es gibt und ob sie erreichbar sind, und kann ihnen Aufträge geben. Dieses Dokument beschreibt das Ziel, die Sicherheitsannahmen, das Protokoll und die Phasen. Was davon gebaut und geprüft ist, steht in §7; wie man es startet, in `README.md`; die Geräteseite in `firmware/README.md`.

## 1. Ziele

1. **Erreichbar aus dem Internet**, die Geräte dagegen nicht: Ein Gerät steht hinter NAT, im Hotspot eines Handys oder in einem Firmennetz. Es baut die Verbindung immer selbst auf (ausgehend, HTTPS). Der Server öffnet nie eine Verbindung zum Gerät.
2. **Zweiseitige Prüfung bei der Registrierung:** Das Gerät muss sicher sein, mit dem richtigen Server zu sprechen, und der Server, dass genau dieses Gerät gemeint ist — bestätigt von einem Menschen, der das Gerät vor sich hat.
3. **Leichtgewichtig:** wenige Kilobyte pro Kontakt, kurze HTTPS-Anfragen, kein dauerhaft offener Kanal. Der ESP32-S3 hat dafür rund 100 kB freien Heap neben WLAN, Bluetooth und Display.
4. **Sicher auch gegen künftige Quantenrechner:** Wer heute mitschneidet, soll den Verkehr auch später nicht entschlüsseln können, und die Anmeldung soll nicht an einem Verfahren hängen, das ein Quantenrechner bricht.
5. **Lokal testbar, später in die Cloud:** ein Container ohne äußere Abhängigkeiten, Konfiguration über Umgebungsvariablen.

Bewusst **nicht** Ziel der ersten Phasen: Fernzugriff auf die serielle Konsole. Das ist die mächtigste und heikelste Funktion (wer sie hat, sitzt auf der Konsole eines Switches) und kommt erst, wenn Anmeldung, Rechte und Protokollierung stehen (Phase 4).

## 2. Bedrohungen und Annahmen

| Angreifer | Was er kann | Schutz |
|---|---|---|
| Lauscher im Netz, heute | TLS-Verkehr mitschneiden und aufheben | innerer Kanal mit ML-KEM-768; ein später gebrochenes X25519 oder TLS reicht nicht |
| Mann in der Mitte (gefälschtes TLS-Zertifikat, DNS, fremdes WLAN) | sich als Server ausgeben | das Gerät vertraut nur dem Serverschlüssel, dessen Fingerabdruck im Einladungs-Token steht; TLS ist die äußere Hülle, nicht der Vertrauensanker |
| Fremder mit Internetzugang | ein eigenes Gerät anmelden, Aufträge abholen | Einladungs-Token (einmalig, läuft ab) und Bestätigungscode vom Gerätedisplay |
| Jemand, der ein Token abfängt | sein Gerät statt des gemeinten anmelden | der Mensch tippt den Code ein, den **sein** Gerät zeigt; ein fremdes Gerät hätte einen anderen |
| Jemand, der Geräte-ID und öffentlichen Schlüssel kennt | sich als das Gerät ausgeben | ohne den privaten Schlüssel lassen sich die Sitzungsschlüssel nicht ableiten |
| Dieb eines Geräts | privaten Schlüssel aus dem Flash lesen (keine Flash-Verschlüsselung) | Gerät am Server sperren; mehr nicht — siehe §8 |
| Angreifer auf dem Server | alles, was der Server darf | Befehlsliste klein halten; heikle Befehle brauchen später die Zustimmung am Gerät (Phase 4) |

Annahmen: Der Server und sein Datenverzeichnis sind vertrauenswürdig. Der Administrator schützt sein Token. Der Mensch, der ein Gerät anmeldet, hat es vor sich.

## 3. Aufbau

```
 Gerät (ESP32-S3)                     Internet                     Leitstelle (Container)
 ┌──────────────────┐   HTTPS, ausgehend, alle 30 s   ┌──────────────────────────────────┐
 │ Geräteschlüssel   │ ──────────────────────────────▶ │ /v1/server-key  /v1/handshake     │
 │ gepinnter Server- │   innen: hybrider Kanal         │ /v1/msg         (binär)           │
 │ schlüssel         │ ◀────────────────────────────── │                                    │
 └──────────────────┘   Antwort: Aufträge              │ /admin/v1/...   (JSON, Token)     │
                                                        │ Datenverzeichnis: Schlüssel,       │
 Administrator ── HTTPS ──────────────────────────────▶ │ Geräte, Aufträge                   │
                                                        └──────────────────────────────────┘
```

Zwei Schichten, mit Absicht:

- **Außen HTTPS.** Kommt durch Firewalls und Proxys, schützt die Admin-Schnittstelle, verbirgt Metadaten. Der Server (Go) handelt mit Clients, die es können, von selbst den hybriden Schlüsseltausch X25519MLKEM768 aus. Das Gerät kann das nicht: Arduino-Core 2.0.17 bringt mbedTLS 2 mit, also TLS 1.2 mit klassischen Verfahren.
- **Innen ein eigener Kanal** zwischen Gerät und Server, hybrid aus X25519 und ML-KEM-768. Er trägt die Sicherheit: Vertraulichkeit, gegenseitige Echtheit, Schutz vor Wiedereinspielen. Er hängt nicht davon ab, ob das TLS-Zertifikat geprüft wurde oder wo TLS endet (Load-Balancer in der Cloud).

„Hybrid" heißt: Jeder Schlüssel entsteht aus einem klassischen **und** einem Post-Quanten-Geheimnis. Bricht eines der beiden Verfahren — ML-KEM ist jung, X25519 fällt gegen Quantenrechner —, hält das andere.

## 4. Protokoll (Version 1)

Maßgeblich ist der Code in `internal/proto/proto.go`; die Firmware muss genau das nachbauen. Hier die Idee.

### 4.1 Schlüssel

Server und jedes Gerät haben einen **Langzeitschlüssel** aus zwei Teilen: X25519 (32 Byte öffentlich) und ML-KEM-768 (1184 Byte öffentlich), zusammen 1216 Byte. Der **Fingerabdruck** ist SHA-256 darüber, die **Geräte-ID** dessen erste 16 Byte. Das Gerät erzeugt seinen Schlüssel selbst; der private Teil verlässt es nie.

Es gibt **keine Signaturen**. Echtheit entsteht allein durch Schlüsselkapselung: Wer entkapseln kann, was an einen Langzeitschlüssel gekapselt wurde, besitzt ihn. Das Gerät braucht damit nur ein Post-Quanten-Verfahren (ML-KEM) statt zwei; ML-DSA-Signaturen wären mit 3,3 kB je Signatur und großem Stack-Bedarf der teurere Teil.

### 4.2 Handshake

Eine Anfrage, eine Antwort.

| Richtung | Inhalt | Größe |
|---|---|---|
| Gerät → Server `POST /v1/handshake` | Version, Art, Geräte-ID, flüchtiger X25519-Schlüssel, flüchtiger ML-KEM-Schlüssel, Kapsel an den Langzeitschlüssel des Servers | 2322 Byte |
| nur bei der Anmeldung zusätzlich | Langzeitschlüssel des Geräts, Token-ID | +1224 Byte |
| Server → Gerät | Version, Sitzungs-ID, flüchtiger X25519-Schlüssel, Kapsel an den flüchtigen Geräteschlüssel, Kapsel an den Langzeitschlüssel des Geräts, Bestätigung (HMAC) | 2257 Byte |

Daraus entstehen sechs gemeinsame Geheimnisse:

| | Post-Quanten (ML-KEM-768) | klassisch (X25519) |
|---|---|---|
| beweist den **Server** | Kapsel an den Server-Langzeitschlüssel | flüchtig (Gerät) × Langzeit (Server) |
| beweist das **Gerät** | Kapsel an den Geräte-Langzeitschlüssel | flüchtig (Server) × Langzeit (Gerät) |
| **Vorwärtsgeheimnis** | Kapsel an den flüchtigen Geräteschlüssel | flüchtig × flüchtig |

Alle sechs gehen durch HKDF-SHA-256, zusammen mit einem Hash über beide Langzeitschlüssel und jedes ausgetauschte Byte. Heraus kommen je ein Schlüssel pro Richtung, die Bestätigung des Servers und — bei der Anmeldung — der Bestätigungscode.

- Der **Server ist bewiesen**, wenn seine Bestätigung stimmt: Die kann nur berechnen, wer den gepinnten Langzeitschlüssel hat.
- Das **Gerät ist bewiesen**, sobald seine erste verschlüsselte Nachricht aufgeht. Bis dahin gilt die Sitzung nichts und verfällt nach zwei Minuten (nicht kürzer: an einem schwachen WLAN braucht das Gerät mehrere Anläufe).
- **Vorwärtsgeheimnis:** Wird später ein Langzeitschlüssel gestohlen, bleiben alte Mitschnitte zu.

### 4.3 Nachrichten

`POST /v1/msg`: Sitzungs-ID (16) ‖ Zähler (8) ‖ AES-256-GCM-Chiffrat. Antwort: Zähler ‖ Chiffrat. Der Zähler ist die Nonce und muss steigen (kein Wiedereinspielen); Richtung, Sitzungs-ID und Zähler sind mitauthentifiziert. AES-GCM, weil der ESP32 AES in Hardware rechnet. Höchstens 32 kB Klartext je Nachricht.

Der Klartext ist JSON (mit ArduinoJson am Gerät billig; später CBOR, falls die Größe stört):

```
Gerät → Server   {"t":"enroll","name":"Labor-1","info":{"fw":"1.8.0"}}
                 {"t":"poll","status":{...},"results":[{"id":7,"ok":true,"out":"pong"}]}
Server → Gerät   {"state":"pending|active","cmds":[{"id":7,"type":"ping","args":{}}],"poll_s":30}
```

Aufträge, die es gibt (die Liste steht zweimal: am Server in `commandArgs`, am Gerät in `execute()` — das Gerät hat das letzte Wort):

| Auftrag | Antwort | braucht Freigabe am Gerät |
|---|---|---|
| `ping` | `pong` | nein |
| `status` | Ports, Zähler, Netz, Akku — was auch die Weboberfläche des Geräts zeigt | nein |
| `settings` | Einstellungen ohne Passwörter und Schlüssel | nein |
| `configs` | Liste der gespeicherten Konfigurationen | nein |
| `play` `{"name":"…","port":1}` | spielt eine gespeicherte Konfiguration auf einen seriellen Port ab; die Antwort kommt, wenn sie durch ist | **ja** |
| `reboot` | startet das Gerät neu, nachdem die Antwort beim Server ist | **ja** |
| `console` `{"port":1,"minutes":15}` | fragt nach einer Fernkonsole (§4.5); die Antwort ist das Ja oder Nein vom Display | **am Display, jedes Mal** |

**Freigabe am Gerät:** Lesen darf der Server immer. Was etwas verändert, führt das Gerät nur aus, wenn in seiner eigenen Weboberfläche der Haken „Die Leitstelle darf Konfigurationen abspielen und das Gerät neu starten" gesetzt ist. Der Haken ist aus, lässt sich nur am Gerät setzen und gehört zu keinem Auftrag — ein übernommener Server kann ihn nicht einschalten.

**Aufträge verfallen:** nicht abgeholt nach 24 Stunden, abgeholt und unbeantwortet nach zwei Stunden. Ein Auftrag wird **nie ein zweites Mal** zugestellt; etwas doppelt abzuspielen kann schlimmer sein als gar nicht.

Eine Sitzung gilt eine Stunde, dann handelt das Gerät neue Schlüssel aus. Sitzungen liegen nur im Speicher des Servers; nach einem Neustart antwortet er mit 401 und das Gerät macht einen neuen Handshake. HTTP-Status für Geräte: 401 = neu aushandeln, 403 = nicht (mehr) willkommen, ohne Begründung. Das Gerät spricht schlichtes HTTP/1.0 — eine Anfrage, eine Antwort, Verbindung zu; der Server nennt immer `Content-Length`.

### 4.4 Registrierung mit zweiseitiger Prüfung

1. **Einladen.** Der Administrator erzeugt am Server ein **Einladungs-Token**: Server-Adresse, Fingerabdruck des Serverschlüssels, Token-ID und ein Geheimnis (32 Byte). Einmal verwendbar, läuft ab (Vorgabe 10 Minuten). Eine Textzeile `c2e1:…`, später auch als QR-Code.
2. **Token ins Gerät.** Über die Weboberfläche des Geräts (Hotspot, `192.168.4.1`) — also über einen Weg, den der Bediener in der Hand hat.
3. **Gerät prüft den Server.** Es holt den Serverschlüssel, vergleicht den Fingerabdruck mit dem Token und pinnt ihn. Im Handshake muss der Server diesen Schlüssel beweisen. Das Token-Geheimnis wird **nicht gesendet**, sondern in die Schlüsselableitung gemischt: Ein Server, der es nicht kennt, kann die Bestätigung nicht berechnen; ein Gerät, das es nicht kennt, kann keine gültige Nachricht schicken.
4. **Server merkt das Gerät vor** (`pending`). Es darf sich melden, bekommt aber keine Aufträge.
5. **Mensch bestätigt.** Beide Seiten leiten aus dem Handshake denselben **sechsstelligen Code** ab. Das Gerät zeigt ihn auf dem Display. Der Administrator **tippt ihn am Server ein** — der Server zeigt ihn nirgends an, damit niemand blind auf „Bestätigen" klickt. Stimmt er, ist das Gerät `active`. Nach fünf falschen Eingaben ist die Anmeldung verworfen.

Damit weiß das Gerät: richtiger Server (Fingerabdruck aus dem Token, im Handshake bewiesen). Und der Server: eingeladenes Gerät (Token-Geheimnis) **und** genau das Gerät, das der Administrator vor sich hat (Code vom Display, gebunden an den Geräteschlüssel).

**Sperren:** Ein gesperrtes Gerät wird sofort getrennt und kommt auch mit neuem Token nicht zurück (sein Schlüssel ist bekannt und gesperrt).

### 4.5 Fernkonsole

Der Administrator bedient einen seriellen Port des Geräts aus seinem Browser. Das ist die heikelste Funktion der Leitstelle, deshalb gilt der Haken aus §4.3 hier nicht: **Jede Sitzung braucht einen Tipp auf dem Touch-Display des Geräts.**

1. **Anfragen.** Der Auftrag `console` nennt Port (1–4) und Dauer (1–60 Minuten). Er verfällt schneller als andere Aufträge: nicht abgeholt nach zwei, unbeantwortet nach drei Minuten. Je Gerät gibt es höchstens eine angefragte oder offene Fernkonsole.
2. **Zustimmen.** Das Display zeigt „Die Leitstelle möchte … für … min bedienen. Erlauben?". Ohne Antwort binnen 60 s gilt die Anfrage als abgelehnt; nach einem Nein oder ohne Antwort weist das Gerät weitere Anfragen 60 s lang ab. Ein Gerät ohne brauchbares Display lehnt immer ab.
3. **Öffnen.** Erst nach dem Ja schickt das Gerät Konsolennachrichten. Der Server nimmt sie nur für einen `console`-Auftrag an, den das Gerät vor weniger als zwei Minuten mit Ja beantwortet hat und der noch nicht benutzt wurde.
4. **Beenden** kann jede Seite: der Administrator, wer am Gerät steht (Tipp auf die rote Titelzeile oder Knopf in der Weboberfläche des Geräts), die abgelaufene Zeit, das Sperren des Geräts. Hört der Server 45 s nichts vom Gerät oder das Gerät 30 s nichts vom Server, ist die Sitzung zu.

Die Nachrichten laufen im selben verschlüsselten Kanal wie alles andere (`POST /v1/msg`):

```
Gerät → Server   {"t":"con","id":5,"off":1200,"d":"<base64>","iack":17,"left_s":280,"end":""}
Server → Gerät   {"ioff":17,"d":"<base64>","brk":1,"end":""}
```

Beide Richtungen sind Byteströme mit Offsets: `off` ist die Stelle von `d` in der Ausgabe des Ports seit Beginn der Sitzung, `iack` sagt, wie viel Eingabe das Gerät übernommen hat, `ioff` ist die Stelle der mitgeschickten Eingabe. Eine Nachricht, die nach einer verlorenen Antwort wiederholt wird, bringt so nichts doppelt. `brk` zählt die verlangten BREAKs, `end` nennt den Grund für das Ende.

Das Gerät fragt weiterhin von sich aus — es gibt keinen Weg vom Server zum Gerät. Solange eine Konsole offen ist, fragt es ohne Pause und hält dafür die Verbindung offen (HTTP/1.1 mit keep-alive statt einer Verbindung je Anfrage; eine zweite TLS-Verbindung hätte keinen Platz im Speicher). Der Server hält eine leere Frage bis zu 200 ms fest, falls gerade etwas getippt wird. Der Browser holt die Ausgabe per Long-Polling ab einem Offset (bis 20 s) und schickt Eingaben der Reihe nach.

**Was der Server sieht und behält:** Die Zeichen laufen im Klartext durch den Server — er könnte mitlesen. Die Fernkonsole ist also **nicht** Ende-zu-Ende bis zum Browser, anders als in Phase 0 gedacht. Aufgezeichnet wird je Sitzung nur: Port, Beginn, Ende, Grund, Zahl der Zeichen in jede Richtung (die letzten 50 je Gerät). Die Ausgabe liegt bis zu 256 kB im Speicher, damit ein Browser nach einer Unterbrechung weiterlesen kann, und noch zehn Minuten nach dem Ende. Sitzungen überleben keinen Neustart des Servers; im Protokoll steht dann „Server neu gestartet".

**Am Gerät:** Solange die Konsole offen ist, steht auf jeder Seite des Displays rot „Fernkonsole" statt des Titels. Was der Administrator tippt, geht an den Port wie eine Eingabe im Web-Terminal; was der Port gerade nicht annimmt (Dateiübertragung, Auto-Baud), fällt weg. Die Ausgabe beginnt mit dem Ja — was vorher auf dem Schirm stand, bleibt am Gerät.

## 5. Server

- **Go, nur Standardbibliothek** (`crypto/mlkem` nach FIPS 203, `crypto/ecdh`, `crypto/hkdf`, AES-GCM). Keine fremden Pakete, ein statisches Programm, Container ohne Shell und ohne Root.
- **Admin-Schnittstelle** `/admin/v1/…`, JSON, geschützt durch ein Bearer-Token aus der Umgebung: Token erzeugen, Geräte auflisten, bestätigen, sperren, Auftrag einreihen, Aufträge und Ergebnisse lesen, Fernkonsole (Zustand, Ausgabe, Eingabe, Beenden). Auftragsarten und ihre Argumente stehen auf einer festen Liste; alles andere weist der Server ab, bevor es ein Gerät erreicht.
- **Weboberfläche** unter `/`: dieselbe Schnittstelle zum Anklicken — Geräteliste mit Zustand, Akku und WLAN-Pegel, Einladung mit QR-Code, Bestätigen mit Code, Aufträge, Verlauf mit Antworten, Fernkonsole mit Terminal (xterm.js), Sperren. Eine Seite ohne fremde Quellen (strenge Content-Security-Policy), in den Server eingebettet. xterm.js gestaltet sich mit `<style>`-Elementen und `style`-Attributen, die `style-src 'self'` nicht zulässt; `web/xterm-csp.js` leitet beides auf konstruierte Stylesheets und direkt gesetzte Stileigenschaften um, statt die Richtlinie aufzuweichen. Das braucht `adoptedStyleSheets` im Browser. Das Admin-Token wird beim Öffnen eingetippt und bleibt im Browser-Tab. Namen und Antworten von Geräten kommen nur als Text auf die Seite, nie als HTML.
- **QR-Code:** Er trägt die Adresse der Weboberfläche des Geräts im eigenen Hotspot samt Token (`http://192.168.4.1/#c2=…`). Ein Handy im Hotspot des Geräts scannt ihn, die Geräteseite öffnet sich mit eingetragenem Token; „Anmelden" tippt der Mensch selbst.
- **Zustand** in einer JSON-Datei im Datenverzeichnis (Serverschlüssel, Geräte, Tokens, Aufträge), atomar geschrieben, Rechte 0600. Reicht für wenige Geräte und einen Prozess. „Zuletzt gesehen" und der gemeldete Status werden nicht bei jeder Meldung geschrieben, sondern mit der nächsten echten Änderung oder nach einer Minute; nach einem Absturz kann diese Angabe also bis zu einer Minute alt sein.
- **TLS:** lokal selbstsigniert (wird beim ersten Start erzeugt), mit eigenen Zertifikatsdateien, oder aus (hinter einem Proxy, der TLS beendet).

## 6. Phasen

| Phase | Inhalt | Stand |
|---|---|---|
| **0 Konzept** | dieses Dokument | fertig |
| **1 Server-Kern** | Protokoll, Registrierung mit Code, Sitzungen, Auftragswarteschlange, Admin-API, Gerätesimulator, Docker, Tests | **fertig, lokal getestet** (§7) |
| **2 Firmware-Client** | ML-KEM-768 auf dem ESP32-S3 (mlkem-native), X25519/HMAC/AES-GCM aus mbedTLS 2; Geräteschlüssel im NVS; Token-Eingabe in der Weboberfläche; Code auf dem Touch-Display; Poll-Schleife; Aufträge `ping` und `status` | **fertig, am Gerät im lokalen Netz getestet** (§7); offen: Weboberfläche im Browser ansehen, QR-Code fürs Token |
| **3 Aufträge und Oberfläche** | Status und Telemetrie (Akku, WLAN, Ports), Einstellungen lesen, Konfigurationen auflisten und auf einen Port abspielen, Neustart; Freigabe am Gerät für alles, was verändert; Aufträge verfallen; Weboberfläche für den Administrator; QR-Code fürs Token | **fertig, im Browser und am Gerät getestet** (§7). Das Firmware-Update ist herausgenommen: Es gehört mit Signaturprüfung im Gerät zusammen (Zeile „später") |
| **4 Fernkonsole** | serielle Konsole über die Leitstelle, Ende-zu-Ende bis zum Browser des Administrators; nur nach Freigabe am Gerät (Touch), zeitlich begrenzt, mit Protokoll. Braucht einen schnelleren Rückkanal (Long-Polling oder WebSocket) | **fertig; Server und Admin-Weboberfläche mit simuliertem Gerät, Firmware am Board im lokalen Netz getestet** (§7); offen: die Richtung Port → Server am Board (nichts am Port angeschlossen). Abweichung: nicht Ende-zu-Ende, der Server leitet die Zeichen im Klartext durch (§4.5) |
| **5 Cloud-Betrieb** | Datenbank statt JSON-Datei; echtes TLS-Zertifikat (Proxy oder ACME); Begrenzung der Anfragerate; Sicherung des Datenverzeichnisses; Benutzerkonten mit zweitem Faktor statt eines Tokens; Wechsel des Serverschlüssels; Überwachung | offen |
| **später** | Firmware-Update über die Leitstelle, signiert (ML-DSA oder SLH-DSA) und im Gerät geprüft — ohne Signatur könnte ein übernommener Server beliebige Firmware einspielen; Flash-Verschlüsselung für den Geräteschlüssel | offen |

Reihenfolge mit Absicht: Phase 2 vor allem anderen am Server, weil erst das echte Gerät zeigt, ob Größen und Rechenzeiten passen. Phase 5 muss vor dem ersten Betrieb im Internet abgeschlossen sein — Phase 1 ist ein lokaler Prüfstand.

## 7. Was geprüft ist

### Phase 1: Server

Alles auf dem Raspberry Pi (arm64), Go 1.25, im Container.

- **Protokoll** (`internal/proto`, 9 Tests): Sitzung und Anmeldung hin und zurück; Code auf beiden Seiten gleich; falscher Server (Mann in der Mitte) wird am Gerät erkannt; falsches Token-Geheimnis ebenso; ein Fremder mit Geräte-ID und öffentlichem Schlüssel bringt keine Nachricht durch; Wiedereinspielen, verfälschte und zurückgespiegelte Nachrichten werden verworfen; jedes gekippte Byte der Handshake-Antwort fällt auf; verstümmelte Eingaben.
- **Server über HTTP** (`internal/server`, 11 Tests aus Phase 1 und 2): Anmelden, Bestätigen, Auftrag hin, Ergebnis zurück; vorgemerktes Gerät bekommt nichts; die Geräteliste verrät den Code nicht; falscher Code und Verwerfen nach fünf Versuchen; Token nur einmal und nur bis zum Ablauf; Token mit fremder Server-Adresse; gefälschtes Token-Geheimnis; Sperren trennt sofort und endgültig; unbekanntes Gerät; Sitzungsablauf und Server-Neustart; Admin-Token; Müll an den Geräte-Endpunkten; binäre Antworten nennen ihre Länge.
- **Ende zu Ende im Container** (`test/e2e.sh`): Image bauen (16 MB), Server mit leerem Datenverzeichnis starten, simuliertes Gerät in eigenem Container anmelden, Token ein zweites Mal abgelehnt, falscher und richtiger Code, Auftrag `ping` → `pong`, Neustart des Servers, Sperren.
- **Äußeres TLS:** `openssl s_client -groups X25519MLKEM768` gegen den Container handelt TLS 1.3 mit genau dieser hybriden Gruppe aus.

### Phase 2: Firmware

Die Geräteseite des Protokolls ist `firmware/src/c2_proto.cpp` (ohne Arduino, läuft auch auf dem PC), der Rest — Task, Speicher, HTTP, Aufträge — `firmware/src/c2.cpp`. ML-KEM kommt aus mlkem-native v1.0.0 (`firmware/lib/mlkem_native/`), alles andere aus dem mbedTLS 2.28 des Arduino-Cores.

- **Auf dem PC** (`tests/c2/run.sh`): derselbe Protokollcode, übersetzt gegen mbedTLS 2.28 (Debian bookworm im Container), meldet sich am echten Server-Container an, wird bestätigt, holt einen Auftrag ab und liefert das Ergebnis; ein Token, das einen anderen Serverschlüssel nennt, wird abgelehnt. Dazu Selbsttest mit bekannten Antworten: X25519 (RFC 7748), HKDF (RFC 5869), AES-256-GCM, ML-KEM-768 gegen einen von Gos `crypto/mlkem` errechneten Schlüssel.
- **Am Waveshare-Board** (9. Oktober 2026, Server im Container auf dem Raspberry Pi im selben WLAN, TLS mit selbstsigniertem Zertifikat): derselbe Selbsttest läuft bei jedem Start auf dem Gerät und besteht. Anmelden mit Token über `/api/c2/enroll`; Code im Log, in `/api/c2` und auf der Info-Seite des Displays (Screenshot); falscher Code am Server abgelehnt, richtiger angenommen; Aufträge `status` und `ping` ausgeführt, Ergebnisse am Server; Neustart des Geräts — verbindet sich von selbst wieder; Sperren am Server — Gerät meldet „abgewiesen"; Abmelden löscht Schlüssel und Server.
- **Gemessen am Gerät:** ein Handshake dauert 2,5–4,7 s, davon 1,1–1,5 s Rechnen, der Rest TLS-Aufbau und Netz. Woran die Rechenzeit im Einzelnen hängt (ML-KEM, die fünf X25519-Multiplikationen über mbedTLS, das Anlegen des Hilfstasks), ist nicht aufgeschlüsselt. ML-KEM braucht rund 19 kB Stack. Freier Heap im Betrieb mit Bluetooth 93 kB, Tiefststand während einer TLS-Verbindung 38 kB.
- **Nicht geprüft:** der Block „Leitstelle" der Weboberfläche im Browser (die Aufrufe dahinter per curl); die Meldung mit dem Code als Einblendung auf dem Display (nur die Info-Zeile per Screenshot); Betrieb über Stunden (Sitzungswechsel nach einer Stunde am Gerät); Akkubetrieb mit 80 MHz; ein Server außerhalb des lokalen Netzes.

### Phase 3: Aufträge und Weboberfläche

- **Server** (3 weitere Tests, zusammen 23): Auftragsarten und Argumente — zwölf Fälle, darunter unbekannte Art, fehlende oder überzählige Felder, Port außerhalb 1–4; Verfallen abgeholter und nicht abgeholter Aufträge, späte Antwort ändert nichts, nichts wird doppelt zugestellt; die Weboberfläche wird ausgeliefert, mit Content-Security-Policy, ohne Pfadausbruch, und die Daten dahinter verlangen das Token.
- **Weboberfläche im Browser** (`test/ui.sh`, Chromium ohne Bildschirm über Playwright, gegen den Container mit simuliertem Gerät): falsches Admin-Token zeigt nichts; Einladung mit Token, Ablaufzeit und QR-Code; vorgemerktes Gerät, falscher und richtiger Code, der Code steht nirgends auf der Seite; Ping, Konfigurationen (deren Namen dann zum Abspielen angeboten werden), Abspielen auf Port 2, Neustart, jeweils mit Antwort im Verlauf; ein Gerät, das sich `<img src=x onerror=…>` nennt, erscheint als Text und führt nichts aus; Sperren; keine JavaScript-Fehler. Der QR-Code wurde einmal mit OpenCV zurückgelesen: Er enthält die Geräteadresse mit dem Token.
- **Am Waveshare-Board** (9. Oktober 2026): `status` (966 Zeichen, keine Felder, die nach Geheimnis aussehen), `settings` (ohne Hotspot- und WLAN-Passwort, nur „gesetzt ja/nein"), `configs`; `play` und `reboot` ohne Freigabe abgelehnt; nach Freigabe in der Weboberfläche des Geräts: „Demo" auf Port 1 abgespielt, Antwort „fertig, 3 Zeilen" nach dem Durchlauf, ein zweiter Auftrag währenddessen abgelehnt; `reboot` — Antwort am Server, dann Neustart, Gerät verbindet sich wieder.
- **Weboberfläche des Geräts im Browser** (Chromium über das LAN): Der Link aus dem QR-Code öffnet den Reiter Netz mit eingetragenem Token; Zustand, Server, Geräte-ID und der Haken stimmen; Haken entfernen schaltet die Freigabe am Gerät ab.
- **Nicht geprüft:** das Scannen des QR-Codes mit einem echten Handy im Hotspot des Geräts; die Weboberfläche auf einem Handy; `play` mit einem angeschlossenen Gerät an der seriellen Leitung (ohne Gegenstelle wartet jede Zeile zwei Sekunden); Verfallen von Aufträgen am echten Gerät.
- **Aufgefallen:** `test/ui.sh` schlug zweimal von sieben Läufen fehl, beide Male, als auf dem Raspberry Pi gleichzeitig anderes lief (ein Auftrag blieb 20 s auf „wartet"). Die Ursache ist nicht gefunden und der Fehler ließ sich nicht gezielt hervorrufen. Vermutet wird, dass der Server bei jeder Gerätemeldung seine Zustandsdatei schrieb und dabei alle anderen Aufrufe warten ließ; das ist geändert (§5), zwei Läufe danach waren in Ordnung — das belegt die Vermutung nicht. Im Geräte-Browsertest wartete Playwright beim normalen Klick auf den Haken vergeblich auf ein „ruhiges" Element; mit erzwungenem Klick ging es. Auch das ist nicht geklärt.

### Phase 4: Fernkonsole

Stand 11. Oktober 2026.

- **Server** (4 weitere Tests in `console_test.go`, zusammen 27): Sitzung hin und zurück samt Beenden durch den Administrator; ohne das Ja des Geräts öffnet sich nichts — nicht ohne Anfrage, nicht mit einem Auftrag anderer Art, nicht nach einem Nein und nicht mit einem zu alten Ja; eine nicht abgeholte Anfrage verfällt nach zwei Minuten; falsche Argumente erreichen kein Gerät; Ende vom Gerät aus, nach Funkstille und beim Sperren; das Warten auf beiden Seiten (gehaltene Frage des Geräts, Long-Polling des Browsers); Überlauf des Ausgabepuffers.
- **Admin-Weboberfläche im Browser** (`test/ui.sh`, Chromium über Playwright, gegen den Simulator mit `-console`, der einen Switch spielt): Konsole anfragen, „Offen auf Port 1", Prompt erscheint, `show` tippen und 40 Zeilen Antwort lesen, BREAK, Restzeit, „Beenden" mit Grund im Terminal und im Sitzungsprotokoll; keine JavaScript-Fehler und keine Verstöße gegen die Content-Security-Policy.
- **Am Waveshare-Board** (11. Oktober 2026, Firmware neu gebaut und geflasht, Server im Container auf dem Raspberry Pi im selben WLAN, TLS selbstsigniert, `ping -s 1400` ohne Verlust; bedient über die Admin-API mit curl, das Display über `lcd_debug.py` mit Screenshots und simulierten Taps): Anfrage erscheint als Dialog „Erlauben?" auf dem Display. „Ablehnen" → am Server „am Geraet abgelehnt", eine zweite Anfrage gleich danach „am Geraet gerade abgelehnt". Keine Antwort → nach gut einer Minute „am Geraet hat niemand geantwortet". „Erlauben" → Sitzung am Server offen, rote Titelzeile „Fernkonsole" auf dem Display, Zustand und Restzeit in `/api/c2`. Eingabe: 13 Zeichen und danach 3000 Zeichen in sechs Stücken kamen vollständig am Port an (Sendezähler des Ports, `bytes_in` im Sitzungsprotokoll). Beenden vom Server, am Display (Tipp auf die Titelzeile, „Beenden"), über `/api/c2/console/end` und durch Ablauf der Zeit (1 min), jeweils mit dem passenden Grund im Protokoll und der Einblendung „Fernkonsole beendet" am Gerät. Sieben Sitzungen und Anfragen hintereinander ohne Neustart; freier Heap während einer Sitzung 47 kB, Tiefststand 34 kB, Stack des Netz-Tasks mindestens 6,2 kB frei.
- **Nicht geprüft:** die Richtung Port → Server am Board — am seriellen Port hängt nichts (kein MAX3232), es kam kein Zeichen zurück, `bytes_out` blieb 0. Ob ein BREAK auf der Leitung ankommt (der Auftrag wurde angenommen, gemessen hat es niemand). Die Admin-Weboberfläche im Browser gegen das echte Gerät (nur gegen den Simulator) und in Firefox oder Safari; der Block „Fernkonsole" in der Weboberfläche des Geräts im Browser (die Aufrufe dahinter per curl). Eine Sitzung über viele Minuten, über den Sitzungswechsel nach einer Stunde, bei schwachem WLAN oder im Akkubetrieb. `tests/c2/run.sh` übersetzt `c2_console.cpp` nicht mit — der Kommentar in `c2_console.h`, der das behauptet, eilt voraus.
- **Aufgefallen:** Mit `style-src 'self'` blieb das Terminal ohne Gestaltung (Text kaum lesbar, Zeilen liefen aus dem Kasten), der Browser meldete je Stil einen Verstoß; behoben mit `xterm-csp.js` (§5). Der Kasten des Terminals zählte seinen Innenabstand zur Höhe, die letzte Zeile ragte heraus (`box-sizing: content-box`). Und im Browsertest verfehlte der Klick auf „Beenden" seinen Knopf: Ein Bild der ganzen Seite (`full_page`) bei Fokus im Terminal ließ die Seite an anderer Stelle stehen, als der Browser annahm. Das war der Test, nicht die Seite; das Bild zeigt jetzt nur den sichtbaren Ausschnitt.

Was die Arbeit am Gerät gezeigt hat:

- **TLS und ML-KEM passen nicht gleichzeitig in den Speicher.** Eine TLS-Verbindung kostet rund 45 kB Heap, der Stack für ML-KEM 28 kB. Der Schlüsseltausch wird deshalb gerechnet, bevor die Anfrage hinausgeht und nachdem die Antwort da ist — in einem kurzlebigen Task mit großem Stack, dessen Speicher sofort wieder frei ist (`big()` in `c2.cpp`).
- **Der Server muss `Content-Length` nennen.** Gos `net/http` schickt Antworten über 2 kB sonst in Stücken (chunked); die Handshake-Antwort hat 2257 Byte. Simulator und PC-Test hatten das nicht bemerkt, das Gerät schon.
- **Schwaches WLAN.** Am Testplatz (−77 bis −83 dBm, Sendeleistung 8,5 dBm) kamen Pakete über etwa 1100 Byte zeitweise gar nicht durch — das betrifft auch die Weboberfläche des Geräts im LAN, nicht nur die Leitstelle. Mit 19,5 dBm ging es meist. Der Client wiederholt eine gescheiterte Anfrage deshalb bis zu dreimal und wartet bis zu 20 s; mehr kann er gegen eine schlechte Funkstrecke nicht tun. Möglich wäre, die MTU des WLAN-Clients bei schwachem Signal zu senken.

## 8. Grenzen und offene Fragen

- **Eigenes Protokoll.** Der Aufbau folgt bekannten Mustern (KEM-basierte Authentifizierung wie bei KEMTLS, hybride Ableitung wie bei X25519MLKEM768), ist aber selbst zusammengesetzt und **von niemandem begutachtet**. Vor einem Betrieb im Internet sollte jemand mit Kryptografie-Erfahrung darauf sehen. Die Bausteine selbst kommen aus Gos Standardbibliothek.
- **Rechenzeit am Gerät:** gut eine Sekunde je Handshake (§7), einmal pro Stunde. Der Hauptschleife und damit der seriellen Brücke nimmt das nichts, der Client hat einen eigenen Task auf dem anderen Kern. Ob sich das im Akkubetrieb bei 80 MHz bemerkbar macht, ist offen.
- **Der Geräteschlüssel liegt unverschlüsselt im Flash**, wie heute schon die 802.1X-Schlüssel. Wer das Gerät hat, hat den Schlüssel; dagegen hilft nur Sperren am Server.
- **Sechs Ziffern** lassen einem Angreifer, der ein Token abgefangen hat, eine Chance von eins zu einer Million je Versuch, bei fünf Versuchen. Mehr Ziffern sind möglich, aber lästiger abzulesen.
- **Keine Begrenzung der Anfragerate.** Ein Handshake kostet den Server Rechenzeit; ohne Token oder bekannte Geräte-ID wird er zwar vor der Kryptografie abgewiesen, aber eine bekannte Geräte-ID genügt, um ihn auszulösen. Gehört in Phase 5.
- **Ein Admin-Token für alles**, keine Benutzer, kein Protokoll der Admin-Aktionen; die Weboberfläche hält das Token im Browser-Tab (`sessionStorage`). Phase 5.
- **Aufträge** werden einmal zugestellt und verfallen (§4.3). Startet das Gerät neu, bevor es das Ergebnis meldet, erfährt der Server nicht, ob der Auftrag ausgeführt wurde — bei `play` muss man dann am Gerät nachsehen.
- **Die Freigabe am Gerät ist ein einziger Haken** für `play` und `reboot` zusammen, und sie gilt, bis jemand sie wieder entfernt. Die Fernkonsole hängt nicht an ihm: Sie braucht je Sitzung einen Tipp am Touch-Display und läuft ab (§4.5).
- **Die Fernkonsole ist nicht Ende-zu-Ende.** Der Server leitet die Zeichen im Klartext durch; wer den Server hat, liest eine offene Sitzung mit und kann in ihr tippen. Öffnen kann er keine — das entscheidet der Mensch am Display. Was getippt und gezeigt wurde, wird nicht aufgezeichnet; damit lässt sich nachträglich auch nicht belegen, was ein Administrator in einer Sitzung getan hat.
- **`play` kann der Server nur starten, nicht anhalten** und spielt nur, was am Gerät schon gespeichert ist; hochladen kann er nichts. Konfigurationen mit `{{Platzhaltern}}` gehen nicht (wie am Touch-Display).
- **TLS am Gerät prüft das Zertifikat nicht** (`setInsecure()`): Der innere Kanal beweist den Server ohnehin, und nur so geht ein selbstsignierter Testserver. Ob das Gerät im Betrieb zusätzlich gegen eine CA prüft, ist zu entscheiden; es wäre die sauberere Wahl, kostet aber ein CA-Bündel im Flash. Ohne Prüfung sieht ein Mann in der Mitte, **dass** ein Gerät mit der Leitstelle spricht, und kann stören — mitlesen oder sich als Server ausgeben kann er nicht.
