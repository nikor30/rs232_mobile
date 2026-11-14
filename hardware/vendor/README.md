# Herstellerdaten des Waveshare ESP32-S3-Touch-LCD-2

Die Dateien des Herstellers liegen nicht im Repository. Für die Prüfung des Gehäuses gegen das Modell
(`enclosure/case.py`) hier ablegen:

| Datei | Woher |
|---|---|
| `esp32-s3-touch-lcd-2_20241108.stp` (3D-Modell, STEP AP203), `ESP32-S3-Touch-LCD-2-20241108.pdf` und `.dwg` (Maßzeichnung) | Archiv `https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2/ESP32-S3-Touch-LCD-2_-20241108.rar` |
| `ESP32-S3-Touch-LCD-2-SchDoc.pdf` (Schaltplan) | `https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2/ESP32-S3-Touch-LCD-2-SchDoc.pdf` |

Beide Adressen stehen auf der Wiki-Seite des Moduls unter „Resources“. Das RAR-Archiv entpackt `bsdtar`
(Paket `libarchive-tools`). Ohne die STEP-Datei läuft `case.py` mit `--no-vendor` und prüft nur gegen das
eigene Hüllmodell.
