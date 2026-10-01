# RS232-WLAN-Konsole

Mobiler serieller Konsolenserver auf ESP32-Basis: mit dem WLAN-Hotspot des Geräts verbinden, `http://192.168.4.1` öffnen, Terminal auf Switch, Router oder Firewall. Keine App, kein Treiber, kein USB-Seriell-Adapter.

Firmware-Stand: **v1.7.1**

- [`HANDOVER.md`](HANDOVER.md) — Projektstand, Erfahrungswissen, offene Punkte
- [`firmware/`](firmware/) — PlatformIO-Projekt ([Mockup](firmware/MOCKUP.md), [Waveshare 2″](firmware/WAVESHARE.md), [5″-Panel](firmware/PANEL.md), [T-RSS3](firmware/README.md))
- [`hardware/`](hardware/) — KiCad-Tochterplatine und Gehäuse
- [`diagramme/`](diagramme/) — Verdrahtungs- und Schaltpläne
- [`tests/`](tests/) — Host-Unittests

```bash
cd firmware
pio run -e esp32dev-max3232 -t upload    # Mockup
pio run -e viewe-5inch -t upload         # 5"-Panel
```
