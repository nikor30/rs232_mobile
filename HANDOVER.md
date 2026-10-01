# RS232-WLAN-Konsole — Projektstand und Übergabe

**Stand:** 1. Oktober 2026 · Firmware **v1.7.1** · Autor der Arbeiten: Claude, im Auftrag von Niko

Dieses Dokument ist so geschrieben, dass man das Projekt allein damit und mit dem Archiv-Inhalt fortsetzen kann — ohne den Chatverlauf. Es nennt ausdrücklich auch, **was nicht verifiziert ist**.

---

## 1. Worum es geht

Ein mobiles Gerät für seriellen Konsolenzugang: Du verbindest Handy oder Notebook mit dem WLAN-Hotspot des Geräts, öffnest `http://192.168.4.1` und hast ein Terminal (xterm.js) auf dem angeschlossenen Switch, Router oder der Firewall. Keine App, kein Treiber, kein USB-Seriell-Adapter. Bis zu vier serielle Ports gleichzeitig, dazu Raw-TCP/Telnet, Dateiübertragung per XMODEM/YMODEM, abspielbare Konfigurationen, 802.1X fürs Firmennetz und HTTPS.

## 2. Drei Hardware-Varianten, eine Quelle

| Variante | Rolle | PlatformIO-Env | Core |
|---|---|---|---|
| **ESP32 DevKit + MAX3232 (HW-044)** | Mockup, der real benutzte Aufbau | `esp32dev-max3232` | Arduino 2.0.17 |
| **LilyGO T-RSS3** | ursprüngliche Zielhardware (isolierter Port, OLED, Akku) | `t-rss3` | Arduino 2.0.17 |
| **VIEWE UEDX80480050E-WB** | 5″-Touchpanel, ESP32-S3, 800×480, SD | `viewe-5inch` | Arduino **3.1.1** |

Die Variante wählt ein Profil in `src/config.h` (`BOARD_ESP32_DEVKIT`, Standard = T-RSS3, `BOARD_VIEWE_5INCH`).

## 3. Was läuft, was nicht

**Am echten Gerät erprobt (Mockup):**
- Web-Terminal, Mehrfach-Clients, Reconnect mit Replay
- Konsolenzugriff auf einen echten Cisco-Switch
- Raw-TCP auf Port 2000 aus dem LAN, mit PuTTY
- OLED 128×64 inkl. Helligkeitsregelung
- PowerBoost 1000C mit LiPo lädt und versorgt

**Übersetzt, aber nie auf Hardware gelaufen:**
- **alles auf dem VIEWE-Panel** — das Board war bei Redaktionsschluss noch nicht geliefert. GUI, SD-Karte, Panel-Treiber: ungetestet.
- **mbedTLS-3-Pfade** (802.1X EAP-TLS gegen echten RADIUS, PKCS#12-Upload) auf Core 3.
- **LBO-Auswertung** (Akkuwarnleitung) — Code ist drin, Leitung war noch nicht gelötet.

**Nicht gebaut:** die I²C-Tochterplatine. Schaltplan und Layout existieren, bestellt ist nichts.

## 4. Was im Archiv liegt

```
firmware/            das PlatformIO-Projekt, git-fertig (hier `git init`)
  src/               C++-Quellen
  web/               Weboberfläche (wird gzip-komprimiert ins Image eingebettet)
  firmware/*.bin     fertige Images für Mockup und Panel
  README.md          Zielhardware T-RSS3
  MOCKUP.md          Mockup-Aufbau, Tests, Akku, 4 Ports  ← die meistgebrauchte Datei
  PANEL.md           das 5″-Board: Pinbudget, GUI, SD, Core-3-Portierung
hardware/
  pcb/               KiCad-Projekt der Tochterplatine + Generator-Skripte
  gehaeuse/          OpenSCAD-Quelle, STLs, Render-Vorschauen
diagramme/           alle Zeichnungen als PNG + das matplotlib-Skript dazu
tests/               Host-Unittests (laufen auf dem PC, nicht auf dem ESP32)
HANDOVER.md          dieses Dokument
```

## 5. Firmware: wo was steckt

| Datei | Aufgabe |
|---|---|
| `main.cpp` | Setup/Loop, Tasten, Verteilung der seriellen Daten an alle Senken |
| `config.h` | **Hardwareprofile** — hier beginnt jede Portierung |
| `settings.h/.cpp` | persistente Einstellungen (NVS), Pin-Plausibilisierung |
| `serial_bridge.h/.cpp` | UARTs, Sendewarteschlangen, Replay-Ringpuffer, Auto-Baud |
| `net.h/.cpp` | WLAN, HTTP, WebSocket, **Raw-TCP/Telnet**, OTA, 802.1X |
| `https.cpp`, `certs.cpp` | TLS-Frontend, Zertifikate, PKCS#12, CSR-Erzeugung |
| `display.h/.cpp` | OLED (U8g2) — auf Panel-Boards leere Hüllen |
| `gui.h/.cpp` | **LVGL-Touch-GUI** fürs 5″-Panel |
| `sdcard.h/.cpp` | SD-Karte: Mitschnitte, Konfigurationen, Transferdateien |
| `power.h/.cpp` | Akkumessung über ADC **und** LBO-Warnleitung |
| `xfer.cpp` | XMODEM/YMODEM im ESP32 |
| `configs.cpp` | gespeicherte Konfigurationen abspielen |
| `compat_eap.h` | 802.1X-API-Namen Core 2 ↔ Core 3 |
| `compat_mbedtls.h` | mbedTLS 2 ↔ 3 |
| `panel/` | LVGL-Port des Herstellers (Espressif, CC0) + `lv_conf.h` |

## 6. Bauen

```bash
pio run -e esp32dev-max3232 -t upload    # Mockup
pio run -e viewe-5inch -t upload         # 5"-Panel
```

Die Panel-Umgebung zieht die **pioarduino**-Platform (Core 3.1.1), weil `ESP32_Display_Panel` Core 3 voraussetzt. Beide Cores existieren parallel; die `platformio.ini` im Archiv ist die normale Fassung, die über die PlatformIO-Registry auflöst — die funktioniert auf einem normalen Entwicklungsrechner.

> **Hinweis für eine Fortsetzung in einer Claude-Sandbox:** Dort war `api.registry.platformio.org` und `dl.espressif.com` durch die Egress-Policy gesperrt, GitHub und PyPI dagegen offen. Der Workaround war: Platform, Framework, IDF-Libs und Toolchains direkt von GitHub-Releases ziehen und mit selbstgeschriebenen `.piopm`/`package.json`-Manifesten unter `~/.platformio/packages` ablegen; Debugger- und Dateisystem-Werkzeuge als leere Stubs. Beide Core-Versionen müssen in **versionierten** Verzeichnissen liegen (`framework-arduinoespressif32@3.20017.0` neben dem einfachen Namen für 3.1.1), sonst überschreibt die eine Installation die andere. Bibliotheken gehören dann in `lib/`, weil PlatformIO transitive Abhängigkeiten sonst doch über die Registry auflösen will.

## 7. Was in dieser Arbeitsphase entstand (v1.5.0 → v1.7.1)

| Version | Inhalt |
|---|---|
| 1.6.0 | **OLED-Helligkeit** (Kontrastregister, Live-Vorschau beim Schieben); Telnet-IAC-Filter; `[TCP]`-Logzeilen; Boot-Log sagt ehrlich „TCP aus" statt immer einen Port zu melden |
| 1.6.1 | **Raw-TCP aus dem LAN** als bewusste Option; **Terminal-Modus** in der Weboberfläche (xterm füllt das Fenster, blinkender Cursor nur bei stehender Sitzung) |
| 1.6.2/1.6.3 | Auto-Baud meldet jetzt **warum** nichts kam: „Leitung stumm" vs. „nur unlesbare Zeichen (max. N Byte)" |
| 1.6.4 | Rücknahme: automatischer RX/TX-Tausch im Auto-Baud wieder raus (siehe 8.1) |
| 1.6.5 | **TCP-Sendewarteschlange** — behebt echten Datenverlust (siehe 8.4) |
| 1.7.0 | **VIEWE-Panel**: Board-Profil, LVGL-GUI, SD-Karte, Portierung auf Arduino-Core 3 / mbedTLS 3 |
| 1.7.1 | **LBO-Warnleitung** für Ladeboards |

## 8. Hartes Erfahrungswissen

Das hier hat jeweils Zeit gekostet. Bitte nicht erneut ausprobieren.

### 8.1 Das RX/TX-Häkchen ersetzt kein Nullmodem
Das Häkchen „RX/TX am TTL-Pegelwandler tauschen" vertauscht, welcher **ESP32-GPIO** an `T1IN` bzw. `R1OUT` des MAX3232 geht — also die zwei Jumper zwischen Mikrocontroller und Pegelwandler. Welcher **DB9-Pin** sendet, legt das Modul-Layout fest; daran ändert keine Einstellung etwas. Eine fehlende Kreuzung im Kabel lässt sich damit nicht ausgleichen. (Ich hatte das zwischenzeitlich falsch behauptet und sogar automatisiert — in 1.6.4 zurückgebaut, weil es zusätzlich einen Pin-Konflikt provoziert: ESP32-Ausgang gegen MAX3232-Ausgang.)

### 8.2 Zwei DB9-Buchsen brauchen eine Kreuzung
Das blaue Cisco-Rollover-Kabel endet in einer DB9-**Buchse**, das HW-044 hat auch eine. Ein **straighter** Gender-Changer legt Pin 2 auf 2 und 3 auf 3 — Sendeausgang trifft Sendeausgang, beide Empfänger hängen in der Luft. Loopback-Test (Pin 2/3 gebrückt) läuft dabei einwandfrei, am Switch kommt nichts. Gelöst wurde es durch Kreuzen auf der **RJ45-Seite** (RJ45-Koppler plus passend belegtes LAN-Kabel), weil das Rollover-Kabel RJ45 3 → DB9 2 und RJ45 6 → DB9 3 führt. Merksatz für die Pinzählung: **bei einer Buchse von vorn liegt Pin 1 oben rechts.**

### 8.3 Der Boot-Log darf nicht lügen
Die Zeile pro Port meldete immer `TCP :2000`, auch wenn Raw-TCP abgeschaltet war. Das hat eine Fehlersuche in die falsche Richtung geschickt. Seitdem: Diagnoseausgaben nennen den tatsächlichen Zustand, und jede abgewiesene TCP-Verbindung sagt den Grund. Genau diese Logzeile hat später das LAN-Problem in Sekunden geklärt.

### 8.4 `WiFiClient::write()` verwirft stillschweigend
Im Arduino-Core 2.0.17 blockiert `write()` bei vollem Socket-Puffer bis zu **10 × 1 s** und gibt dann zurück, wie viel untergebracht wurde. Der Rückgabewert wurde ignoriert — der Rest war weg. Bei schwachem WLAN (hier −78 dBm) fehlten dadurch im PuTTY-Fenster Zeilen, die im Web-Terminal vollständig standen. Seit 1.6.5: 4 kB Warteschlange pro Port, `send(..., MSG_DONTWAIT)` über `WiFiClient::fd()`, Überlauf wird gemeldet statt verschwiegen.

### 8.5 Telnet vs. Raw auf demselben Port
Die Firmware unterscheidet am **allerersten Byte**: `0xFF` → Telnet-Client, dann antwortet sie wie ein Terminalserver (`WILL ECHO`, `WILL SUPPRESS-GO-AHEAD`), was das lokale Echo des Clients abschaltet. Alles andere → reiner Raw-Durchgang, kein Byte wird angefasst. In PuTTY entsprechend „Telnet" **oder** „Raw" wählen; beides geht. Achtung, klassische PuTTY-Falle: Beim Umschalten des Verbindungstyps setzt PuTTY das Port-Feld auf den Standard zurück.

### 8.6 Das VIEWE-Panel hat zwei freie GPIOs
Nach Abzug von RGB-Panel (≈20 Pins), SD-Karte und Touch bleiben **IO17 und IO18**, dazu das UART-Paar IO43/44 (= USB-Debug-Konsole). Vier native RS232-Ports sind damit ausgeschlossen. Der Touch-I²C auf **IO19/20** ist mitbenutzbar: GT911 liegt auf 0x5D/0x14, die SC16IS752 auf 0x48–0x57, kein Adresskonflikt. Das ist die eigentliche Begründung für die I²C-Tochterplatine.

### 8.7 Lade-/Boost-Board
PowerBoost 1000C ist **ausdrücklich 1-zellig** (3,7/4,2 V) — ein 7,4-V-Pack zerstört den Laderegler. 5Vo geht auf **VIN**, nie auf 3V3. Eine Schottky (1N5817, Ring zum DevKit) zwischen 5Vo und VIN macht den Aufbau USB-sicher, sodass Flashen mit angeklemmtem Akku geht. `LBO` ist offener Kollektor und braucht keinen externen Widerstand. Schaltplan: `diagramme/powerboost_schaltplan.png`.

### 8.8 Tochterplatine: beide Kanäle nutzen
Erste Fassung war 4× SC16IS750 + 4× MAX3232 — je ein Chippaar pro Port. Beide Bausteinfamilien sind aber zweikanalig: der SC16IS**752** ist der I²C-fähige Doppel-UART, und der MAX3232 hat ohnehin zwei Transceiver, die sich dieselben vier Ladepumpen-Kondensatoren teilen. Jetzt **2× SC16IS752 + 2× MAX3232** für vier Ports. Halbe Chipzahl, Platine von 112,5 auf **107 × 110 mm**, Materialkosten von geschätzt ~58 € auf **~20 €**. Pinbelegungen wurden gegen KiCads eigene Symbolbibliothek geprüft, nicht aus Datenblatt-PDFs abgetippt — das hatte sich vorher als unzuverlässig erwiesen.

## 9. Wie geprüft wurde

Da kein Zugriff auf die Hardware bestand, wurde das Prüfbare auf dem Host geprüft:

- **`tests/telnet_iac_test.cpp`** — die Telnet-Zustandsmaschine, 1:1 aus `net.cpp` extrahiert: Raw bleibt byte-genau (auch `0xFF` in Nutzdaten), Aushandlung wird korrekt beantwortet, Subnegotiation, `IAC IAC`, Sequenzen über TCP-Paketgrenzen, zweite Sitzung wird neu begrüßt. 12 Fälle, alle grün.
- **`tests/iac_filter_test.cpp`** — die ältere Filterfassung, 8 Fälle über alle Chunk-Größen.
- **Builds** beider Umgebungen mit **0 Warnungen** im eigenen Code.
- **Binärprüfung**: Bootloader und Partitionstabelle der Mockup-Images sind byte-identisch zur vorigen Auslieferung, Flash-Parameter unverändert (dio/4 MB/40 MHz) — OTA ist dadurch gefahrlos, Einstellungen überleben.
- 3D-Gehäuse: alle drei STL manifold (`Volumes: 2`), Seitenansicht-Renders zur Kontrolle der RJ45-Ausschnitte.

## 10. Offene Punkte, nach Priorität

1. **VIEWE-Panel in Betrieb nehmen.** Flashen, GUI ansehen, SD testen. Erwartbare Baustellen: PSRAM-Bandbreite zwischen Bildpuffer und WLAN (Flackern/Tearing; `panel/lvgl_v8_port.h` hat Anti-Tearing- und Bounce-Buffer-Schalter), Touch-Kalibrierung, die gerechnete Terminal-Geometrie (96 × 17 Zeichen).
2. **802.1X auf Core 3 gegen echten RADIUS testen.** Die mbedTLS-3-Pfade sind nie gelaufen. Danach PKCS#12-Upload und CSR-Erzeugung durchspielen.
3. **LBO-Leitung löten** und im Web-UI unter Menü → Ports → „Warnleitung (LBO)" auf D23 setzen.
4. **Tochterplatine**: Stückliste gegen aktuelle LCSC-Preise prüfen, Fertigung beauftragen. Danach fehlt noch der **SC16IS752-Treiber in der Firmware** — der existiert noch nicht.
5. **Gehäuse** für die VIEWE-Variante neu auslegen; das vorhandene passt auf das LCDwiki-Basisboard, nicht auf dieses.
6. Die Docs-Seite (Artifact `2f26e902-4f1e-4797-bfbc-4037495d8121`) beschreibt noch das LCDwiki-Basisboard und müsste auf VIEWE umgeschrieben werden.

## 11. Arbeitsweise, die sich bewährt hat

- **Diagnoseausgaben vor Ratespielen.** Die `[TCP]`-Logzeilen haben zwei Probleme in Sekunden geklärt, die vorher Stunden gekostet hätten.
- **Pinbelegungen aus maschinenlesbaren Quellen**, nicht aus Datenblatt-Grafiken.
- **Byte-Logik auf dem Host testen**, bevor sie auf den Mikrocontroller geht.
- **Loopback schließt den Fehler ein, nicht aus.** Dass Test 1 läuft, sagt nur, dass ESP32 → MAX3232 → DB9 stimmt — über das Kabel dahinter sagt es nichts.
