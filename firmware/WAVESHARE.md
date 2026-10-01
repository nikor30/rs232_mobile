# Mockup 2: Waveshare ESP32-S3-Touch-LCD-2

ESP32-S3R8 (16 MB Flash, 8 MB PSRAM) mit 2″-Farbdisplay 240×320 (ST7789T3), kapazitivem Touch (CST816D), Lagesensor (QMI8658), SD-Slot, LiPo-Anschluss und USB-C am nativen USB-Port.

```bash
pio run -e waveshare-s3-lcd2 -t upload
```

Stand 1. Oktober 2026: gebaut, geflasht und am Board geprüft, soweit unten angegeben. Core 2.0.17 wie das DevKit-Mockup.

## Was das Board zusätzlich kann

| Funktion | Datei | Geprüft |
|---|---|---|
| Farb-Touch-Oberfläche mit sechs Seiten | `src/lcd_ui.cpp` | Bild und Touch laufen; die neue Oberfläche selbst hat noch niemand angesehen |
| Automatisches Drehen über den Lagesensor | `src/lcd_ui.cpp` | Sensor liefert Werte; Zuordnung Achse → Drehung nur für eine Lage bestätigt |
| Serielle Konsole über Bluetooth LE | `src/ble.cpp` | Kopplung mit PIN, Verschlüsselung und Schreiben vom Raspberry Pi aus; Richtung Gerät → Handy ungetestet (kein Loopback verdrahtet) |
| Mitschnitt auf SD-Karte | `src/sdcard.cpp` | Karte wurde einmal erkannt (CS = IO41), danach nicht mehr — offen |

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
| SD CS (SPI-Leitungen des LCD) | 41 | einmal am Gerät bestätigt |
| Port 1 RX / TX | 44 / 43 | UART-Pads, über USB-CDC frei |
| Akkumessung | 5 | **nicht bestätigt**, nur wählbar, nicht voreingestellt |

Weitere Header-Pins sind bewusst nicht freigegeben, solange die Belegung nicht aus einer verlässlichen Quelle stammt.

## Erfahrungswissen

- **Der QMI8658 liefert im Nur-Beschleunigungs-Modus eingefrorene Werte.** Erst nach Soft-Reset und mit eingeschaltetem Gyroskop (CTRL7 = 0x03) laufen die Daten. CTRL1 = 0x60 schaltet zusätzlich auf Big-Endian — hier 0x40.
- **Das Panel antwortet nicht auf MISO.** Ein Auslesen der Panel-ID liefert FFFFFF und beweist nichts.
- **SD und LCD teilen sich SPI2.** LovyanGFX öffnet dafür das globale Arduino-`SPI`; die SD-Bibliothek muss dasselbe Objekt benutzen, kein eigenes `SPIClass(HSPI)`.
- **Die Reihenfolge beim BLE-Advertising zählt.** Erst `enableScanResponse(true)`, dann Name setzen — sonst passt der Name nicht neben die 128-Bit-UUID.
- **`onAuthenticationComplete` kommt bei einem bereits gekoppelten Gerät nicht verlässlich.** Der Sicherheitszustand wird deshalb in `loop()` von der Verbindung gelesen.
- **Erster Build auf dem Raspberry Pi dauert lange** (2,4 GB Pakete auf die SD-Karte). PlatformIO liegt in `/root/.local/share/pio-venv`.

## Offen

1. Neue Oberfläche ansehen: Lesbarkeit, Aufteilung, Drehrichtung in den beiden Hochformat-Lagen.
2. SD-Karte: warum sie nach dem ersten Start nicht mehr erkannt wird.
3. Bluetooth Richtung Gerät → Handy mit Loopback (TXD–RXD gebrückt) und mit einer Handy-App prüfen.
4. MAX3232 an IO43/IO44 anschließen, echter Konsolenzugriff.
5. Akkumessung an IO5 bestätigen.
6. SD-Mitschnitte sind nur am Display bedienbar, nicht in der Weboberfläche.
