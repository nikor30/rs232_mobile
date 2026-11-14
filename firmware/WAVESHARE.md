# Waveshare ESP32-S3-Touch-LCD-2

ESP32-S3R8 (16 MB Flash, 8 MB PSRAM) mit 2″-Farbdisplay 240×320 (ST7789T3), kapazitivem Touch (CST816D), Lagesensor (QMI8658), SD-Slot, LiPo-Anschluss und USB-C am nativen USB-Port.

```bash
pio run -e waveshare-s3-lcd2 -t upload
```

Stand 2. Oktober 2026: gebaut, geflasht und am Board geprüft, soweit unten angegeben. Arduino-Core 2.0.17. Seit dem 9. Oktober 2026 ist dies das einzige Board der Firmware; Weboberfläche und Netzfunktionen beschreibt [README.md](README.md).

## Was das Board zusätzlich kann

| Funktion | Datei | Geprüft |
|---|---|---|
| Farb-Touch-Oberfläche mit neun Seiten | `src/lcd_ui.cpp` | alle Seiten quer und hoch per Screenshot angesehen, jede Taste per simuliertem Tippen ausgelöst, 4 Minuten Zufallstest (336 Aktionen) ohne Absturz; mit echtem Finger nur die Vorversion |
| Seitenübersicht, Blättern mit Schwung und Gummiband, Wisch-Hinweis (12. Oktober 2026) | `src/lcd_ui.cpp` | am Board mit simuliertem Finger: Übersicht quer und hoch per Screenshot, Sprung auf eine Seite; Stand der Seite über `lcd_debug.py status` gelesen (läuft nach dem Loslassen weiter, gibt über das Ende hinaus nach und kehrt zurück, ein ruhender Finger hält sie an); Wisch-Hinweis von beiden Seiten per Screenshot bei gehaltenem Finger; 150 s Zufallstest nur mit Gesten (302 Aktionen) ohne Absturz. **Ungeprüft:** wie es sich unter einem echten Finger anfühlt — das Bild kommt mit rund 10 Bildern je Sekunde, die Bewegung ist richtig gerechnet, aber nicht flüssig wie am Handy |
| Automatisches Drehen über den Lagesensor | `src/lcd_ui.cpp` | am Gerät bestätigt (quer); die beiden Hochformat-Richtungen sind gerechnet |
| Serielle Konsole über Bluetooth LE | `src/ble.cpp` | Kopplung mit PIN, Verschlüsselung und Schreiben vom Raspberry Pi aus; Ein-/Ausschalten mehrfach; Richtung Gerät → Handy ungetestet (kein Loopback verdrahtet) |
| Akkuanzeige mit Ladeerkennung | `src/power.cpp` | Messung an IO5 mit 1000-mAh-LiPo bestätigt (6. Oktober 2026); „lädt" am USB-Port eines Rechners gesehen; Akkubetrieb, Netzteil und Ab-/Anstecken **ungeprüft** — siehe unten |
| Stromsparen im Akkubetrieb, Ausschalten | `src/power.cpp`, `src/lcd_ui.cpp`, `src/main.cpp` | Sparmodus erzwungen (`lcd_debug.py saver 1`): 80 MHz, Abdunkeln nach 15 s, Aufhellen bei Berührung gesehen; Ausschalten und Wecken per Zeitablauf dreimal durchlaufen. **Ungeprüft:** echter Akkubetrieb ohne USB, Wecken mit der BOOT-Taste, Abschalten bei leerem Akku, jede Stromaufnahme (kein Messgerät) — siehe unten |
| Mitschnitt auf SD-Karte | `src/sdcard.cpp` | mit einer 32-GB-SDHC-Karte am Gerät geprüft (CS = IO41), auch über Resets; eine alte 1-GB-SDSC-Karte fiel aus, siehe unten |
| SD-Karte als USB-Stick | `src/sdcard.cpp` | am Gerät geprüft (12. Oktober 2026), siehe unten |

## Bedienung

- **Ziehen nach oben/unten** verschiebt eine Seite, die länger ist als der Bildschirm; der Balken am rechten Rand zeigt, dass es weitergeht und wo man steht. Die Seite behält beim Loslassen die Geschwindigkeit des Fingers und wird von selbst langsamer; an ihren Enden gibt sie nach — je weiter gezogen, desto weniger — und kehrt ohne Nachschwingen zurück. Eine Berührung hält sie an, wo sie gerade ist, und drückt dabei nichts. Nichts wird mehr abgeschnitten: lange Werte laufen in die nächste Zeile, Listeneinträge und Titel, die einzeilig bleiben müssen, wandern langsam hin und her.
- **Tasten** sind 56 Pixel hoch (7 mm), Listenzeilen 44, die Pfeile in der Titelzeile haben je 78 × 46 Pixel (10 mm breit) — für große Daumen und Handschuhe in der Fertigung. Die Beschriftung einer Tastenreihe ist so groß, wie es die längste zulässt. Passen die Beschriftungen einer Tastenreihe nicht nebeneinander (hochkant), stehen sie in zwei Reihen.
- **Wischen** oder die Pfeile oben wechseln die Seite: Status, Netz, Terminal, Skripte, Bluetooth, Info, Setup — und System, wenn in der Weboberfläche „Einstellungen für Administratoren zeigen“ gesetzt ist. Die BOOT-Taste blättert ebenfalls. Während der Finger seitwärts zieht, kommt der Name der Seite, zu der es geht, von diesem Rand herein und bleibt unter dem Finger; er wird blau, sobald Loslassen die Seite wechselt (ab 50 Pixel, oder mit Schwung). Wer den Finger zurückbewegt, nimmt es zurück.
- **Der Titel** öffnet die Übersicht: alle Seiten als Kacheln, jede mit dem einen Wert, den man vorher wissen will (Baudrate, Hotspot-Name, Bluetooth-Zustand, Akku …). Ein Tipp führt direkt hin — statt bis zu acht Mal zu wischen. Noch ein Tipp auf den Titel schließt sie.
- **Status** beantwortet eine Frage: Kann ich an diesem Port arbeiten? Ein Wort, groß (Bereit / In Benutzung / Auto-Baud / Skript läuft / Fernkonsole), darunter die Leitungseinstellung und die RX/TX-Zähler. Sonst nichts. Tasten: Baud weiterschalten, Auto, BREAK.
- **Netz**: QR-Code für den Hotspot (das Handy öffnet danach die Weboberfläche von selbst — einen eigenen QR-Code dafür gibt es nicht mehr), daneben WLAN-Name, Passwort, IP und die Adresse im LAN.
- **Terminal**: Live-Ansicht der seriellen Leitung (52×19 Zeichen quer, 39×29 hoch), gespeist aus dem Replay-Puffer. Escape-Sequenzen werden verworfen, nicht ausgewertet. Tasten: Enter, Ctrl-C, BREAK, REC (nur mit SD-Karte).
- **Skripte**: die in der Weboberfläche (Reiter Konfig) gespeicherten Konfigurationen. Antippen wählt aus, erst „Senden an Port N" spielt ab — zeilenweise, mit Warten auf den Prompt, Stopp bei Fehlermeldung, Steuerzeilen `@pause`, `@expect`, `@break`, `@timeout` wie im Browser (`src/player.cpp`). Konfigurationen mit `{{Variablen}}` gehen nur in der Weboberfläche.
- **Bluetooth**: zeigt die sechsstellige PIN und schaltet Bluetooth ein/aus (wird gespeichert).
- **System**: CPU-Last je Kern, RAM, PSRAM als Balken; dazu freier Speicher, größter Block, längste Pause der Hauptschleife, Zeichenzeit, Chip-Temperatur, Laufzeit. Die CPU-Last ist eine Schätzung aus der Leerlaufzeit (Auflösung 1 ms).
- **Info**: Firmware, Hostname, Clients, Raw-TCP, Leitstelle (Zustand oder der Code zum Bestätigen, siehe [README.md](README.md#leitstelle)), Akku, SD-Karte, Lage, Board.
- **Setup**: Helligkeit, Abschaltzeit des Displays, WLAN-Client (die Akku-Kalibrierung steht nur noch in der Weboberfläche und an der Debug-Konsole), ganz unten **Ausschalten** (mit Nachfrage; quer dafür nach oben ziehen). „Netz waehlen" sucht Netze; ein verschlüsseltes Netz öffnet die Bildschirmtastatur (QWERTZ, drei Ebenen), ein offenes fragt nach. Gespeichert wird wie in der Weboberfläche, mit Neustart. 802.1X bleibt der Weboberfläche vorbehalten.
- Das Display dreht sich zur oberen Kante; liegt das Board flach, bleibt die letzte Lage. Bewegung und Berührung wecken das dunkle Display; die erste Berührung weckt nur.
- Helligkeit und Abschaltzeit kommen aus den vorhandenen Display-Einstellungen der Weboberfläche.

## Bluetooth

Nordic UART Service, fest auf Port 1. Der Gerätename ist der Hotspot-Name. Jede Verbindung muss mit der PIN gekoppelt werden, die das Display zeigt (neu bei jedem Start); ohne verschlüsselte, authentifizierte Verbindung geht kein Byte in keine Richtung, und eine ungekoppelte Verbindung wird nach 60 s getrennt. Gekoppelte Geräte bleiben gespeichert.

## Pins

| Zweck | GPIO | Quelle |
|---|---|---|
| LCD SCLK / MOSI / MISO / DC / CS / Backlight | 39 / 38 / 40 / 42 / 45 / 1 | LovyanGFX-Boardkonfiguration, am Gerät bestätigt |
| Touch + Lagesensor I²C SDA / SCL | 48 / 47 | ebenso |
| SD CS (SPI-Leitungen des LCD) | 41 | am Gerät bestätigt (Karte eingebunden) |
| Port 1 RX / TX | 44 / 21 (Voreinstellung) | Stiftleiste rechts Pin 4 „RXD“ (GPIO44) und links Pin 22 (GPIO21) — **nicht** Pin 3 „TXD“ (GPIO43). Am Testplatz ist der MAX3232 andersherum verdrahtet und Port 1 entsprechend eingestellt (RX 21, TX 44); siehe unten |
| Akkumessung (BAT_ADC, Teiler 3:1) | 5 | am Gerät bestätigt: 1,35 V am Pin bei angeschlossenem LiPo; voreingestellt |

Weitere Header-Pins sind noch nicht freigegeben. Die Belegung der Stiftleisten steht im Schaltplan des Herstellers (`ESP32-S3-Touch-LCD-2-SchDoc.pdf`, verlinkt im Wiki aus `info.md`).

### Warum Port 1 nicht auf TXD (IO43) sendet

IO43 ist `U0TXD`. Das ROM des ESP32-S3 schreibt bei jedem Reset seine Startmeldungen auf UART0 (Technical Reference Manual, Kapitel „Chip Boot Control“) — die landeten als Eingabe in der Konsole des angeschlossenen Geräts. Deshalb sendet Port 1 seit dem 6. Oktober 2026 auf **IO21**:

- IO21 steht nicht in der Tabelle der Einschalt-Störimpulse des Datenblatts.
- Der 4,7-kΩ-Pull-up der Kameraschnittstelle (TWI_SDA) hält die Leitung im Reset auf Ruhepegel. Mit eingesteckter Kamera ist der Pin belegt.
- IO44 (RXD) bleibt Empfangspin; er ist beim Start ein Eingang. „RX/TX tauschen“ geht weiterhin (21 ↔ 44).
- IO43 wird in der Weboberfläche nicht mehr angeboten. Eine gespeicherte Belegung mit IO43 fällt beim Start auf die Voreinstellung zurück.

### Sendeleitung bei Neustart, Update und Ausschalten (30. Oktober 2026)

Was an einem Port hängt, darf einen Neustart des Boards nicht merken: Lässt der UART den Sendepin los und treibt ihn niemand, sieht der MAX3232 low und legt die RS232-Leitung auf „space“ — länger als ein Zeichen ist das ein BREAK, und ein Router oder Switch mit erlaubtem Break fällt in seinen Bootloader. Deshalb (`serial_bridge.cpp`, Abschnitt „TX at rest“):

- **Vor jedem Neustart** — Update über `/update` oder die Leitstelle, `reboot`, Einstellungen, auch nach Absturz oder Watchdog — schaltet `__wrap_esp_restart_noos()` jeden Sendepin auf GPIO-Ausgang high (Ruhepegel „mark“) und hält ihn mit dem Pad-Hold fest (`-Wl,--wrap=esp_restart_noos` in `platformio.ini`; normaler und Absturz-Neustart gehen beide durch diese Funktion, im fertigen Image nachgesehen).
- **Beim Neuöffnen eines Ports** (Einstellungen, Auto-Baud, Selbstprüfung): erst das letzte Zeichen hinaus, dann festhalten, umstellen, loslassen, wenn der UART die Leitung wieder treibt.
- **Beim Ausschalten** (Tiefschlaf) bleiben die Sendepins festgehalten; vorher trieben die Pads frei. *Nicht geprüft.*
- **Der Hold überlebt den Neustart nicht** — am Anfang des nächsten Laufs sind die Hold-Register leer (gemessen). Ab dem Reset trägt der Grundzustand des Pins die Leitung: GPIO44 (U0RXD) startet als Eingang mit Pull-up, GPIO21 hat den 4,7-kΩ-Pull-up der Platine; beide lesen beim Start high (`txHold` in `/api/status`). Ein Sendepin ohne Pull-up im Reset braucht einen auf der Platine (10 kΩ nach 3V3) — ebenso das Einschalten, bevor Code läuft.

Geprüft am 30. Oktober 2026 mit der Konsole eines Cisco C2960CX am Port: 8 Neustarts und Updates, der Switch blieb oben. Das beweist allein wenig — ein Catalyst reagiert im Betrieb womöglich gar nicht auf BREAK. Sein Neustart an diesem Tag war laut `show version` ein `power-on`, kein Absturz und kein BREAK.

### MAX3232 am Testplatz (16./17. Oktober 2026)

Modul an 3V3 und GND der Stiftleiste, die beiden TTL-Leitungen an GPIO21 (links Pin 22) und GPIO44 (rechts Pin 4, „RXD“). Welche Leitung des Moduls Ein- und welche Ausgang ist, sagen die Aufdrucke billiger Module nicht verlässlich — am Testplatz sendet das Board auf **GPIO44** und empfängt auf **GPIO21** (Port 1 so gespeichert, „RX/TX tauschen“ aus). Der erste Versuch scheiterte an einer Leitung auf dem falschen Stift.

- **Prüfen ohne Gegenstelle:** DB9-Pins 2 und 3 brücken, dann `lcd_debug.py pins`: Der Befehl schließt den UART kurz, treibt jeden der beiden Pins und liest den anderen. Die Zeile mit „folgt (Schleife geschlossen)“ nennt den Sendepin. Zeigen beide „folgt nicht“, ist die Schleife offen (Verdrahtung, Versorgung des Moduls).
- **Geprüft:** Schleife über die Weboberfläche (Echo, RX-Zähler steigt); ein Cisco WS-C3750X-24P-S (IOS 15.2(4)E10) mit 9600 8N1 über das Web-Terminal (Eingabeaufforderung, `show version`) und über die Fernkonsole der Leitstelle Ende-zu-Ende in beiden Richtungen.

## Akku und Laden

Am MX1.25-Stecker hängt ein 1S-LiPo (hier 1000 mAh); der ETA6096 auf dem Board lädt ihn, sobald USB-C Strom liefert. **Der Laderegler hat keine Statusleitung zum ESP32.** Ob geladen wird, schließt `src/power.cpp` deshalb aus dem, was sich beobachten lässt:

| Beobachtung | Schluss | Grenze |
|---|---|---|
| Der USB-Port empfängt Start-of-Frame-Pakete (Framezähler läuft) | ein Rechner speist das Board → lädt | ein reines Netzteil sendet nichts |
| Spannung springt in 15 s um 40 mV nach oben / unten | Ladegerät an- / abgesteckt | Schwelle geschätzt, nie am Gerät ausgelöst; ein beim Einschalten schon steckendes Netzteil bleibt unbemerkt |
| Spannung ≥ 4,15 V | Laderegler hält die Zelle | — |
| ≥ 4,15 V seit 30 min **oder** über 4,0 V seit 15 min auf 8 mV konstant | voll | Zeiten geschätzt |

Anzeige: Akkusymbol mit Füllstand links im Kopf, grün mit Blitz beim Laden, grün ohne Blitz bei „voll", rot bei schwachem Akku; die Prozentzahl daneben, wo der Titel Platz lässt. Die Zeile „Akku" (Status, Info) nennt Prozent, Millivolt und `laedt`/`voll`. Die Weboberfläche zeigt ⚡ statt 🔋.

`tools/lcd_debug.py bat` nennt die Rohwerte hinter der Anzeige (Pin-Spannung, Teiler, USB-Host, Sprung, Zeiten).

Zu wissen:

- **Die Prozentzahl stammt allein aus der Spannung** und liegt beim Laden zu hoch (die Ladespannung liegt über der Ruhespannung).
- **Die Messung liest vermutlich zu niedrig.** Am USB-Port stand die Spannung minutenlang unbewegt bei 4,05 V — so verhält sich eine volle Zelle an 4,2 V, keine, die noch lädt. Ohne Kalibrierung zeigt ein voller Akku dann etwa 83 % — dafür gibt es die Kalibrierung (nächster Abschnitt). Wer die Spannung selbst richtigstellen will: mit dem Multimeter am Akku nachmessen und `BAT_CAL` in `config.h` setzen (echte Spannung / angezeigte Spannung).
- Ohne Akku liegt am Messpunkt die Ausgangsspannung des Ladereglers; das ist von einem vollen Akku nicht zu unterscheiden.
- Die Messung ist auf diesem Board fest eingeschaltet: ein gespeichertes „keine Messung" wird beim Start durch IO5 ersetzt.

### Prozentanzeige kalibrieren

Die Kennlinie erwartet 4,20 V am vollen und 3,30 V am leeren LiPo. Was Teiler und ADC eines bestimmten Boards in diesen beiden Zuständen melden, lässt sich speichern; die Kennlinie wird dann zwischen die beiden Punkte gespannt (`src/bat_curve.h`), ihre Form bleibt.

- **Weboberfläche:** Menü → Ports → „Akku-Anzeige kalibrieren": „Jetzt ist voll" / „Jetzt ist leer" übernehmen die gerade gemessene Spannung, die beiden Felder nehmen Werte in mV, „Zurücksetzen" stellt die Kennlinie wieder her. Wirkt sofort, ohne Neustart.
- **Touch-Display:** Setup, nach unten ziehen: Zeile „0 / 100 %" mit den geltenden Punkten, „Jetzt voll" / „Jetzt leer" (mit Rückfrage), nach einer Kalibrierung „Kalibrierung loeschen". Werte in mV eingeben geht nur in der Weboberfläche.
- **Debug-Konsole:** `tools/lcd_debug.py batcal full`, `batcal empty`, `batcal 3400 4050` (leer, voll in mV; 0 = Kennlinie), `batcal reset`. `bat` nennt die geltenden Punkte.

Vorgehen: **Voll** setzen, wenn der Akku geladen ist, das Ladegerät abgezogen und eine Minute vergangen — am Ladegerät liegt die Spannung höher als danach im Betrieb, sonst wird die Anzeige nach dem Abziehen nie 100 %. **Leer** lässt sich am Gerät kaum abpassen; den Wert eintragen, bei dem es zuletzt noch lief.

Grenzen: Ein Punkt darf höchstens 0,5 V vom Ende der Kennlinie entfernt liegen, beide mindestens 0,3 V auseinander. Geändert wird **nur die Prozentzahl** — die angezeigte Spannung, die Ladeerkennung (4,15 V) und das Abschalten bei leerem Akku (3,3 V) rechnen weiter mit der gemessenen Spannung. Ein 0-%-Punkt unter 3,3 V wird auf diesem Board also nie erreicht, das Gerät schaltet vorher ab. Ein anderer Akkutyp, Mess-Pin oder Teiler löscht die Kalibrierung.

Stand 7. Oktober 2026: Rechnung als Host-Test (`tests/bat_curve_test.cpp`); am Gerät über die Debug-Konsole gesetzt, zurückgewiesen (unplausibler Wert), nach einem Neustart wiedergefunden und zurückgesetzt; auf dem Touch-Display „Jetzt voll" und „Kalibrierung loeschen" per simuliertem Tap und Screenshot; `/api/batcal` per curl (setzen, Messwert übernehmen, Zurückweisung). **Ungeprüft:** der Block in der Weboberfläche im Browser, „Jetzt leer", und ob die Anzeige mit einem real leergefahrenen Akku stimmt.

## Stromsparen und Ausschalten

Das Board hat **keinen Netzschalter**; der Akku hängt immer am Regler. Was die Firmware tun kann, tut sie von selbst, sobald `Power::saver()` wahr ist — also 10 s nachdem die Ladeerkennung „Akkubetrieb" meldet:

| Maßnahme | Wo | Wirkung |
|---|---|---|
| CPU-Takt 80 statt 240 MHz | `main.cpp: cpuClock()` | Funk, UART und SPI laufen am 80-MHz-Bustakt weiter; nicht mitten in einer Übertragung umgeschaltet. Zurück auf 240 MHz, sobald Strom von außen erkannt wird |
| Hintergrundlicht nach 15 s ohne Berührung auf ein Viertel | `lcd_ui.cpp: uiTask()` | die nächste Berührung macht wieder hell und wird normal ausgeführt; die eingestellte Abschaltzeit gilt weiter |
| Dunkles Display: Touch-Abfrage alle 120 statt 50 ms, Schnappschuss jede Sekunde statt alle 100 ms | `lcd_ui.cpp` | Aufwecken dauert einen Wimpernschlag länger |
| Abschalten bei leerem Akku: unter 3,3 V für 30 s | `power.cpp: empty()` | schützt die Zelle; danach lässt sich das Gerät mit BOOT wieder einschalten, schaltet sich leer aber erneut ab |

Immer, nicht nur am Akku: Bluetooth meldet sich alle 250–400 ms statt alle 30–60 ms.

Grenzen: Ein reines USB-Netzteil erkennt die Ladeerkennung nicht (siehe oben) — das Gerät spart dann auch am Netzteil. HTTPS-Verbindungsaufbau dauert bei 80 MHz länger. Der Hotspot selbst lässt sich nicht sparen, er ist die Funktion des Geräts. Wie viel Milliampere das alles bringt, ist **nicht gemessen**.

**Ausschalten** (Setup → Ausschalten, oder von selbst bei leerem Akku) ist Tiefschlaf: Mitschnitte werden geschlossen, Display, Hintergrundlicht, Lagesensor und Karte schlafen gelegt, Funk aus. Wach bleibt nur die Überwachung der **BOOT-Taste — sie schaltet wieder ein** (RST auch). Der Laderegler arbeitet unabhängig weiter, ein ausgeschaltetes Gerät lädt am USB-Kabel. Die USB-Schnittstelle verschwindet solange am Rechner.

Details, die man kennen sollte:

- IO1 (Hintergrundlicht, aus) und IO41 (SD-CS, hoch) werden für die Schlafzeit festgehalten und beim Start wieder freigegeben. IO45 (LCD-CS) bewusst nicht: Es ist ein Strapping-Pin, der festgehalten bei einem Unterspannungs-Reset die Flash-Spannung auf 1,8 V stellen würde.
- Der Lagesensor wird ganz abgeschaltet und behält im Schlaf seine Versorgung. Beim Start muss deshalb erst sein Oszillator wieder an, sonst liefert er nur `FFFF` (`imuBegin()`).
- Nach dem Einschalten per Taste gilt der noch gehaltene Tastendruck weder als Werksreset noch als „nächste Seite".

## Aufbau der Oberfläche

Die serielle Brücke darf nie auf Pixel warten. Deshalb zwei Tasks:

- **Hauptschleife** (Core 1): besitzt den Zustand der Firmware. Alle 100 ms (bei dunklem Display jede Sekunde) kopiert sie, was die Seiten zeigen, in einen Schnappschuss aus einfachen Daten, füttert das Terminal-Raster und führt die angetippten Befehle aus.
- **UI-Task** (Core 0): besitzt Panel, Touch und Lagesensor. Er zeichnet nur aus dem Schnappschuss in einen Puffer im PSRAM und schickt **nur die 16-Pixel-Streifen, die sich geändert haben**.

Dazwischen liegen ein Mutex für den Schnappschuss, eine Befehlswarteschlange und ein paar Anforderungs-Flags. Kein `String` wird über die Task-Grenze gelesen. LCD und SD-Karte teilen sich die SPI-Leitungen; wer darauf spricht, hält `Display::busLock()`.

Gemessen: Die Hauptschleife pausiert im Betrieb 2–3 ms; die längste Pause ist das BREAK-Signal (300 ms, wie in der Weboberfläche). Ein kompletter Seitenwechsel braucht bei 20 MHz rund 100 ms, eine Statusänderung nur wenige Streifen. Die Werksdemo des Boards arbeitet nach demselben Muster (LVGL mit Teil-Updates in einem eigenen Task).

## Debug-Konsole: das Display ohne Hinsehen prüfen

Über den USB-Seriell-Port nimmt die Firmware Diagnosebefehle an; `tools/lcd_debug.py` bedient sie, ohne das Board zurückzusetzen. **Seit 13. November 2026 nur mit dem Häkchen „Debug-Konsole am USB-Port“** (Tab Gerät, `POST /api/usbdebug {"on":true}`) — ab Werk ist der Port außerhalb des Stick-Modus tot (siehe „SD-Karte als USB-Stick“):

```bash
tools/lcd_debug.py status                  # Laufzeiten beider Tasks, Touch, SD-Verlauf, Bluetooth
tools/lcd_debug.py stick                   # SD-Karte als USB-Stick (die Konsole endet; zurück: Neustart am Gerät)
tools/lcd_debug.py shot bild.png           # Screenshot dessen, was das Display zeigt
tools/lcd_debug.py screen 1 tap 54 205     # Seite wählen, Berührung simulieren
tools/lcd_debug.py rot 0 shot hoch.png     # Drehung erzwingen (bleibt, bis "rot auto" sie dem Sensor zurückgibt)
tools/lcd_debug.py drag 160 180 160 40     # Finger von A nach B: ziehen (scrollen) oder wischen
tools/lcd_debug.py hold 280 130 200 130 shot wisch.png lift   # Finger bleibt liegen: ansehen, was das Display während einer Geste zeigt
tools/lcd_debug.py bat                     # Akku: Rohwerte, worauf "laedt"/"voll" beruht, Sparmodus, CPU-Takt
tools/lcd_debug.py batcal full             # gemessene Spannung = 100 % (empty, reset, oder zwei Werte in mV)
tools/lcd_debug.py c2                      # Leitstelle: Zustand, Code, Dauer des Handshakes, Stack und Heap
tools/lcd_debug.py saver 1                 # Sparmodus wie im Akkubetrieb erzwingen (0 = sperren, -1 = automatisch)
tools/lcd_debug.py poweroff 15             # ausschalten, nach 15 s per Zeitablauf wieder an
```

`poweroff` **ohne Zeit** schaltet wirklich aus — dann hilft nur noch die BOOT-Taste am Gerät. Das Werkzeug sucht den Port selbst (`/dev/serial/by-id/...`), weil das Board nach dem Wiedereinschalten auch als `ttyACM1` auftauchen kann; `LCD_PORT=...` legt ihn fest.

Bleibt die Hauptschleife länger als 3 s stehen, meldet der UI-Task das von sich aus im Log (`[DIAG] Hauptschleife steht seit ...`).

## Erfahrungswissen

- **`Serial.setTxTimeoutMs(0)` blockiert, statt nie zu blockieren.** Die Schreibschleife der USB-Seriell-Klasse (Core 2.0.17) zählt den Wert herunter und läuft bei 0 über. Steckt das Board an einem Rechner, der den Port nicht geöffnet hat, bleibt `setup()` in der ersten längeren Log-Ausgabe hängen: kein Hotspot, kein Bluetooth, eingefrorene Anzeige. Das war sehr wahrscheinlich auch das „Einfrieren nach einigen Berührungen" (jede Berührung schrieb eine Logzeile). Jetzt 5 ms.
- **WLAN-Station plus Bluetooth braucht Modem-Sleep.** Ohne ihn bricht der WLAN-Treiber mit `abort()` ab, sobald die Station startet — auch beim bloßen Netz-Scan aus dem Hotspot-Betrieb.
- **Offene Netze nie mit einem einzigen Tippen verbinden.** Beim Testen ist genau das passiert; seitdem fragt die Oberfläche nach.
- **Vorzeichenlose Zeitvergleiche über Task- oder Funktionsgrenzen sind eine Falle.** `now - lastActivity > timeout` schaltete das Display bei jeder Berührung ab, weil `lastActivity` nach `now` gestempelt wurde und die Differenz überlief. Immer `(int32_t)(now - x)` vergleichen.
- **Der CST816 antwortet nicht auf jede Abfrage**, auch wenn der Finger aufliegt. Ohne Haltezeit (120 ms) zerfällt eine Berührung in mehrere Taps.
- **`SD.usedBytes()` läuft die ganze Belegungstabelle ab** — Sekunden. Nie pro Bild aufrufen; die Werte werden beim Einbinden und nach einem Mitschnitt gemerkt.
- **Die USB-Seriell-Klasse dieses Cores verwirft Daten**, sobald sie den Host für abwesend hält. Für den Screenshot geht die Ausgabe deshalb direkt in den USB-FIFO.
- **DTR/RTS setzen das Board zurück oder schicken es in den Bootloader.** Port ganz normal öffnen und `HUPCL` abschalten, damit der Kernel die Leitungen beim Schließen nicht fallen lässt (`lcd_debug.py` macht das).
- **Der QMI8658 liefert im Nur-Beschleunigungs-Modus eingefrorene Werte.** Erst nach Soft-Reset und mit eingeschaltetem Gyroskop (CTRL7 = 0x03) laufen die Daten. CTRL1 = 0x60 schaltet zusätzlich auf Big-Endian — hier 0x40.
- **LovyanGFX bringt eine fertige Konfiguration für genau dieses Board mit** (`lgfx_user/LGFX_ESP32_S3_Touch_LCD_2.h`): keine Reset- und keine Touch-Interrupt-Leitung, Panel liest über MOSI (`spi_3wire`). Abweichend hier SPI2 statt SPI3, weil LovyanGFX nur diesen Bus mit Arduinos `SPI`-Objekt und damit mit der SD-Bibliothek teilt.
- **Die Reihenfolge beim BLE-Advertising zählt.** Erst `enableScanResponse(true)`, dann Name setzen — sonst passt der Name nicht neben die 128-Bit-UUID.
- **`onAuthenticationComplete` kommt bei einem bereits gekoppelten Gerät nicht verlässlich.** Der Sicherheitszustand wird deshalb in `loop()` von der Verbindung gelesen.
- **Ein per I²C abgeschalteter Sensor überlebt den Reset des Prozessors.** Er hängt an 3V3, nicht am Reset. Alles, was vor dem Tiefschlaf abgeschaltet wird, muss der Start ausdrücklich wieder einschalten.
- **`rot N` wurde vom Lagesensor nach 0,6 s überstimmt**, sobald das Board nicht flach lag — die Screenshots zeigten dann immer dieselbe Lage. Eine erzwungene Drehung bleibt jetzt bis `rot auto`.
- **Erster Build auf dem Raspberry Pi dauert lange** (2,4 GB Pakete auf die SD-Karte). PlatformIO liegt in `/root/.local/share/pio-venv`.

## SD-Karte als USB-Stick (seit 12. Oktober 2026)

Die Karte lässt sich an den USB-Port verleihen: Was dort steckt — ein Switch, eine Firewall, ein Rechner — sieht einen USB-Stick (`RS232 Console SD card`) und kann Firmware-Images oder Konfigurationen davon lesen und darauf schreiben. Einschalten: Seite Info am Display („SD-Karte als USB-Stick“), Tab Gerät der Weboberfläche (`POST /api/stick`) oder `lcd_debug.py stick`. Zurück geht es nur mit einem Neustart (Taste im Dialog am Display, „Neustart“ in der Weboberfläche).

- **Die Karte gehört dann allein dem USB-Port.** Die Firmware fasst das Dateisystem nicht mehr an (`Sd::mounted()` ist falsch, Mitschnitte enden) — zwei Herren über eine FAT würden sie zerstören. Die Sektoranfragen des Hosts kommen im USB-Task an und gehen über `disk_read`/`disk_write` direkt zur Karte, mit `Display::busLock()`, weil das LCD dieselben Leitungen benutzt.
- **Außerhalb des Stick-Modus ist der USB-Port tot (seit 13. November 2026).** Er gehört dann dem USB-OTG-Controller, der nicht gestartet ist: kein Pull-up an D+, ein Host sieht gar kein Gerät (`Sd::usbStart()` in `setup()`; am Laptop geprüft: sauberes Abmelden, kein Gerät, keine Aufzählungsfehler). Erst der Stick-Modus startet TinyUSB (`stickDevice()`), und der Host sieht einen Stick, der eingesteckt wird — mit Medium, Klasse 0/0/0, eine Massenspeicher-Schnittstelle. Die Debug-Konsole (`lcd_debug.py`, Flashen per Kabel) gibt es nur mit dem Häkchen „Debug-Konsole am USB-Port“ im Tab Gerät der Weboberfläche (`settings.usbDebug`, NVS `usbdbg`, `POST /api/usbdebug {"on":true}`, startet neu); setzen lässt es sich nur am Gerät, nicht über `set` der Leitstelle. Ohne sie flasht man über die Weboberfläche (`/update`) oder mit gedrückter BOOT-Taste beim Einschalten (ROM-Downloadmodus). `/api/status` nennt `usbDebug` und `reset` (Grund des letzten Neustarts, `esp_reset_reason_t`: 1 Einschalten, 3 Software, 4 Absturz, 5–7 Watchdog) — einen Absturz sieht man ohne Debug-Konsole nur dort. Eine Zwischenfassung (30. Oktober bis 13. November) zeigte außerhalb des Stick-Modus einen leeren Stick ohne Medium; damit meldete der Besitzer am 12. November weiter Abstürze des Switches, und ein Host, der einen Leser ohne Medium einmal geprüft hat, fragt später nicht wieder.
- **Warum die Serial/JTAG-Einheit des Chips einen Cisco-Switch abstürzen lässt** (C2960CX, IOS 15.2(7)E5, 30. Oktober 2026 zweimal, `crashinfo_ext_8`/`_9`: „Corrupted magic value in in-use chunk“, „Software-forced reload“): Die Einheit beantwortet Anfragen nach Texten, die sie nicht hat, nicht mit einer Ablehnung (STALL), sondern mit nichts oder mit Stücken aus ihrem ROM — gemessen am Laptop: Text 4 → 18 Byte vom Typ 0x0F, Text 5 → **0 Byte**, Text 6 → 239 Byte, Text 7 → ein einzelnes `FF`, dazu Texte mit angehängtem NUL. Das eigene Stick-Gerät lehnt alles Unbekannte sauber ab (Texte 1–5 richtig, ab 6 STALL). Der Speicherauszug des Switches zeigt, was IOS daraus macht: Hinter einem 12-Byte-Stück des Heaps stehen rund 250 Byte, paarweise gedreht und um zwei Byte versetzt (`FD0110DF AB1234CD` erscheint als `01FD DF10 12AB CD34`) — die Umwandlung eines USB-Textes (UTF-16, Länge aus dem ersten Byte, zwei Byte Kopf übersprungen) mit einer Länge von 0 oder 255, also ohne Prüfung. Das ist ein Fehler in IOS; auslösen kann ihn jede leere oder abgebrochene Antwort auf eine Textanfrage — auch ein Gerät, das mitten in der Aufzählung verschwindet. **Regeln daraus:** Die Serial/JTAG-Einheit nie an ein Netzwerkgerät; außerhalb des Stick-Modus nichts am Port; ein Gerät am Switch nicht mitten in der Aufzählung wegnehmen. **Bestätigt am echten Switch (14. November 2026):** Das Board blieb eine Nacht mit eingeschalteter Debug-Konsole am Switch — dieser stürzte 100-mal ab, im Abstand von rund vier Minuten (`crashinfo_ext_10` bis `_109`, je ~270 kB; freier Speicher von 80 auf 52 MB), bei jedem Start mit `%USB_HOST_STACK-5-USB_ENUM_UNSUPPORTED_DEVICE` und `%SYS-3-BADMAGIC: Corrupt block`, bis jemand den Strom zog. Mit totem Port startet er sauber, und das Stick-Gerät nimmt er an. Die 100 Dateien liegen noch auf dem Switch.
- **Die ersten Millisekunden nach dem Einschalten (geschlossen am 13./14. November 2026).** Welche Einheit den Port hat, steht im RTC-Bereich und überlebt jeden Neustart (auch Absturz und Tiefschlaf) — aber nicht das Einschalten: Dann legt der Chip die Serial/JTAG-Einheit von selbst an den Port, und die Anwendung kommt erst nach rund einer halben Sekunde dazu (gemessen: ein Host hatte da schon alle Texte gelesen und einen Treiber gebunden). Ein Switch, in den ein **ausgeschaltetes** Board gesteckt wird, sah also genau das Gerät, das ihn abstürzen lässt. Deshalb hat die Firmware einen **eigenen Bootloader** (`bootloader/`): der Bootloader des Arduino-Cores 2.0.17 (ESP-IDF 4.4.7, DIO, 80 MHz) plus ein Haken `bootloader_before_init()` (`bootloader_components/usb_port/usb_port.c`), der als Erstes zwei Bits im RTC-Bereich setzt — der Port gehört dann dem nicht gestarteten OTG-Controller, die Serial/JTAG-Einheit ist weg, einige zehn Millisekunden nach dem Einschalten und vor jeder Aufzählung (ein Host wartet nach dem Erkennen mindestens 100 ms). Nur ein Port, über den noch niemand entschieden hat, wird angefasst; die Wahl der Anwendung (tot, Stick, Debug-Konsole) bleibt über Neustarts bestehen. `bootloader/build.sh` baut ihn (holt ESP-IDF 4.4.7 nach `~/.cache/rs232-console`, 300 MB); `./build.sh check` beweist, dass der Bau ohne Haken Byte für Byte der Bootloader des Cores ist. `platformio.ini` nimmt ihn über `board_build.arduino.custom_bootloader`. **Er wird nur über das Kabel geschrieben** (`pio run -t upload`; ein Update übers Netz oder die Leitstelle schreibt nur die Anwendung): Kabel heißt Debug-Konsole an (Häkchen, startet neu) oder BOOT gedrückt beim Einschalten (ROM-Downloadmodus). Am Board geprüft (14. November 2026): ein Reset, der den RTC-Bereich löscht wie das Einschalten (RTC-Watchdog, `reset` 7), zeigt dem Laptop **kein** Gerät mehr — mit dem Bootloader des Cores kam innerhalb einer halben Sekunde die Serial/JTAG-Einheit, aufgezählt und mit Treiber, und verschwand mit der Anwendung wieder; mit Debug-Konsole an kommt sie wie gewollt. **Nicht geprüft:** ein echtes Einschalten (Akku ab) und der Rückweg zum Original (`bootloader_dio_80m` aus dem Core, 14 032 Byte, geschrieben mit `esptool write_flash 0x0`; die Sicherung vom 13. November liegt im Scratchpad dieser Sitzung, nicht im Repository).
- **Ungeklärt: Das Board selbst startete am 13. November 2026 zweimal von allein neu**, beide Male am Laptop mit dem leeren Stick der Zwischenfassung, ohne Karte, nach 4,7 und 5,5 Stunden (12:39 mit `reset` 4 = Absturz; 18:52 Grund nicht gelesen). Ohne Debug-Konsole gibt es kein Log dazu. Ob es mit dem toten Port noch vorkommt, ist nicht beobachtet.
- **Tempo:** gemessen 470–540 kB/s lesen, rund 370 kB/s schreiben (USB Full Speed, SPI mit dem Display geteilt). 100 MB brauchen also um vier Minuten.
- **Was der Host sieht, ist die ganze Karte** mit ihrer Partitionstabelle. Switches und Firewalls lesen in aller Regel nur FAT/FAT32 auf der ersten Partition; die Firmware selbst benutzt ebenfalls die erste FAT-Partition.
- Der Dialog am Display zeigt, ob ein Host da ist, was gelesen und geschrieben wurde und ob der Host die Karte ausgeworfen hat. `lcd_debug.py stickui 20` zeigt nur den Dialog (zum Ansehen per Screenshot — im echten Stick-Modus gibt es keine Screenshots).
- Die Akkuanzeige erkennt einen Rechner am Port nur noch im Stick-Modus (`tud_mounted()`) oder mit Debug-Konsole (Rahmen der Serial/JTAG-Einheit). Am toten Port bleibt ihr die Spannung: Ein Netzteil oder Switch, der schon beim Start steckt, gilt als Akkubetrieb (Sparmodus), bis die Zelle 4,15 V erreicht. Im Stick-Modus ist der Sparmodus abgeschaltet.
- Eine Karte, die antwortet, aber kein lesbares Dateisystem hat (`sdRaw` in `/api/status`), lässt sich ebenfalls als Stick verleihen — Taste am Display und in der Weboberfläche erscheinen dann auch.

Geprüft am Board gegen einen Linux-Rechner: Einschalten per Tipp, per Weboberfläche und per Debug-Konsole; 4 MB geschrieben und Byte für Byte zurückgelesen; eine im Stick-Modus geschriebene Datei nach Neustart und erneutem Stick-Modus wieder gelesen; Auswerfen wird angezeigt; nach dem Neustart ist die Debug-Konsole wieder da und die Karte eingebunden. **Nicht geprüft:** an einem echten Switch oder einer Firewall, an Windows/macOS, mit großen Dateien, im Akkubetrieb (das Board wird dann aus dem USB-Port des Switches versorgt).

**An einem echten Switch gescheitert** (28. Oktober 2026, Cisco WS-C2960CX-8PC-L, IOS 15.2(7)E5): Der Switch meldet bei jedem Einstecken `%USB_HOST_STACK-5-USB_ENUM_UNSUPPORTED_DEVICE: Unsupported device inserted. Host id 3, Device Addr N` und legt kein `usbflash0:` an. Er weist das Board also schon bei der Aufzählung ab, bevor eine Datei gelesen wird. Vermutete Ursache (30. Oktober 2026, mit `lsusb -v` am Laptop): Das Board meldete sich als zusammengesetztes Gerät (`bDeviceClass` 0xEF/2/1, IAD — die Vorgabe des Arduino-Cores), obwohl es nur die eine Massenspeicher-Schnittstelle hat (8/6/80, SCSI, Bulk-Only); ein gewöhnlicher Stick meldet 0/0/0. Seitdem setzt `stickStart()` in `sdcard.cpp` die Klasse auf 0/0/0 — am Laptop geprüft (Deskriptor 0/0/0, Karte wird eingebunden und gelesen). **Am Switch (30. Oktober 2026):** Damit nimmt der C2960CX das Board an (`%USBFLASH-5-CHANGE: usbflash0 has been inserted!`), meldete aber `Invalid DOS media` — er las nur 2–3 Sektoren. Auf der Karte lag ein Raspberry-Pi-System (FAT 256 MB + ext4). Auch eine einzige FAT32-Partition über die ganze 32-GB-Karte nahm er nicht („unexpected values in partition table or boot sector“), und `format usbflash0:` scheiterte („Primary Partition created...Size 1856 MB“, dann „Didn't find 55AA signature in System Sector“). **Ursache:** Dieses IOS rechnet die Größe des Sticks in Byte mit 32 Bit — 32 010 928 128 Byte werden zu 1 946 157 056 Byte = 1856 MB. Seitdem zeigt das Board eine Karte über 4 GB, deren Partitionen alle in den ersten 4 GB liegen, dem USB-Host als 4-GB-Stick (`stickSize()` in `sdcard.cpp`, 8 386 560 Sektoren; `sizeMb` unter `stick` in `/api/status`); liegt eine Partition dahinter, bleibt es bei der ganzen Karte. Die Karte im Board hat dafür eine FAT32-Partition ab Sektor 2048 bis knapp unter 4 GiB (Typ `0x0b`, Name `RS232`; der Rest der Karte bleibt ungenutzt; alter Inhalt am Laptop unter `/root/backups/rs232-sdcard-20261030`). Am Laptop geprüft (4 GB gesehen, Dateien lesbar). **Merke:** Für ältere Cisco-Switches muss die Karte eine Partition unter 4 GB haben.

Auch mit 4 GB meldet der Switch „unexpected values in partition table or boot sector“, und `format usbflash0:` scheitert weiter mit „Didn't find 55AA signature in System Sector“. Was seitdem geändert ist:

- **SCSI beantwortet die Firmware selbst** (statt `USBMSC` des Cores): In dessen `tud_msc_scsi_cb` ist die Länge ein `uint16_t`, „-1 = nicht unterstützt“ wurde zu 65535 und als Erfolg gemeldet. Jetzt bekommen unbekannte Befehle ILLEGAL REQUEST; READ(6)/WRITE(6), SYNCHRONIZE CACHE, VERIFY, MODE SENSE(10), READ CAPACITY(16) werden beantwortet. Der C2960CX benutzt keinen davon — das war es also nicht.
- **Stick-Modus auch ohne lesbares Dateisystem:** Eine Karte, die die Firmware nicht einbinden kann (etwa nach einem gescheiterten Formatieren am Switch), wird roh verliehen (`sdcard_init()` und `disk_initialize()`; `sdcard_init()` allein startet die Karte nicht, die Größe war dann Speichermüll). Eine Karte ohne Partitionstabelle zeigt sich als 4 GB.
- **Diagnose:** `/api/status` nennt unter `stick` die letzten 48 Anfragen des Hosts (`trace`: Art, Sektor, Größe, Ergebnis, die letzten zwei Byte des ersten Sektors), `GET /api/stick/sectors` die letzten vier Ein-Sektor-Schreibvorgänge als Hex.

**Was der Mitschnitt zeigt:** Beim Formatieren liest der Switch Sektor 0, schreibt Sektor 32 (Bootsektor der Partition) und Sektor 0 (Partitionstabelle) mit je einem WRITE(10) über 512 Byte — und **in beiden fehlt schon beim Eintreffen das `55AA` am Ende** — dann liest er Sektor 0 zurück und findet es nicht.

**Ursache gefunden und behoben (14. November 2026): ein Zeitrennen im OHCI-Treiber des Switches.** Ein Gerät mit Full-Speed (alle ESP32-S3) hängt am Switch nicht am EHCI, sondern am OHCI-Begleiter (`show usb controllers`: Host 3, Typ OHCI, „Low/Full“; `show usb device` zeigt „Speed: Full“, „Device Supported: YES“). Dessen Treiber bekommt die Daten eines Auftrags falsch, wenn das Gerät **sofort** antwortet. Gemessen mit einem Stick im RAM des Boards (`POST /api/stick {"ram":true,"mb":N}`, 4 MB hinterlegt, N MB gemeldet; Abbild vorher mit `POST /api/stick/ram` laden; `/api/stick/replug` zieht den Stick und steckt ihn wieder), dasselbe Abbild jeweils wieder eingesteckt: ohne Verzögerung nahm der Switch es 3 von 12 Mal an („unexpected values“ sonst, nach einem einzigen Lesen von Sektor 0), bei 10 ms 3 von 10, 20 ms 6 von 10, 30 ms 8 von 10, 40 ms und 60 ms 10 von 10. Dieselbe Datei ergab ohne Verzögerung bei jedem Lesen eine andere MD5-Summe. Seine eigenen Schreibaufträge enthielten Heap-Reste (`ef4321cd`, Zeiger `0a49xxxx`) — **das fehlende `55AA` war ein Folgefehler, nichts ging auf dem Weg verloren.** Ein Flash-Stick ist langsam genug, um das nie zu treffen; das RAM des Boards und eine SD-Karte am SPI (rund 1 ms) sind es nicht — daher scheiterte jede Karte, jedes Layout, jede Größe (die 4-GB-Anzeige, FAT32/FAT16, Start 32/2048 waren nie die Ursache). Auch `debug usb …` am Switch spielt keine Rolle (aus: gleiches Ergebnis).

**Behebung:** `stickIo()` in `sdcard.cpp` antwortet auf das erste Stück eines Auftrags frühestens `STICK_FIRST_MS` = 60 ms nach dessen Eingang; TinyUSB reicht einen Auftrag in Stücken zu 4 kB weiter, ein Stück, das sofort am selben Ort anschließt (`STICK_CONT_MS` 25 ms), wartet nicht mehr (`stickNextMs` 0). Zur Messung zur Laufzeit änderbar: `POST /api/stick/delay?first=60&next=0`. **Am Switch geprüft (RAM-Stick, 2000 MB FAT16 mit 32-kB-Clustern, Typ 0x06, Start 32, und 2100 MB FAT32, Typ 0x0b, Start 32):** wird ohne Warnung angenommen (10 von 10 Mal bei 60 ms), `dir usbflash0:` und `more` zeigen die Dateien, `verify /md5` einer 3-MB-Datei stimmt 14 von 14 Mal mit der Summe des Rechners überein (bei 60 ms/0 ms und 40 ms/0 ms; mit 0 ms Antwortzeit drei Läufe, drei verschiedene Summen), rund 5 s für 3 MB (600 kB/s, weil IOS mit großen Clustern große Aufträge stellt und das Warten je Auftrag, nicht je 4 kB anfällt); `copy flash:config.text usbflash0:cfg.txt` legt die Datei an, die MD5-Summe der Kopie ist gleich der des Originals. Ein Abbild mit 512-Byte-Clustern (FAT12, 4 MB) liest IOS in Aufträgen zu 2 kB — dort kostet jeder 60 ms (rund 34 kB/s): **Karten mit großen Clustern vorbereiten** (FAT16 bis 2 GB mit 32 kB oder FAT32 ab etwa 2 GB mit 32 kB).

**Weiter offen:** Das **`format usbflash0:` des Switches selbst scheitert weiter**, auch mit richtigem Takt („Didn't find 55AA signature in System Sector“): Der Switch schreibt zwar jetzt den Partitionseintrag richtig (Sektor 0: `80 00 00 00 01 00 00 00 20 00 00 00 e0 1f 00 00`, Typ 0x01, Start 32, ganzer Stick), aber ohne `55AA`, und als Bootsektor (Sektor 32) einen nicht gefüllten Heap-Block (Füllmuster `0b0d`) — er baut das Dateisystem offenbar nie auf. Es kam kein SCSI-Befehl vor, den TinyUSB nicht selbst beantwortet (`cmds` leer); ob IOS dafür Geometrie aus MODE SENSE erwartet, lässt sich nicht sehen (TinyUSB beantwortet MODE SENSE(6) selbst, ohne Seiten). Bis das geklärt ist: **die Karte am Rechner vorbereiten.** Nicht am Switch geprüft: die echte SD-Karte mit dem Takt (im Board steckte keine), das Schreiben großer Dateien (IOS-Abbilder), Start 2048 und die 4-GB-Anzeige von `stickSize()` mit echter Karte.

**Werkzeug:** `tools/port_console.py PORT SEKUNDEN 'befehl\r' …` bedient einen seriellen Port des Boards über dessen WebSocket (`BOARD=<Adresse>`; `--until REGEX` wartet auf eine Antwort) — damit ließ sich der Switch über die Konsole am Port 1 des Boards fernbedienen, während das Board am USB-Port des Switches steckte.

**Schreiben im Stick-Modus (30. Oktober 2026):** Die Karte verweigert auch hier regelmäßig ein Schreiben. Das Board gab das als Fehler an den Rechner weiter, Linux verwarf die Seite („lost async page write“), und ein 25-MB-Image kam viermal hintereinander kaputt an. Jetzt versucht das Board jede Anfrage bis zu fünfmal (`stickIo()` in `sdcard.cpp`) und zählt in `/api/status` unter `stick` mit (`retries`: zunächst verweigert, `fails`: endgültig). Geprüft: 4 × 25 MB geschrieben und zurückgelesen, alle Summen gleich, dabei 4 Wiederholungen, 0 Fehler.

## Das SD-Karten-Problem (nur mit der alten Karte)

Mit einer 32-GB-SDHC-Karte tritt es nicht auf: über 50 Starts nach einem Reset in Folge eingebunden (`R+`), in zwei Stunden Betrieb kein Ausfall (Stand 12. Oktober 2026). Der Fehler gehört damit zu der alten Karte, nicht zum Aufbau.

Beobachtet mit einer 1-GB-SDSC-Karte:

- Nach dem **Einschalten** wird die Karte eingebunden.
- Nach einem **Reset ohne Stromunterbrechung** antwortet sie nicht mehr. Auf der Datenleitung liegt dann bei jedem Takt ein festes Muster (ein Null-Bit alle vier Takte), unabhängig von Chip-Select, Taktfrequenz und jedem Kommando (CMD0, CMD12, Stop-Token). 4096 Byte am Stück sind identisch — das sind keine Daten, die Karte hängt.
- Per Messung ausgeschlossen: der ESP32 selbst (IO40 ist kein Ausgang), das LCD (Software-Reset ändert nichts, es liest ohnehin über MOSI), ein unterbrochener Lesevorgang.
- Nur Strom weg holt die Karte zurück.

Beide naheliegenden Erklärungen sind inzwischen widerlegt:

1. *Der LCD-Takt von 40 MHz ist zu schnell für die Karte.* Mit 20 MHz fiel sie genauso aus.
2. *Der Reset bringt sie aus dem Tritt.* Sie fiel auch im laufenden Betrieb aus, ohne jeden Reset: eingeschaltet, eingebunden, nach spätestens 254 s tot (`K+ !254s` im Verlauf).

Was bleibt: Diese Karte verträgt den Datenverkehr des Displays auf den gemeinsamen Leitungen nicht, obwohl ihr Chip-Select dabei inaktiv ist. Es lag an dieser einen (alten) Karte: Die SDHC-Karte danach läuft am selben Aufbau ohne Ausfall.

Die Firmware führt Buch (`[DIAG] SD: ... Verlauf:` in `lcd_debug.py status`, im Flash gespeichert): `K+`/`K-xx` Start nach Einschalten, `R+`/`R-xx` Start nach Reset (xx = Antwort der Karte), `!<n>s` Karte fiel nach n Sekunden Betrieb aus (geprüft wird alle 20 s mit einem Sektor-Lesen).

Weitere Regeln, die dabei entstanden sind: Die Karte wird **vor** dem LCD gestartet (`Display::begin()` ruft `Sd::begin()`), und eine nach dem Start eingesteckte Karte lässt sich auf der Info-Seite einbinden.

## Offen

1. USB-Stick-Modus: am C2960CX gelöst (Takt der Antworten, siehe oben) — offen: die echte SD-Karte am Switch (FAT32 ab Sektor 2048, 4-GB-Anzeige), IOS-Abbilder von 20 MB und mehr lesen, der Rückweg zum Original-Bootloader und ein echtes Einschalten am Switch; `format usbflash0:` am Switch geht nicht (Karte am Rechner vorbereiten); 100 Absturzdateien auf dem Switch (`flash:crashinfo_ext/`) wegräumen, wenn der Besitzer es erlaubt.
2. Der Fix für das Blockieren ohne USB-Leser ist aus dem Code der USB-Klasse hergeleitet und im Betrieb ohne geöffneten Port geprüft, aber nicht mit einem echten Aus- und Einstecken.
3. WLAN-Beitritt: Suchen, Tastatur, Nachfrage, Speichern und Wieder-Ausschalten sind geprüft; ein erfolgreicher Verbindungsaufbau mit einem echten Schlüssel nicht.
4. Skripte: Ablauf geprüft, aber ohne angeschlossenes Gerät (jede Zeile wartet dann 2 s auf eine Antwort).
5. Bluetooth Richtung Gerät → Handy mit Loopback (TXD–RXD gebrückt) und mit einer Handy-App prüfen.
6. ~~MAX3232 anschließen, echter Konsolenzugriff~~ — erledigt am 16./17. Oktober 2026, siehe „MAX3232 am Testplatz“. Offen: prüfen, dass beim Reset nichts auf dem Sendepin erscheint.
7. Akku: Spannung mit dem Multimeter gegenmessen (`BAT_CAL`); Ladeerkennung im Akkubetrieb, am Netzteil und beim Ab-/Anstecken prüfen. Davon hängt der Sparmodus ab.
11. Stromsparen: USB abziehen und prüfen, dass nach 10 s `[PWR] CPU-Takt 80 MHz` kommt (über die Weboberfläche oder nach dem Wiederanstecken im Log) und das Display abdunkelt; Stromaufnahme an, dunkel und ausgeschaltet messen.
12. Ausschalten am Display (Setup → Ausschalten → Ja) und Einschalten mit der BOOT-Taste von Hand prüfen; ebenso das Scrollen und die größeren Tasten mit echtem Finger.
13. Bluetooth mit dem längeren Meldeintervall vom Handy aus suchen.
8. SD-Mitschnitte sind nur am Display bedienbar, nicht in der Weboberfläche.
10. Baudraten-Wechsel am Display gelten bis zum Neustart (wie bei der BOOT-Taste), sie werden nicht gespeichert.
