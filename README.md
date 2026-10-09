# RS232-WLAN-Konsole

Mobiler serieller Konsolenserver auf ESP32-Basis: mit dem WLAN-Hotspot des Geräts verbinden, `http://192.168.4.1` öffnen, Terminal auf Switch, Router oder Firewall. Keine App, kein Treiber, kein USB-Seriell-Adapter.

Firmware-Stand: **v1.8.0**

- [`HANDOVER.md`](HANDOVER.md) — Projektstand, Erfahrungswissen, offene Punkte
- [`firmware/`](firmware/) — PlatformIO-Projekt für das Waveshare ESP32-S3-Touch-LCD-2 ([Weboberfläche und Netz](firmware/README.md), [Board](firmware/WAVESHARE.md))
- [`tests/`](tests/) — Host-Unittests

```bash
cd firmware
pio run -t upload    # waveshare-s3-lcd2
```
