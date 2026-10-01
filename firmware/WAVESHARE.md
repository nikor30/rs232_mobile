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
| Mitschnitt auf SD-Karte | `src/sdcard.cpp` | Karte wird nach dem Einschalten erkannt (CS = IO41), **nach jedem Reset ohne Stromunterbrechung nicht mehr** — offen, siehe unten |

## Bedienung

- **Wischen** oder die Pfeile oben wechseln die Seite: Status, Terminal, WLAN (QR), Web-UI (QR), Bluetooth, Info. Die BOOT-Taste blättert ebenfalls.
- **Status**: Schnittstelle, RX/TX-Zähler, Netz. Tasten: Baudrate weiterschalten, Auto-Baud, BREAK.
- **Terminal**: Live-Ansicht der seriellen Leitung (52×19 Zeichen quer, 39×29 hoch), gespeist aus dem Replay-Puffer. Escape-Sequenzen werden verworfen, nicht ausgewertet. Tasten: Enter, Ctrl-C, BREAK, REC (nur mit SD-Karte).
- **Bluetooth**: zeigt die sechsstellige PIN und schaltet Bluetooth ein/aus (wird gespeichert).
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
| Akkumessung | 5 | **nicht bestätigt**, nur wählbar, nicht voreingestellt |

Weitere Header-Pins sind bewusst nicht freigegeben, solange die Belegung nicht aus einer verlässlichen Quelle stammt.

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
```

Bleibt die Hauptschleife länger als 3 s stehen, meldet der UI-Task das von sich aus im Log (`[DIAG] Hauptschleife steht seit ...`).

## Erfahrungswissen

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

Zwei Erklärungen sind übrig, keine ist bewiesen:

1. Der LCD-Takt von 40 MHz lag über dem, was die Karte verträgt (25 MHz), und hat sie im Betrieb aus dem Tritt gebracht. Deshalb steht `LCD_SPI_HZ` jetzt auf 20 MHz.
2. Beim Reset schweben Takt und Chip-Select, und die Karte fängt sich dabei etwas ein.

Die Firmware führt dazu Buch (`[DIAG] SD: ... Verlauf:` in `lcd_debug.py status`, im Flash gespeichert): `K+`/`K-xx` Start nach Einschalten, `R+`/`R-xx` Start nach Reset (xx = Antwort der Karte), `!<n>s` Karte fiel nach n Sekunden Betrieb aus. Fällt sie im Betrieb aus, ist es Erklärung 1; überlebt sie den Betrieb, aber nicht den Reset, Erklärung 2.

Weitere Regeln, die dabei entstanden sind: Die Karte wird **vor** dem LCD gestartet (`Display::begin()` ruft `Sd::begin()`), und eine nach dem Start eingesteckte Karte lässt sich auf der Info-Seite einbinden.

## Offen

1. SD-Karte: nach dem nächsten Einschalten den Verlauf lesen (siehe oben) und die Ursache festnageln.
2. Oberfläche mit echtem Finger prüfen — die Vorversion fror nach einigen Berührungen ein; die Ursache wurde nicht gefunden, die Oberfläche seither neu aufgebaut. `lcd_debug.py status` zeigt, welcher Task steht, falls es wieder passiert.
3. In vier Minuten Test wurde eine Berührung registriert, die niemand gemacht hat. Beobachten.
4. Bluetooth Richtung Gerät → Handy mit Loopback (TXD–RXD gebrückt) und mit einer Handy-App prüfen.
5. MAX3232 an IO43/IO44 anschließen, echter Konsolenzugriff.
6. Akkumessung an IO5 bestätigen.
7. SD-Mitschnitte sind nur am Display bedienbar, nicht in der Weboberfläche.
8. Die Envs `esp32dev-max3232` und `viewe-5inch` wurden mit den Änderungen an `sdcard.cpp`, `settings` und `main.cpp` nicht neu gebaut.
