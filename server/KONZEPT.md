# Leitstelle (Command-and-Control-Server) — Konzept

**Stand:** 9. Oktober 2026 · Phase 1 umgesetzt und lokal im Docker-Container getestet · Firmware-Seite noch nicht begonnen

Ein Server im Internet, bei dem sich die RS232-Konsolen anmelden. Er weiß, welche Geräte es gibt und ob sie erreichbar sind, und kann ihnen Aufträge geben. Dieses Dokument beschreibt das Ziel, die Sicherheitsannahmen, das Protokoll und die Phasen. Was davon gebaut und geprüft ist, steht in §7; wie man es startet, in `README.md`.

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
- Das **Gerät ist bewiesen**, sobald seine erste verschlüsselte Nachricht aufgeht. Bis dahin gilt die Sitzung nichts und verfällt nach 30 s.
- **Vorwärtsgeheimnis:** Wird später ein Langzeitschlüssel gestohlen, bleiben alte Mitschnitte zu.

### 4.3 Nachrichten

`POST /v1/msg`: Sitzungs-ID (16) ‖ Zähler (8) ‖ AES-256-GCM-Chiffrat. Antwort: Zähler ‖ Chiffrat. Der Zähler ist die Nonce und muss steigen (kein Wiedereinspielen); Richtung, Sitzungs-ID und Zähler sind mitauthentifiziert. AES-GCM, weil der ESP32 AES in Hardware rechnet. Höchstens 32 kB Klartext je Nachricht.

Der Klartext ist JSON (mit ArduinoJson am Gerät billig; später CBOR, falls die Größe stört):

```
Gerät → Server   {"t":"enroll","name":"Labor-1","info":{"fw":"1.8.0"}}
                 {"t":"poll","status":{...},"results":[{"id":7,"ok":true,"out":"pong"}]}
Server → Gerät   {"state":"pending|active","cmds":[{"id":7,"type":"ping","args":{}}],"poll_s":30}
```

Eine Sitzung gilt eine Stunde, dann handelt das Gerät neue Schlüssel aus. Sitzungen liegen nur im Speicher des Servers; nach einem Neustart antwortet er mit 401 und das Gerät macht einen neuen Handshake. HTTP-Status für Geräte: 401 = neu aushandeln, 403 = nicht (mehr) willkommen, ohne Begründung.

### 4.4 Registrierung mit zweiseitiger Prüfung

1. **Einladen.** Der Administrator erzeugt am Server ein **Einladungs-Token**: Server-Adresse, Fingerabdruck des Serverschlüssels, Token-ID und ein Geheimnis (32 Byte). Einmal verwendbar, läuft ab (Vorgabe 10 Minuten). Eine Textzeile `c2e1:…`, später auch als QR-Code.
2. **Token ins Gerät.** Über die Weboberfläche des Geräts (Hotspot, `192.168.4.1`) — also über einen Weg, den der Bediener in der Hand hat.
3. **Gerät prüft den Server.** Es holt den Serverschlüssel, vergleicht den Fingerabdruck mit dem Token und pinnt ihn. Im Handshake muss der Server diesen Schlüssel beweisen. Das Token-Geheimnis wird **nicht gesendet**, sondern in die Schlüsselableitung gemischt: Ein Server, der es nicht kennt, kann die Bestätigung nicht berechnen; ein Gerät, das es nicht kennt, kann keine gültige Nachricht schicken.
4. **Server merkt das Gerät vor** (`pending`). Es darf sich melden, bekommt aber keine Aufträge.
5. **Mensch bestätigt.** Beide Seiten leiten aus dem Handshake denselben **sechsstelligen Code** ab. Das Gerät zeigt ihn auf dem Display. Der Administrator **tippt ihn am Server ein** — der Server zeigt ihn nirgends an, damit niemand blind auf „Bestätigen" klickt. Stimmt er, ist das Gerät `active`. Nach fünf falschen Eingaben ist die Anmeldung verworfen.

Damit weiß das Gerät: richtiger Server (Fingerabdruck aus dem Token, im Handshake bewiesen). Und der Server: eingeladenes Gerät (Token-Geheimnis) **und** genau das Gerät, das der Administrator vor sich hat (Code vom Display, gebunden an den Geräteschlüssel).

**Sperren:** Ein gesperrtes Gerät wird sofort getrennt und kommt auch mit neuem Token nicht zurück (sein Schlüssel ist bekannt und gesperrt).

## 5. Server

- **Go, nur Standardbibliothek** (`crypto/mlkem` nach FIPS 203, `crypto/ecdh`, `crypto/hkdf`, AES-GCM). Keine fremden Pakete, ein statisches Programm, Container ohne Shell und ohne Root.
- **Admin-Schnittstelle** `/admin/v1/…`, JSON, geschützt durch ein Bearer-Token aus der Umgebung: Token erzeugen, Geräte auflisten, bestätigen, sperren, Auftrag einreihen, Aufträge und Ergebnisse lesen. Auftragsarten stehen auf einer festen Liste (Phase 1: `ping`, `status`).
- **Zustand** in einer JSON-Datei im Datenverzeichnis (Serverschlüssel, Geräte, Tokens, Aufträge), atomar geschrieben, Rechte 0600. Reicht für wenige Geräte und einen Prozess.
- **TLS:** lokal selbstsigniert (wird beim ersten Start erzeugt), mit eigenen Zertifikatsdateien, oder aus (hinter einem Proxy, der TLS beendet).

## 6. Phasen

| Phase | Inhalt | Stand |
|---|---|---|
| **0 Konzept** | dieses Dokument | fertig |
| **1 Server-Kern** | Protokoll, Registrierung mit Code, Sitzungen, Auftragswarteschlange, Admin-API, Gerätesimulator, Docker, Tests | **fertig, lokal getestet** (§7) |
| **2 Firmware-Client** | ML-KEM-768 auf dem ESP32-S3 (Kandidaten: mlkem-native, PQClean), X25519/HKDF/AES-GCM aus mbedTLS 2; Geräteschlüssel im NVS; Token-Eingabe in der Weboberfläche; Code auf dem Touch-Display; Poll-Schleife. Zuerst Host-Test der Firmware-Krypto gegen diesen Server (Testvektoren), dann aufs Gerät. Zu messen: RAM, Stack, Dauer eines Handshakes | offen |
| **3 Aufträge und Oberfläche** | Status und Telemetrie (Akku, WLAN, Ports), Einstellungen lesen, gespeicherte Konfiguration auf einen Port abspielen, Firmware-Update anstoßen; Weboberfläche für den Administrator; QR-Code fürs Token | offen |
| **4 Fernkonsole** | serielle Konsole über die Leitstelle, Ende-zu-Ende bis zum Browser des Administrators; nur nach Freigabe am Gerät (Touch), zeitlich begrenzt, mit Protokoll. Braucht einen schnelleren Rückkanal (Long-Polling oder WebSocket) | offen |
| **5 Cloud-Betrieb** | Datenbank statt JSON-Datei; echtes TLS-Zertifikat (Proxy oder ACME); Begrenzung der Anfragerate; Sicherung des Datenverzeichnisses; Benutzerkonten mit zweitem Faktor statt eines Tokens; Wechsel des Serverschlüssels; Überwachung | offen |
| **später** | signierte Firmware-Updates (ML-DSA oder SLH-DSA, geprüft im Gerät); Flash-Verschlüsselung für den Geräteschlüssel | offen |

Reihenfolge mit Absicht: Phase 2 vor allem anderen am Server, weil erst das echte Gerät zeigt, ob Größen und Rechenzeiten passen. Phase 5 muss vor dem ersten Betrieb im Internet abgeschlossen sein — Phase 1 ist ein lokaler Prüfstand.

## 7. Was in Phase 1 geprüft ist

Alles auf dem Raspberry Pi (arm64), Go 1.25, im Container. Nichts davon lief gegen ein echtes Gerät.

- **Protokoll** (`internal/proto`, 9 Tests): Sitzung und Anmeldung hin und zurück; Code auf beiden Seiten gleich; falscher Server (Mann in der Mitte) wird am Gerät erkannt; falsches Token-Geheimnis ebenso; ein Fremder mit Geräte-ID und öffentlichem Schlüssel bringt keine Nachricht durch; Wiedereinspielen, verfälschte und zurückgespiegelte Nachrichten werden verworfen; jedes gekippte Byte der Handshake-Antwort fällt auf; verstümmelte Eingaben.
- **Server über HTTP** (`internal/server`, 10 Tests): Anmelden, Bestätigen, Auftrag hin, Ergebnis zurück; vorgemerktes Gerät bekommt nichts; die Geräteliste verrät den Code nicht; falscher Code und Verwerfen nach fünf Versuchen; Token nur einmal und nur bis zum Ablauf; Token mit fremder Server-Adresse; gefälschtes Token-Geheimnis; Sperren trennt sofort und endgültig; unbekanntes Gerät; Sitzungsablauf und Server-Neustart; Admin-Token; Müll an den Geräte-Endpunkten.
- **Ende zu Ende im Container** (`test/e2e.sh`): Image bauen (16 MB), Server mit leerem Datenverzeichnis starten, simuliertes Gerät in eigenem Container anmelden, Token ein zweites Mal abgelehnt, falscher und richtiger Code, Auftrag `ping` → `pong`, Neustart des Servers, Sperren.
- **Äußeres TLS:** `openssl s_client -groups X25519MLKEM768` gegen den Container handelt TLS 1.3 mit genau dieser hybriden Gruppe aus.

## 8. Grenzen und offene Fragen

- **Eigenes Protokoll.** Der Aufbau folgt bekannten Mustern (KEM-basierte Authentifizierung wie bei KEMTLS, hybride Ableitung wie bei X25519MLKEM768), ist aber selbst zusammengesetzt und **von niemandem begutachtet**. Vor einem Betrieb im Internet sollte jemand mit Kryptografie-Erfahrung darauf sehen. Die Bausteine selbst kommen aus Gos Standardbibliothek.
- **ML-KEM auf dem ESP32 ist ungeprüft.** Erwartung aus veröffentlichten Messungen: wenige Millisekunden je Operation, etwa 10–20 kB Stack. Ob das neben WLAN, Bluetooth und Display passt, zeigt Phase 2.
- **Der Geräteschlüssel liegt unverschlüsselt im Flash**, wie heute schon die 802.1X-Schlüssel. Wer das Gerät hat, hat den Schlüssel; dagegen hilft nur Sperren am Server.
- **Sechs Ziffern** lassen einem Angreifer, der ein Token abgefangen hat, eine Chance von eins zu einer Million je Versuch, bei fünf Versuchen. Mehr Ziffern sind möglich, aber lästiger abzulesen.
- **Keine Begrenzung der Anfragerate.** Ein Handshake kostet den Server Rechenzeit; ohne Token oder bekannte Geräte-ID wird er zwar vor der Kryptografie abgewiesen, aber eine bekannte Geräte-ID genügt, um ihn auszulösen. Gehört in Phase 5.
- **Ein Admin-Token für alles**, keine Benutzer, kein Protokoll der Admin-Aktionen. Phase 5.
- **Aufträge** werden einmal zugestellt. Startet das Gerät neu, bevor es das Ergebnis meldet, bleibt der Auftrag auf „gesendet" stehen. Wiederholen oder Verfallen ist Phase 3.
- **TLS am Gerät:** ob das Gerät das Zertifikat der Leitstelle gegen eine CA prüft oder — weil der innere Kanal ohnehin den Server beweist — darauf verzichtet, ist zu entscheiden. Prüfen ist die sauberere Wahl, kostet aber ein CA-Bündel im Flash.
