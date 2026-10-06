# Mockup 2: Waveshare ESP32-S3-Touch-LCD-2

ESP32-S3R8 (16 MB Flash, 8 MB PSRAM) mit 2″-Farbdisplay 240×320 (ST7789T3), kapazitivem Touch (CST816D), Lagesensor (QMI8658), SD-Slot, LiPo-Anschluss und USB-C am nativen USB-Port.

```bash
pio run -e waveshare-s3-lcd2 -t upload
```

Stand 2. Oktober 2026: gebaut, geflasht und am Board geprüft, soweit unten angegeben. Core 2.0.17 wie das DevKit-Mockup.

## Was das Board zusätzlich kann

| Funktion | Datei | Geprüft |
|---|---|---|
| Farb-Touch-Oberfläche mit sechs Seiten | `src/lcd_ui.cpp` | alle Seiten quer und hoch per Screenshot angesehen, jede Taste per simuliertem Tippen ausgelöst, 4 Minuten Zufallstest (336 Aktionen) ohne Absturz; mit echtem Finger nur die Vorversion |
| Automatisches Drehen über den Lagesensor | `src/lcd_ui.cpp` | am Gerät bestätigt (quer); die beiden Hochformat-Richtungen sind gerechnet |
| Serielle Konsole über Bluetooth LE | `src/ble.cpp` | Kopplung mit PIN, Verschlüsselung und Schreiben vom Raspberry Pi aus; Ein-/Ausschalten mehrfach; Richtung Gerät → Handy ungetestet (kein Loopback verdrahtet) |
| Akkuanzeige mit Ladeerkennung | `src/power.cpp` | Messung an IO5 mit 1000-mAh-LiPo bestätigt (6. Oktober 2026); „lädt" am USB-Port eines Rechners gesehen; Akkubetrieb, Netzteil und Ab-/Anstecken **ungeprüft** — siehe unten |
| Mitschnitt auf SD-Karte | `src/sdcard.cpp` | Karte wird nach dem Einschalten erkannt (CS = IO41), **nach jedem Reset ohne Stromunterbrechung nicht mehr** — offen, siehe unten |

## Bedienung

- **Wischen** oder die Pfeile oben wechseln die Seite: Status, Terminal, Skripte, WLAN (QR), Web-UI (QR), Bluetooth, System, Info, Setup. Die BOOT-Taste blättert ebenfalls.
- **Status**: Schnittstelle, RX/TX-Zähler, Netz. Tasten: Baudrate weiterschalten, Auto-Baud, BREAK.
- **Terminal**: Live-Ansicht der seriellen Leitung (52×19 Zeichen quer, 39×29 hoch), gespeist aus dem Replay-Puffer. Escape-Sequenzen werden verworfen, nicht ausgewertet. Tasten: Enter, Ctrl-C, BREAK, REC (nur mit SD-Karte).
- **Skripte**: die in der Weboberfläche (Reiter Konfig) gespeicherten Konfigurationen. Antippen wählt aus, erst „Senden an Port N" spielt ab — zeilenweise, mit Warten auf den Prompt, Stopp bei Fehlermeldung, Steuerzeilen `@pause`, `@expect`, `@break`, `@timeout` wie im Browser (`src/player.cpp`). Konfigurationen mit `{{Variablen}}` gehen nur in der Weboberfläche.
- **Bluetooth**: zeigt die sechsstellige PIN und schaltet Bluetooth ein/aus (wird gespeichert).
- **System**: CPU-Last je Kern, RAM, PSRAM als Balken; dazu freier Speicher, größter Block, längste Pause der Hauptschleife, Zeichenzeit, Chip-Temperatur, Laufzeit. Die CPU-Last ist eine Schätzung aus der Leerlaufzeit (Auflösung 1 ms).
- **Info**: Firmware, Hostname, Clients, Raw-TCP, Akku, SD-Karte, Lage, Board.
- **Setup**: Helligkeit, Abschaltzeit des Displays, WLAN-Client. „Netz waehlen" sucht Netze; ein verschlüsseltes Netz öffnet die Bildschirmtastatur (QWERTZ, drei Ebenen), ein offenes fragt nach. Gespeichert wird wie in der Weboberfläche, mit Neustart. 802.1X bleibt der Weboberfläche vorbehalten.
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
| Port 1 RX / TX | 44 / 43 | UART-Pads, über USB-CDC frei |
| Akkumessung (BAT_ADC, Teiler 3:1) | 5 | am Gerät bestätigt: 1,35 V am Pin bei angeschlossenem LiPo; voreingestellt |

Weitere Header-Pins sind bewusst nicht freigegeben, solange die Belegung nicht aus einer verlässlichen Quelle stammt.

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
- **Die Messung liest vermutlich zu niedrig.** Am USB-Port stand die Spannung minutenlang unbewegt bei 4,05 V — so verhält sich eine volle Zelle an 4,2 V, keine, die noch lädt. Mit dem Multimeter am Akku nachmessen und `BAT_CAL` in `config.h` setzen (echte Spannung / angezeigte Spannung); bis dahin zeigt ein voller Akku etwa 83 %.
- Ohne Akku liegt am Messpunkt die Ausgangsspannung des Ladereglers; das ist von einem vollen Akku nicht zu unterscheiden.
- Die Messung ist auf diesem Board fest eingeschaltet: ein gespeichertes „keine Messung" wird beim Start durch IO5 ersetzt.

## Aufbau der Oberfläche

Die serielle Brücke darf nie auf Pixel warten. Deshalb zwei Tasks:

- **Hauptschleife** (Core 1): besitzt den Zustand der Firmware. Alle 100 ms kopiert sie, was die Seiten zeigen, in einen Schnappschuss aus einfachen Daten, füttert das Terminal-Raster und führt die angetippten Befehle aus.
- **UI-Task** (Core 0): besitzt Panel, Touch und Lagesensor. Er zeichnet nur aus dem Schnappschuss in einen Puffer im PSRAM und schickt **nur die 16-Pixel-Streifen, die sich geändert haben**.

Dazwischen liegen ein Mutex für den Schnappschuss, eine Befehlswarteschlange und ein paar Anforderungs-Flags. Kein `String` wird über die Task-Grenze gelesen. LCD und SD-Karte teilen sich die SPI-Leitungen; wer darauf spricht, hält `Display::busLock()`.

Gemessen: Die Hauptschleife pausiert im Betrieb 2–3 ms; die längste Pause ist das BREAK-Signal (300 ms, wie in der Weboberfläche). Ein kompletter Seitenwechsel braucht bei 20 MHz rund 100 ms, eine Statusänderung nur wenige Streifen. Die Werksdemo des Boards arbeitet nach demselben Muster (LVGL mit Teil-Updates in einem eigenen Task).

## Debug-Konsole: das Display ohne Hinsehen prüfen

Über den USB-Seriell-Port nimmt die Firmware Diagnosebefehle an; `tools/lcd_debug.py` bedient sie, ohne das Board zurückzusetzen:

```bash
tools/lcd_debug.py status                  # Laufzeiten beider Tasks, Touch, SD-Verlauf, Bluetooth
tools/lcd_debug.py shot bild.png           # Screenshot dessen, was das Display zeigt
tools/lcd_debug.py screen 1 tap 54 205     # Seite wählen, Berührung simulieren
tools/lcd_debug.py rot 0 shot hoch.png     # Drehung erzwingen
tools/lcd_debug.py bat                     # Akku: Rohwerte und worauf "laedt"/"voll" beruht
```

Bleibt die Hauptschleife länger als 3 s stehen, meldet der UI-Task das von sich aus im Log (`[DIAG] Hauptschleife steht seit ...`).

## Erfahrungswissen

- **`Serial.setTxTimeoutMs(0)` blockiert, statt nie zu blockieren.** Die Schreibschleife der USB-Seriell-Klasse (Core 2.0.17) zählt den Wert herunter und läuft bei 0 über. Steckt das Board an einem Rechner, der den Port nicht geöffnet hat, bleibt `setup()` in der ersten längeren Log-Ausgabe hängen: kein Hotspot, kein Bluetooth, eingefrorene Anzeige. Das war sehr wahrscheinlich auch das „Einfrieren nach einigen Berührungen" (jede Berührung schrieb eine Logzeile). Jetzt 5 ms. Betrifft alle Boards mit USB-CDC, auch das T-RSS3.
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
- **Erster Build auf dem Raspberry Pi dauert lange** (2,4 GB Pakete auf die SD-Karte). PlatformIO liegt in `/root/.local/share/pio-venv`.

## Das SD-Karten-Problem (offen)

Beobachtet mit einer 1-GB-SDSC-Karte:

- Nach dem **Einschalten** wird die Karte eingebunden.
- Nach einem **Reset ohne Stromunterbrechung** antwortet sie nicht mehr. Auf der Datenleitung liegt dann bei jedem Takt ein festes Muster (ein Null-Bit alle vier Takte), unabhängig von Chip-Select, Taktfrequenz und jedem Kommando (CMD0, CMD12, Stop-Token). 4096 Byte am Stück sind identisch — das sind keine Daten, die Karte hängt.
- Per Messung ausgeschlossen: der ESP32 selbst (IO40 ist kein Ausgang), das LCD (Software-Reset ändert nichts, es liest ohnehin über MOSI), ein unterbrochener Lesevorgang.
- Nur Strom weg holt die Karte zurück.

Beide naheliegenden Erklärungen sind inzwischen widerlegt:

1. *Der LCD-Takt von 40 MHz ist zu schnell für die Karte.* Mit 20 MHz fiel sie genauso aus.
2. *Der Reset bringt sie aus dem Tritt.* Sie fiel auch im laufenden Betrieb aus, ohne jeden Reset: eingeschaltet, eingebunden, nach spätestens 254 s tot (`K+ !254s` im Verlauf).

Was bleibt: Diese Karte verträgt den Datenverkehr des Displays auf den gemeinsamen Leitungen nicht, obwohl ihr Chip-Select dabei inaktiv ist. Ob das an dieser einen (alten) Karte liegt oder am Aufbau, ist offen — **als Nächstes eine andere Karte probieren (SDHC, 8–32 GB)**.

Die Firmware führt Buch (`[DIAG] SD: ... Verlauf:` in `lcd_debug.py status`, im Flash gespeichert): `K+`/`K-xx` Start nach Einschalten, `R+`/`R-xx` Start nach Reset (xx = Antwort der Karte), `!<n>s` Karte fiel nach n Sekunden Betrieb aus (geprüft wird alle 20 s mit einem Sektor-Lesen).

Weitere Regeln, die dabei entstanden sind: Die Karte wird **vor** dem LCD gestartet (`Display::begin()` ruft `Sd::begin()`), und eine nach dem Start eingesteckte Karte lässt sich auf der Info-Seite einbinden.

## Offen

1. SD-Karte: mit einer anderen Karte gegenprüfen (siehe oben).
2. Der Fix für das Blockieren ohne USB-Leser ist aus dem Code der USB-Klasse hergeleitet und im Betrieb ohne geöffneten Port geprüft, aber nicht mit einem echten Aus- und Einstecken.
3. WLAN-Beitritt: Suchen, Tastatur, Nachfrage, Speichern und Wieder-Ausschalten sind geprüft; ein erfolgreicher Verbindungsaufbau mit einem echten Schlüssel nicht.
4. Skripte: Ablauf geprüft, aber ohne angeschlossenes Gerät (jede Zeile wartet dann 2 s auf eine Antwort).
5. Bluetooth Richtung Gerät → Handy mit Loopback (TXD–RXD gebrückt) und mit einer Handy-App prüfen.
6. MAX3232 an IO43/IO44 anschließen, echter Konsolenzugriff.
7. Akku: Spannung mit dem Multimeter gegenmessen (`BAT_CAL`); Ladeerkennung im Akkubetrieb, am Netzteil und beim Ab-/Anstecken prüfen.
8. SD-Mitschnitte sind nur am Display bedienbar, nicht in der Weboberfläche.
9. Die Envs `esp32dev-max3232` und `viewe-5inch` wurden mit den Änderungen nicht neu gebaut.
10. Baudraten-Wechsel am Display gelten bis zum Neustart (wie bei der BOOT-Taste), sie werden nicht gespeichert.
