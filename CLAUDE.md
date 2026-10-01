# RS232-WLAN-Konsole (rs232_mobile)

Projektgedächtnis für Claude-Sitzungen. Die ausführliche Fassung ist `HANDOVER.md` — zuerst lesen.

## Was das ist

Mobiler serieller Konsolenserver auf ESP32: WLAN-Hotspot, `http://192.168.4.1`, xterm.js-Terminal auf Switch/Router/Firewall. Bis zu vier Ports, Raw-TCP/Telnet, XMODEM/YMODEM, 802.1X, HTTPS. Firmware-Stand **v1.7.1** (1. Oktober 2026).

## Aufbau

- `firmware/` — PlatformIO-Projekt (`src/`, `web/`, fertige Images in `firmware/firmware/*.bin`)
- `hardware/pcb/` — KiCad-Tochterplatine + Generator-Skripte; `hardware/gehaeuse/` — OpenSCAD/STL
- `diagramme/` — PNGs + matplotlib-Quellen
- `tests/` — Host-Unittests (g++, laufen auf dem PC), Anleitung in `tests/README.md`
- Doku: `firmware/MOCKUP.md` (meistgebraucht), `firmware/WAVESHARE.md`, `firmware/PANEL.md`, `firmware/README.md`

## Bauen

```bash
cd firmware
pio run -e esp32dev-max3232 -t upload    # Mockup, Arduino-Core 2.0.17
pio run -e t-rss3                        # LilyGO T-RSS3, Core 2.0.17
pio run -e viewe-5inch -t upload         # 5"-Panel, Core 3.1.1 (pioarduino)
pio run -e waveshare-s3-lcd2 -t upload   # Waveshare ESP32-S3-Touch-LCD-2, Core 2.0.17
```

Auf dem Raspberry Pi liegt PlatformIO in `/root/.local/share/pio-venv/bin/pio`; das Waveshare-Board hängt dort an `/dev/ttyACM0`.

`src/web_assets.h` wird vor jedem Build von `tools/embed_web.py` aus `web/` erzeugt — nicht von Hand ändern. Hardwareprofile stehen in `src/config.h`.

## Verifikationsstand

- Am Gerät erprobt: das Mockup (ESP32 DevKit + MAX3232).
- Waveshare ESP32-S3-Touch-LCD-2 (seit 1. Oktober 2026): Boot, Hotspot, LCD, Touch, Lagesensor und Bluetooth-Kopplung am Gerät geprüft; Oberfläche per Screenshot und simulierten Taps getestet; SD-Karte fällt nach jedem Reset aus (offen); serieller Port ohne MAX3232. Details und offene Punkte: `firmware/WAVESHARE.md`.
- Das Waveshare-Display lässt sich ohne Hinsehen prüfen: `firmware/tools/lcd_debug.py shot bild.png`, `status`, `tap X Y` (Screenshot ansehen statt raten).
- Übersetzt, nie gelaufen: alles auf dem VIEWE-Panel, mbedTLS-3-Pfade (802.1X/PKCS#12 auf Core 3), LBO-Auswertung.
- Nicht gebaut: I²C-Tochterplatine; SC16IS752-Treiber fehlt in der Firmware.

## Regeln aus Erfahrung (Details: HANDOVER.md §8)

- RX/TX-Häkchen tauscht nur ESP32-GPIOs am MAX3232, es ersetzt kein Nullmodem. Kein automatischer Tausch im Auto-Baud.
- Diagnoseausgaben müssen den tatsächlichen Zustand nennen (Boot-Log, `[TCP]`-Zeilen).
- Rückgabewert von `WiFiClient::write()` nie ignorieren; TCP läuft über Sendewarteschlange mit `MSG_DONTWAIT`.
- Telnet/Raw wird am ersten Byte (`0xFF`) unterschieden; Raw bleibt byte-genau.
- VIEWE-Panel: nur IO17/IO18 frei, I²C auf IO19/20 mitbenutzbar.
- PowerBoost 1000C: nur 1 Zelle, 5Vo an VIN, nie an 3V3.
- Pinbelegungen aus KiCad-Symbolbibliothek, nicht aus Datenblatt-PDFs.
- Byte-Logik erst auf dem Host testen, dann auf den Mikrocontroller.

## Offene Punkte (Priorität)

1. VIEWE-Panel in Betrieb nehmen (Flackern/Tearing, Touch, Terminal-Geometrie 96×17)
2. 802.1X auf Core 3 gegen echten RADIUS, danach PKCS#12 und CSR
3. LBO-Leitung löten, im Web-UI auf D23 setzen
4. Tochterplatine: Stückliste prüfen, fertigen, SC16IS752-Treiber schreiben
5. Gehäuse für die VIEWE-Variante neu auslegen
6. Docs-Artifact `2f26e902-4f1e-4797-bfbc-4037495d8121` auf VIEWE umschreiben
