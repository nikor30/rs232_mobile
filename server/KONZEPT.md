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

- **Go, fast nur Standardbibliothek** (`crypto/mlkem` nach FIPS 203, `crypto/ecdh`, `crypto/hkdf`, AES-GCM). Das einzige fremde Paket ist der PostgreSQL-Treiber `pgx` (mit dem, was er selbst mitbringt); alles Kryptografische und alles am Netz bleibt Standardbibliothek. Ein statisches Programm, Container ohne Shell und ohne Root.
- **Admin-Schnittstelle** `/admin/v1/…`, JSON, geschützt durch die Anmeldung aus §5.1: Token erzeugen, Geräte auflisten, bestätigen, sperren, Auftrag einreihen, Aufträge und Ergebnisse lesen, Fernkonsole (Zustand, Ausgabe, Eingabe, Beenden). Auftragsarten und ihre Argumente stehen auf einer festen Liste; alles andere weist der Server ab, bevor es ein Gerät erreicht.
- **Weboberfläche** unter `/`: dieselbe Schnittstelle zum Anklicken — Geräteliste mit Zustand, Akku und WLAN-Pegel, Einladung mit QR-Code, Bestätigen mit Code, Aufträge, Verlauf mit Antworten, Fernkonsole mit Terminal (xterm.js), Sperren. Eine Seite ohne fremde Quellen (strenge Content-Security-Policy), in den Server eingebettet. xterm.js gestaltet sich mit `<style>`-Elementen und `style`-Attributen, die `style-src 'self'` nicht zulässt; `web/xterm-csp.js` leitet beides auf konstruierte Stylesheets und direkt gesetzte Stileigenschaften um, statt die Richtlinie aufzuweichen. Das braucht `adoptedStyleSheets` im Browser. Angemeldet wird mit einem Konto (§5.1); das Admin-Token, wenn es eins gibt, wird eingetippt und bleibt im Browser-Tab. Namen und Antworten von Geräten und die Namen von Konten kommen nur als Text auf die Seite, nie als HTML.
- **QR-Code:** Er trägt die Adresse der Weboberfläche des Geräts im eigenen Hotspot samt Token (`http://192.168.4.1/#c2=…`). Ein Handy im Hotspot des Geräts scannt ihn, die Geräteseite öffnet sich mit eingetragenem Token; „Anmelden" tippt der Mensch selbst.
- **Zustand:** Der Server hält alles im Speicher (Serverschlüssel, Geräte, Tokens, Aufträge, Konten, Protokoll), entscheidet dort unter einer Sperre und schreibt jede Änderung durch — wahlweise in eine **JSON-Datei** im Datenverzeichnis (atomar, Rechte 0600; für den lokalen Betrieb und die Tests) oder nach **PostgreSQL** (`C2_DATABASE_URL`). In der Datenbank liegt je Sache eine Tabelle (`server`, `tokens`, `devices`, `commands`, `consoles`, `users`, `audit`); geschrieben werden nur die geänderten Zeilen, je Änderung eine Transaktion. Gelesen wird die Datenbank einmal, beim Start. Daraus folgt: **ein Serverprozess je Datenbank** — ein zweiter bekäme die Änderungen des ersten nicht mit; eine Sperre in der Datenbank (advisory lock) lässt ihn deshalb gar nicht erst starten. Das Schema ist eine Liste von Migrationen im Code, die Datenbank merkt sich ihren Stand. Was Geräte melden (Status, Antworten), bleibt als `json` so liegen, wie es kam; Text, den PostgreSQL nicht nimmt (Nullbytes, kaputtes UTF-8 — Namen und Gründe kommen von Geräten und aus dem Anmeldeformular), wird vor dem Schreiben bereinigt, damit eine einzelne Eingabe nicht jedes weitere Speichern scheitern lässt. Beim ersten Start mit leerer Datenbank übernimmt der Server eine vorhandene `state.json`. Das Protokoll behält in der Datenbank 200 000 Einträge, der Server arbeitet mit den neuesten 5000. „Zuletzt gesehen" und der gemeldete Status werden nicht bei jeder Meldung geschrieben, sondern mit der nächsten echten Änderung oder nach einer Minute; nach einem Absturz kann diese Angabe also bis zu einer Minute alt sein.
- **Sicherung** (nur mit PostgreSQL, `backup.sh`): Ein eigener Container am Netz der Datenbank schreibt alle 24 Stunden ein `pg_dump`-Archiv der ganzen Datenbank — ein Schnappschuss, der Server läuft weiter — in ein eigenes Volume, prüft, dass es sich lesen lässt und die Tabelle mit dem Serverschlüssel enthält, und behält die neuesten 14. Zurückgespielt wird in einer Transaktion und nur bei gestopptem Server: Der hält den Zustand im Speicher und schriebe ihn sonst darüber; `restore` erkennt den laufenden Server an dessen Sperre in der Datenbank und weigert sich. Die Archive sind unverschlüsselt und bleiben auf dem Rechner; sie wegzubringen ist Sache des Betreibers (§8).
- **Überwachung:** `/healthz` sagt jedem, der fragt, ob der Server seinen Zustand schreiben kann (200 oder 503, ohne Begründung); gefragt wird der Speicherort dafür höchstens alle 10 Sekunden. Der Server prüft zweimal in der Minute auch selbst und schreibt ins Log, wenn sich das ändert — sonst fiele eine verschwundene Datenbank erst auf, wenn sich etwas ändern soll. Bei dieser Prüfung wird nachgeschrieben, was liegen geblieben ist, und die Sperre in der Datenbank neu genommen, falls die Verbindung, an der sie hing, verloren ging; hat sie inzwischen ein anderer Prozess, schreibt dieser nicht mehr und beendet sich. `GET /admin/v1/status` nennt den Grund eines Fehlers, zählt Geräte je Zustand, stille Geräte, offene Fernkonsolen und abgewiesene Anfragen. Die Container melden ihren Zustand an Docker (das Image hat keine Shell: `c2server -health`). Benachrichtigt wird niemand — das ist Sache eines Wächters außerhalb (§8).
- **TLS:** lokal selbstsigniert (wird beim ersten Start erzeugt), mit eigenen Zertifikatsdateien, oder aus — hinter einem Proxy, der TLS beendet. Für den Betrieb ist das **Caddy** (`docker-compose.cloud.yml`, `Caddyfile`): Er holt und erneuert das Zertifikat (Let's Encrypt), leitet http auf https um, setzt HSTS und reicht alles an den Server weiter, der selbst keinen Port nach außen hat. Caddy komprimiert nicht — die Firmware braucht Antworten mit `Content-Length` (§7). Die Absenderadresse für Anfragerate und Protokoll nimmt der Server dann aus `X-Forwarded-For` (`C2_TRUST_PROXY=1`); Caddy überschreibt, was ein Client dort selbst hineinschreibt. Die Datenbank hängt an einem Netz ohne Weg nach außen. Weil die Sicherheit der Geräte am inneren Kanal hängt (§3), sieht Caddy von ihnen nur verschlüsselte Nachrichten; von der Admin-Schnittstelle sieht er alles, wie der Server selbst.

### 5.1 Wer Administrator ist

Drei Wege hinein, alle zum selben Recht — es gibt nur Administratoren, keine Rollen:

- **Konto mit Passwort und Einmalcode.** Ein Administrator legt das Konto an und bekommt einen **Einrichtungscode** (80 Bit, gilt einen Tag, wird einmal gezeigt, gespeichert ist nur sein Hash). Der Besitzer wählt damit sein Passwort (mindestens 12 Zeichen) und verbindet eine Authenticator-App (TOTP nach RFC 6238, per QR-Code); erst wenn er einen Code aus der App eingetippt hat, gilt das Konto, und der Einrichtungscode ist verbraucht. Anmelden heißt Name, Passwort und Code in einem Schritt, mit einer einzigen Fehlermeldung, gleich ob Name, Passwort oder Code falsch war. Passwörter liegen als PBKDF2-HMAC-SHA256 mit 600 000 Runden und eigenem Salt in der Zustandsdatei, das TOTP-Geheimnis im Klartext (der Server muss damit rechnen). Ein Code gilt einmal; angenommen wird der aktuelle Zeitschritt und je einer davor und danach. Nach fünf falschen Versuchen ist der Name 15 Minuten gesperrt. „Zurücksetzen“ nimmt Passwort und App weg und gibt einen neuen Einrichtungscode aus — für das verlorene Handy.
- **Microsoft Entra ID** (wenn eingerichtet): OpenID Connect, Authorization-Code-Flow mit PKCE. Der Server prüft das ID-Token selbst — Signatur (nur RS256) gegen die Schlüssel des Mandanten, Aussteller, Mandant, Anwendung, Gültigkeit, Nonce — und nimmt Antworten nur in dem Browser an, der die Anmeldung begonnen hat. Entra sagt, **wer** jemand ist; ob er hier Administrator ist, entscheidet dieser Server: Es muss ein Konto der Art „Entra“ für ihn geben, angelegt mit seinem Anmeldenamen oder seiner Objekt-ID. Bei der ersten Anmeldung wird ein mit Namen angelegtes Konto an die Objekt-ID gebunden, danach zählt nur sie (Namen lassen sich ändern und neu vergeben). Den zweiten Faktor verlangt Entra (Conditional Access); das v2.0-ID-Token sagt nicht, wie sich jemand angemeldet hat, der Server kann es also nicht nachprüfen.
- **Das Admin-Token** aus der Umgebung, wenn eins gesetzt ist: für das erste Konto, für Skripte und als Zugang, wenn sonst nichts geht. Ist es leer, kommen nur Konten herein; ohne Token und ohne Konto startet der Server nicht.

**Sitzungen:** Ein angemeldeter Browser hält ein zufälliges Cookie (`HttpOnly`, `Secure`, `SameSite=Strict`, mit `__Host-`-Präfix), der Server dessen Hash im Speicher. Eine Sitzung endet nach 30 Minuten ohne Aufruf, spätestens nach 12 Stunden, beim Abmelden, beim Sperren oder Zurücksetzen des Kontos und bei jedem Neustart des Servers. Verändernde Aufrufe mit Cookie brauchen zusätzlich den Kopf `X-C2`, den eine fremde Seite nicht mitschicken kann.

**Protokoll:** Jede Anmeldung, jeder Versuch und jeder verändernde Aufruf der Admin-Schnittstelle wird festgehalten — wann, wer (Kontoname, `token` oder `-`), von welcher Adresse, was, an welchem Gerät oder Konto, ob es angenommen wurde. Die letzten 5000 Einträge, in der Zustandsdatei und als Logzeile. Nicht festgehalten wird Lesen und, was in einer Fernkonsole getippt wird (die Sitzung selbst schon, §4.5).

**Begrenzung der Anfragerate** je Absenderadresse: 30 Handshakes auf einmal, dann einer je Sekunde; 10 Anmeldeversuche, dann einer alle 6 Sekunden; 30 Aufrufe der Admin-Schnittstelle ohne gültige Anmeldung, dann einer alle 2 Sekunden. Darüber antwortet der Server mit 429 und `Retry-After`. Hinter einem Proxy sieht der Server nur dessen Adresse; mit `C2_TRUST_PROXY=1` nimmt er den letzten Eintrag aus `X-Forwarded-For` — nur setzen, wenn wirklich ein Proxy davorsteht, der diesen Kopf schreibt.

## 6. Phasen

| Phase | Inhalt | Stand |
|---|---|---|
| **0 Konzept** | dieses Dokument | fertig |
| **1 Server-Kern** | Protokoll, Registrierung mit Code, Sitzungen, Auftragswarteschlange, Admin-API, Gerätesimulator, Docker, Tests | **fertig, lokal getestet** (§7) |
| **2 Firmware-Client** | ML-KEM-768 auf dem ESP32-S3 (mlkem-native), X25519/HMAC/AES-GCM aus mbedTLS 2; Geräteschlüssel im NVS; Token-Eingabe in der Weboberfläche; Code auf dem Touch-Display; Poll-Schleife; Aufträge `ping` und `status` | **fertig, am Gerät im lokalen Netz getestet** (§7); offen: Weboberfläche im Browser ansehen, QR-Code fürs Token |
| **3 Aufträge und Oberfläche** | Status und Telemetrie (Akku, WLAN, Ports), Einstellungen lesen, Konfigurationen auflisten und auf einen Port abspielen, Neustart; Freigabe am Gerät für alles, was verändert; Aufträge verfallen; Weboberfläche für den Administrator; QR-Code fürs Token | **fertig, im Browser und am Gerät getestet** (§7). Das Firmware-Update ist herausgenommen: Es gehört mit Signaturprüfung im Gerät zusammen (Zeile „später") |
| **4 Fernkonsole** | serielle Konsole über die Leitstelle, Ende-zu-Ende bis zum Browser des Administrators; nur nach Freigabe am Gerät (Touch), zeitlich begrenzt, mit Protokoll. Braucht einen schnelleren Rückkanal (Long-Polling oder WebSocket) | **fertig; Server und Admin-Weboberfläche mit simuliertem Gerät, Firmware am Board im lokalen Netz getestet** (§7); offen: die Richtung Port → Server am Board (nichts am Port angeschlossen). Abweichung: nicht Ende-zu-Ende, der Server leitet die Zeichen im Klartext durch (§4.5) |
| **5 Cloud-Betrieb** | Datenbank statt JSON-Datei; echtes TLS-Zertifikat (Proxy oder ACME); Begrenzung der Anfragerate; Sicherung des Datenverzeichnisses; Benutzerkonten mit zweitem Faktor statt eines Tokens; Wechsel des Serverschlüssels; Überwachung | **begonnen:** Konten mit Passwort und TOTP, Anmeldung über Microsoft Entra ID, Protokoll der Admin-Aktionen und Begrenzung der Anfragerate sind gebaut und getestet (§5.1, §7). PostgreSQL statt der JSON-Datei und Caddy davor sind gebaut und auf dem Raspberry Pi getestet, mit Caddys eigener Zertifizierungsstelle statt eines echten Zertifikats (§5, §7). Die Sicherung der Datenbank und die Überwachung sind gebaut und getestet (§5, §7). **Offen:** Wechsel des Serverschlüssels; ein Lauf mit echtem Zertifikat |
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
- **Auf dem PC** (`tests/c2/run.sh`): der Strom-Code der Firmware (`c2_console.cpp`) spielt gegen den echten Server einen Port — 2000 Zeichen Begrüßung, Echo, „[BREAK]“. Jede dritte Antwort des Servers wird verworfen und dieselbe Nachricht noch einmal geschickt, wie es das Gerät nach einer verlorenen Antwort tut; am Server kommt die Ausgabe trotzdem Zeichen für Zeichen einmal an, am „Port“ die Eingabe und das BREAK ebenso, und das Sitzungsprotokoll zählt 5 Zeichen hin und 2012 zurück. Das prüft `c2_console.cpp`, nicht den Teil in `c2.cpp`, der es mit dem seriellen Port und der Verbindung verknüpft.
- **Am Waveshare-Board** (11. Oktober 2026, Firmware neu gebaut und geflasht, Server im Container auf dem Raspberry Pi im selben WLAN, TLS selbstsigniert, `ping -s 1400` ohne Verlust; bedient über die Admin-API mit curl, das Display über `lcd_debug.py` mit Screenshots und simulierten Taps): Anfrage erscheint als Dialog „Erlauben?" auf dem Display. „Ablehnen" → am Server „am Geraet abgelehnt", eine zweite Anfrage gleich danach „am Geraet gerade abgelehnt". Keine Antwort → nach gut einer Minute „am Geraet hat niemand geantwortet". „Erlauben" → Sitzung am Server offen, rote Titelzeile „Fernkonsole" auf dem Display, Zustand und Restzeit in `/api/c2`. Eingabe: 13 Zeichen und danach 3000 Zeichen in sechs Stücken kamen vollständig am Port an (Sendezähler des Ports, `bytes_in` im Sitzungsprotokoll). Beenden vom Server, am Display (Tipp auf die Titelzeile, „Beenden"), über `/api/c2/console/end` und durch Ablauf der Zeit (1 min), jeweils mit dem passenden Grund im Protokoll und der Einblendung „Fernkonsole beendet" am Gerät. Sieben Sitzungen und Anfragen hintereinander ohne Neustart; freier Heap während einer Sitzung 47 kB, Tiefststand 34 kB, Stack des Netz-Tasks mindestens 6,2 kB frei.
- **Admin-Weboberfläche gegen das Board** (11. Oktober 2026, Chromium über Playwright; ein Helfer auf dem Raspberry Pi meldet das Board an und tippt über `lcd_debug.py` auf dessen Display): einladen, mit dem Code vom Gerät bestätigen, `ping` und `status` mit Antwort im Verlauf; Fernkonsole anfragen — „Angefragt … am Gerät muss jetzt jemand tippen“ —, nach dem Tipp „Offen auf Port 1“, Terminal, 13 getippte Zeichen am Port angekommen, BREAK, Restzeit, „Beenden“; eine zweite Sitzung am Display beendet und auf der Seite als „am Geraet beendet“ gezeigt; eine dritte am Display abgelehnt. Keine JavaScript-Fehler, keine Verstöße gegen die Content-Security-Policy. **Einer von zwei Läufen scheiterte:** Das Board holte den `ping` ab und meldete sich dann mindestens 49 s nicht mehr (Takt 5 s), die Antwort kam in 30 s nicht. Die Funkstrecke war gleich danach in Ordnung (−66 dBm, 20 Pings mit 1400 Byte ohne Verlust); die Ursache ist nicht gefunden, der zweite Lauf ging ohne Änderung durch.
- **Nicht geprüft:** die Richtung Port → Server am Board — am seriellen Port hängt nichts (kein MAX3232), es kam kein Zeichen zurück, `bytes_out` blieb 0. Ob ein BREAK auf der Leitung ankommt (der Auftrag wurde angenommen, gemessen hat es niemand). Die Admin-Weboberfläche in Firefox oder Safari; der Block „Fernkonsole" in der Weboberfläche des Geräts im Browser (die Aufrufe dahinter per curl). Eine Sitzung über viele Minuten, über den Sitzungswechsel nach einer Stunde, bei schwachem WLAN oder im Akkubetrieb.
- **Aufgefallen:** Mit `style-src 'self'` blieb das Terminal ohne Gestaltung (Text kaum lesbar, Zeilen liefen aus dem Kasten), der Browser meldete je Stil einen Verstoß; behoben mit `xterm-csp.js` (§5). Der Kasten des Terminals zählte seinen Innenabstand zur Höhe, die letzte Zeile ragte heraus (`box-sizing: content-box`). Und im Browsertest verfehlte der Klick auf „Beenden" seinen Knopf: Ein Bild der ganzen Seite (`full_page`) bei Fokus im Terminal ließ die Seite an anderer Stelle stehen, als der Browser annahm. Das war der Test, nicht die Seite; das Bild zeigt jetzt nur den sichtbaren Ausschnitt.

### Phase 5, erster Teil: Konten, Protokoll, Anfragerate

Stand 11. Oktober 2026.

- **Server** (6 weitere Tests, zusammen 33): TOTP gegen die Werte aus RFC 6238. Konto anlegen, einrichten, anmelden — falscher Einrichtungscode, zu kurzes Passwort, Anmeldung vor dem Bestätigen der App, falscher Code, Einrichtungscode ein zweites Mal; ein Code gilt einmal, der Nachbarschritt wird angenommen, ein alter nicht; verändernde Aufrufe ohne `X-C2` abgewiesen; Abmelden beendet genau diese Sitzung; Ende nach 30 Minuten Ruhe und nach 12 Stunden; Sperre nach fünf Fehlversuchen; Sperren und Zurücksetzen eines Kontos beenden dessen Sitzungen; die Benutzerliste zeigt keine Geheimnisse; niemand löscht sich selbst. Protokoll: Einträge für Anlegen, Einrichten, Anmeldung, Fehlversuch, Auftrag angenommen und abgelehnt, nichts fürs Lesen. Ohne konfiguriertes Token kommt kein Token herein, auch kein leeres. Anfragerate für Anmeldung, Handshake und falsche Tokens; Absenderadresse hinter einem Proxy nur mit `C2_TRUST_PROXY`.
- **Entra, gegen einen nachgebauten Identitätsanbieter im Test** (ein HTTP-Server, der ID-Tokens mit eigenem RSA-Schlüssel ausstellt): Anmeldung ohne Konto abgelehnt, mit Konto angenommen, Bindung an die Objekt-ID (derselbe Mensch mit neuem Namen kommt herein, ein anderer mit dem alten Namen nicht), PKCE stimmt. Abgelehnt werden: andere Anwendung, anderer Mandant, anderer Aussteller, abgelaufen, noch nicht gültig, ohne Ablauf, falsche Nonce, ohne Objekt-ID, von einem fremden Schlüssel signiert, unbekannter Schlüssel, unsigniert (`alg: none`), anderer Algorithmus im Kopf, nach dem Signieren geänderte Angaben, Unsinn; außerdem die Antwort in einem anderen Browser, dieselbe Antwort ein zweites Mal und ein gesperrtes Konto.
- **Im Browser** (`test/ui.sh`, neue Stufe `accounts`): Konto anlegen, Einrichtungscode einmal gezeigt; als Besitzer einrichten mit falschem Code, ungleichen Passwörtern, falschem App-Code und dann richtig (der Test rechnet den TOTP-Code selbst); Benutzerliste und Protokoll; die Sitzung übersteht ein Neuladen, das Cookie ist `HttpOnly`, `Secure`, `SameSite=Strict` mit `__Host-`-Präfix, im Tab bleibt nichts gespeichert; nach dem Abmelden ist man auch nach Neuladen draußen; Anmelden mit falschem und richtigem Passwort; das eigene Konto lässt sich nicht löschen. Die bisherigen Stufen laufen weiter über das Admin-Token.
- **Weiter in Ordnung:** `test/e2e.sh` und `tests/c2/run.sh` (beide über das Admin-Token).
- **Nicht geprüft:** die Anmeldung gegen ein echtes Entra — es gab keinen Mandanten zum Testen; geprüft ist nur gegen den Nachbau, also auch nicht, ob die Angaben im ID-Token (`preferred_username`, `oid`, `tid`) in jeder Mandanten-Einstellung so kommen wie angenommen. Der Knopf „Mit Microsoft anmelden“ und das Anlegen eines Entra-Kontos in der Weboberfläche im Browser. Eine echte Authenticator-App (der Test rechnet nach RFC 6238). Der Betrieb ohne Admin-Token mit vorhandenen Konten über einen Neustart (geprüft ist nur, dass der Server ohne Token und ohne Konto den Start verweigert). Die Anfragerate hinter einem echten Proxy. Wie lange die 600 000 PBKDF2-Runden auf dem Zielrechner dauern (auf dem Raspberry Pi hier: 1,4 s je Anmeldeversuch, unter Last mehrere Sekunden — der Browsertest wartet deshalb bis zu 30 s).

### Phase 5, zweiter Teil: PostgreSQL und Caddy

Stand 12. Oktober 2026.

- **Zustand, beide Arten** (`internal/store/store_test.go`, 4 Tests auf der Datei und auf PostgreSQL): von allem eines anlegen — Geräte in jedem Zustand, Tokens, Aufträge erledigt, gescheitert und wartend, eine Konsolensitzung, Konten beider Arten, Protokoll —, schließen, öffnen: derselbe Zustand, und die Auftragszählung geht weiter. Eine beim Stopp offene Konsolensitzung ist nach dem Start beendet, auch beim zweiten Start. Was der Server aus dem Verlauf streicht (über 200 Aufträge, ein nach fünf falschen Codes verworfenes Gerät), ist auch im Speicherort weg. Ein Auftrag, der beim Lesen abgelaufen ist, wird mit der nächsten Änderung geschrieben.
- **Nur PostgreSQL** (4 Tests): Nullbytes, kaputtes UTF-8 und `\u0000` in Namen, Gründen, Status und Antworten lassen das Speichern nicht scheitern; ein zweiter Server an derselben Datenbank startet nicht, nach dem Ende des ersten schon; die `state.json` eines Servers auf der Datei wird übernommen und umbenannt, ein zweites Mal nicht; vom Protokoll bleiben mehr Zeilen in der Datenbank, als der Server lädt.
- **Alle Go-Tests gegen ein echtes PostgreSQL 17** (`test/pg.sh`): Die Tests des Servers — Protokoll, Aufträge, Fernkonsole, Konten, Entra — laufen dabei mit dem Zustand in der Datenbank, jeder in einem eigenen Schema. Ohne Datenbank laufen sie weiter auf der Datei (`docker build --target test`).
- **Der ganze Aufbau** (`test/cloud.sh`, 42 Prüfungen, Caddy mit eigener Zertifizierungsstelle): ein Server auf der Datei mit einem Gerät und einem Auftrag; danach derselbe Zustand hinter Caddy in PostgreSQL — Serverschlüssel gleich, Gerät weiter aktiv und es meldet sich, ohne etwas zu merken. Das Zertifikat hält der Prüfung gegen Caddys Wurzel stand, auch für einen Client ohne Servernamen; http wird umgeleitet; HSTS; Antworten an Geräte kommen mit `Content-Length` und nicht in Stücken; weder Server noch Datenbank haben einen veröffentlichten Port. Ein neues Gerät meldet sich über Caddy an und führt einen Auftrag aus. Fernkonsole über Caddy: Begrüßung, Eingabe, Echo, und eine Abfrage, die 20 s auf Ausgabe wartet, bleibt so lange offen. Ein Konto wird über Caddy eingerichtet (das Cookie ist `__Host-c2`, `Secure`, `HttpOnly`) und öffnet die Admin-Schnittstelle. Im Protokoll steht die Adresse des Clients, nicht die von Caddy und nicht die, die der Client in `X-Forwarded-For` behauptet. Ein zweiter Server an der Datenbank verweigert den Start. **Neustart ohne Admin-Token:** Der Server startet mit den Konten aus der Datenbank, das alte Token gilt nicht mehr, Anmelden mit Passwort und Code geht, beide Geräte sind da, und das Sperren eines Geräts steht mit dem Kontonamen in der Tabelle `audit`.
- **Am Waveshare-Board** (12. Oktober 2026, Caddy auf dem Pi unter `https://192.168.10.232`, Zertifikat von Caddys eigener Stelle): angemeldet, bestätigt, `ping`, `status`, `settings`, `configs` ausgeführt; Gerät und Aufträge stehen in der Datenbank. Die Firmware (mbedTLS 2, TLS 1.2, kein Servername) kommt also mit Caddys TLS zurecht. Der Handshake dauerte 6,3 s, davon 3,4 s Rechenzeit — rund dreimal so lang wie bisher gemessen; das Board hing dabei nicht am USB-Port des Pi, vermutlich lief es im Sparmodus mit 80 MHz. Nachgeprüft ist das nicht.
- **Weiter in Ordnung:** `test/e2e.sh` und `tests/c2/run.sh` (beide auf der Datei).
- **Nicht geprüft:** ein echtes Zertifikat von Let's Encrypt (dafür braucht es einen öffentlichen Namen) und damit auch, ob die Firmware dessen längere Zertifikatskette verträgt; der Betrieb im Internet; die Fernkonsole am Board über Caddy (das Board war nicht per USB erreichbar, also kein Tipp am Display möglich); die Weboberfläche im Browser hinter Caddy (`test/ui.sh` läuft gegen den Server allein); ein längerer Ausfall der Datenbank (ein kurzer ist seit dem vierten Teil geprüft, siehe unten); ein Zustand mit vielen Geräten (Ladezeit, Speicher); ein späterer Schemawechsel (es gibt erst die eine Migration).

### Phase 5, dritter Teil: Sicherung

Stand 12. Oktober 2026, auf dem neuen Entwicklungsrechner (x86-64, nicht mehr der Raspberry Pi).

- **Sicherung und Wiederherstellung** (`test/backup.sh`, 22 Prüfungen, der Aufbau aus `docker-compose.cloud.yml`): Der Dienst schreibt von selbst eine Sicherung, lesbar nur für ihren Besitzer, und innerhalb des Abstands keine zweite; die Zustandsprüfung des Containers sieht sie. Eine Sicherung auf Zuruf; danach wird ein zweites Gerät angemeldet und das erste gesperrt. Zurückspielen bei laufendem Server wird abgelehnt; eine kaputte Datei ändert nichts an der Datenbank. Nach dem Zurückspielen: das erste Gerät wieder aktiv, das zweite unbekannt, Serverschlüssel gleich, der alte Auftrag samt Antwort da, das Gerät meldet sich. Von drei Sicherungen bleiben mit `C2_BACKUP_KEEP=2` die neuesten zwei. **Alle Volumes gelöscht:** Datenbank neu, die herausgeholte Kopie hinein, zurückgespielt, Server gestartet — derselbe Serverschlüssel, das Gerät meldet sich, ohne etwas zu merken, und führt einen neuen Auftrag aus. Der Sicherungscontainer hängt nur am inneren Netz, ohne Rechte und mit schreibgeschütztem Dateisystem.
- **Weiter in Ordnung auf dem neuen Rechner:** die Host-Tests, `test/e2e.sh`, `test/pg.sh`, `test/cloud.sh`, `tests/c2/run.sh`, und die Firmware baut. `test/ui.sh` ist dort noch nicht gelaufen.
- **Nicht geprüft:** der Lauf über einen echten Tag (der Abstand ist nur als „innerhalb des Abstands keine zweite“ geprüft, die Wiederholung nach einem Fehlschlag gar nicht); eine Sicherung aus einem älteren Schema in einen neueren Server; große Datenbanken; ein echtes Gerät nach dem Zurückspielen.

### Phase 5, vierter Teil: Überwachung

Stand 12. Oktober 2026.

- **Speicher** (`internal/store/store_test.go`): Verschwindet das Datenverzeichnis, merkt es die Prüfung, nennt Fehler und Beginn, und eine Änderung wird abgewiesen; ist es wieder da, schreibt die nächste Prüfung nach, was liegen blieb. Mit PostgreSQL (`test/pg.sh`): Wird die Verbindung beendet, an der die Sperre hängt, nimmt die Prüfung sie neu — ein zweiter Server startet weiterhin nicht. War ein zweiter schneller, meldet der erste das, schreibt nichts mehr, und der zweite arbeitet.
- **Server** (`TestHealthAndStatus`): `/healthz` ohne Anmeldung, `/admin/v1/status` nur mit; Geräte je Zustand, ein Gerät wird nach fünf Minuten ohne Meldung still; abgewiesene Anfragen werden gezählt; ohne Datenverzeichnis 503 und der Grund im Status, danach wieder 200.
- **Der ganze Aufbau** (`test/cloud.sh`, jetzt 54 Prüfungen): Der Container des Servers ist `healthy`. Datenbank gestoppt: `/healthz` über Caddy 503, die Zustandsprüfung des Containers scheitert, der Status nennt den Fehler, ein neues Token wird mit 500 abgewiesen, im Log steht `CANNOT BE SAVED`. Datenbank gestartet: 200, im Log `can be saved again`, beide Geräte da, ein Gerät meldet sich, und ein zweiter Server verweigert weiter den Start — die Sperre ist neu genommen.
- **Nicht geprüft:** ein Wächter von außen und überhaupt jede Benachrichtigung; eine Datenbank, die hängt statt zu fehlen (jede Änderung wartet dann bis zu 15 s unter der Sperre des Servers); ein Ausfall über Stunden; ob `unhealthy` bei Docker irgendetwas auslöst (von selbst nicht).

Was die Arbeit am Gerät gezeigt hat:

- **TLS und ML-KEM passen nicht gleichzeitig in den Speicher.** Eine TLS-Verbindung kostet rund 45 kB Heap, der Stack für ML-KEM 28 kB. Der Schlüsseltausch wird deshalb gerechnet, bevor die Anfrage hinausgeht und nachdem die Antwort da ist — in einem kurzlebigen Task mit großem Stack, dessen Speicher sofort wieder frei ist (`big()` in `c2.cpp`).
- **Der Server muss `Content-Length` nennen.** Gos `net/http` schickt Antworten über 2 kB sonst in Stücken (chunked); die Handshake-Antwort hat 2257 Byte. Simulator und PC-Test hatten das nicht bemerkt, das Gerät schon.
- **Schwaches WLAN.** Am Testplatz (−77 bis −83 dBm, Sendeleistung 8,5 dBm) kamen Pakete über etwa 1100 Byte zeitweise gar nicht durch — das betrifft auch die Weboberfläche des Geräts im LAN, nicht nur die Leitstelle. Mit 19,5 dBm ging es meist. Der Client wiederholt eine gescheiterte Anfrage deshalb bis zu dreimal und wartet bis zu 20 s; mehr kann er gegen eine schlechte Funkstrecke nicht tun. Möglich wäre, die MTU des WLAN-Clients bei schwachem Signal zu senken.

## 8. Grenzen und offene Fragen

- **Eigenes Protokoll.** Der Aufbau folgt bekannten Mustern (KEM-basierte Authentifizierung wie bei KEMTLS, hybride Ableitung wie bei X25519MLKEM768), ist aber selbst zusammengesetzt und **von niemandem begutachtet**. Vor einem Betrieb im Internet sollte jemand mit Kryptografie-Erfahrung darauf sehen. Die Bausteine selbst kommen aus Gos Standardbibliothek.
- **Rechenzeit am Gerät:** gut eine Sekunde je Handshake (§7), einmal pro Stunde. Der Hauptschleife und damit der seriellen Brücke nimmt das nichts, der Client hat einen eigenen Task auf dem anderen Kern. Ob sich das im Akkubetrieb bei 80 MHz bemerkbar macht, ist offen.
- **Der Geräteschlüssel liegt unverschlüsselt im Flash**, wie heute schon die 802.1X-Schlüssel. Wer das Gerät hat, hat den Schlüssel; dagegen hilft nur Sperren am Server.
- **Sechs Ziffern** lassen einem Angreifer, der ein Token abgefangen hat, eine Chance von eins zu einer Million je Versuch, bei fünf Versuchen. Mehr Ziffern sind möglich, aber lästiger abzulesen.
- **Die Anfragerate ist je Absenderadresse begrenzt** (§5.1), nicht insgesamt: Viele Adressen zugleich bremst das nicht. Geräte hinter demselben Anschluss teilen sich ein Kontingent (30 Handshakes auf einmal). Ob die Firmware ein 429 beim Handshake anders behandelt als einen Netzfehler, ist nicht geprüft.
- **Konten (§5.1):** Es gibt nur Administratoren, keine abgestuften Rechte. Wer einen Kontonamen kennt, kann ihn mit fünf falschen Versuchen für 15 Minuten aussperren. Das TOTP-Geheimnis und die Passwort-Hashes liegen in der Zustandsdatei bzw. in der Datenbank; wer sie hat, kann Codes erzeugen und Passwörter offline durchprobieren. Sitzungen und Fehlversuche stehen im Speicher: Ein Neustart meldet alle ab und vergisst Sperren. Das Protokoll liegt in derselben Datei oder Datenbank wie alles andere — wer den Server hat, kann es ändern. Solange `C2_ADMIN_TOKEN` gesetzt ist, bleibt es ein Zugang ohne zweiten Faktor; die Weboberfläche hält es dann im Browser-Tab (`sessionStorage`). Die Anmeldung über Entra ist nur gegen einen Nachbau getestet (§7).
- **Zustand und Proxy (§5):** Ein Serverprozess je Datenbank, also kein Betrieb mit mehreren Instanzen hinter einem Lastverteiler — dafür müssten auch Sitzungen und offene Konsolen aus dem Speicher heraus. Jede Änderung wartet unter der Sperre des Servers auf die Datenbank; eine langsame Datenbank bremst alles. Fällt die Datenbank aus, weist der Server Änderungen mit einem Fehler ab, behält sie aber im Speicher und schreibt sie, sobald sie wieder da ist — ein abgewiesenes Sperren oder ein abgewiesenes Token gilt also trotzdem, und wer in der Zwischenzeit den Server neu startet, verliert es. Zwischen dem Verlust der Sperre und der nächsten Prüfung (bis zu 30 s) könnte ein zweiter Prozess starten; der erste hört dann auf, aber was beide in dieser Zeit schrieben, kann sich widersprechen. Die Überwachung meldet nur, sie benachrichtigt niemanden und läuft auf demselben Rechner — dass der Rechner weg ist, merkt nur ein Wächter von außen. Der private Serverschlüssel liegt unverschlüsselt in der Tabelle `server` und in jeder Sicherung. Die Sicherungen (§5) liegen unverschlüsselt auf demselben Rechner wie die Datenbank: Sie helfen gegen eine kaputte oder versehentlich gelöschte Datenbank, nicht gegen den Verlust des Rechners — dafür muss jemand sie wegbringen, und dort sind sie so geheim wie der Server selbst. Wer Datenbank und Sicherungen verliert, meldet alle Geräte neu an. Zurückspielen dreht auch Sperren zurück: Ein nach der Sicherung gesperrtes Gerät oder Konto ist danach wieder aktiv, bis es jemand erneut sperrt; was Geräte zwischen Sicherung und Ausfall gemeldet haben, fehlt. Das Passwort der Datenbank steht in der `.env` und in der Umgebung des Server-Containers. Die Strecke zwischen Caddy, Server und Datenbank ist unverschlüsselt (ein Rechner, Docker-Netze) — auf mehrere Rechner verteilt ginge das so nicht.
- **Aufträge** werden einmal zugestellt und verfallen (§4.3). Startet das Gerät neu, bevor es das Ergebnis meldet, erfährt der Server nicht, ob der Auftrag ausgeführt wurde — bei `play` muss man dann am Gerät nachsehen.
- **Die Freigabe am Gerät ist ein einziger Haken** für `play` und `reboot` zusammen, und sie gilt, bis jemand sie wieder entfernt. Die Fernkonsole hängt nicht an ihm: Sie braucht je Sitzung einen Tipp am Touch-Display und läuft ab (§4.5).
- **Die Fernkonsole ist nicht Ende-zu-Ende.** Der Server leitet die Zeichen im Klartext durch; wer den Server hat, liest eine offene Sitzung mit und kann in ihr tippen. Öffnen kann er keine — das entscheidet der Mensch am Display. Was getippt und gezeigt wurde, wird nicht aufgezeichnet; damit lässt sich nachträglich auch nicht belegen, was ein Administrator in einer Sitzung getan hat.
- **`play` kann der Server nur starten, nicht anhalten** und spielt nur, was am Gerät schon gespeichert ist; hochladen kann er nichts. Konfigurationen mit `{{Platzhaltern}}` gehen nicht (wie am Touch-Display).
- **TLS am Gerät prüft das Zertifikat nicht** (`setInsecure()`): Der innere Kanal beweist den Server ohnehin, und nur so geht ein selbstsignierter Testserver. Ob das Gerät im Betrieb zusätzlich gegen eine CA prüft, ist zu entscheiden; es wäre die sauberere Wahl, kostet aber ein CA-Bündel im Flash. Ohne Prüfung sieht ein Mann in der Mitte, **dass** ein Gerät mit der Leitstelle spricht, und kann stören — mitlesen oder sich als Server ausgeben kann er nicht.
