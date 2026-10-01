# 5-Zoll-Panel: VIEWE UEDX80480050E-WB

Zweite Hardware-Variante neben Mockup und T-RSS3: ein ESP32-S3 mit 800×480-RGB-Touchdisplay, GT911-Touch, SD-Karte, 16 MB Flash und 8 MB PSRAM. Dieselbe Firmware, dieselbe Weboberfläche – zusätzlich eine Touch-Oberfläche direkt am Gerät.

> **Stand: am Gerät noch nicht erprobt.** Die Firmware übersetzt sauber (0 Warnungen im eigenen Code), Pinbelegung und Panel-Treiber stammen aus der Herstellerdokumentation. Alles, was Timing, Touch-Kalibrierung und PSRAM-Bandbreite angeht, lässt sich erst mit dem Board in der Hand beurteilen.

## 1. Die GPIO-Lage, und warum sie das Projekt bestimmt

Das RGB-Panel belegt rund zwanzig Pins, SD-Karte vier, Touch vier. Übrig bleibt:

| GPIO | Lage |
|---|---|
| **17** | frei |
| **18** | frei (Touch-Interrupt ist ab Werk nicht bestückt, erst wenn R28 nach R33 umgelötet wird) |
| 19 / 20 | I²C des GT911-Touch – **mitbenutzbar** |
| 43 / 44 | UART TX/RX, hängt am USB-Seriell-Wandler (Debug-Konsole) |

**Zwei freie GPIOs.** Vier RS232-Ports nativ sind damit ausgeschlossen. Das ist kein Mangel des Boards, sondern die Bestätigung der Architektur: Die geplante I²C-Tochterplatine (2× SC16IS752 + 2× MAX3232) braucht genau zwei Leitungen, und die liegen mit 19/20 schon an. Die SC16IS752 antworten auf 0x48–0x57, der GT911 auf 0x5D bzw. 0x14 – kein Adresskonflikt. Als gemeinsame Interrupt-Leitung bietet sich GPIO18 an.

Die Firmware legt deshalb ab Werk so auf:

| Port | RX | TX | Anmerkung |
|---|---|---|---|
| 1 | IO18 | IO17 | die beiden freien Pins, Hardware-UART |
| 2 | IO44 | IO43 | möglich, kostet aber die USB-Debug-Konsole |
| 3, 4 | – | – | erst mit der Tochterplatine |

## 2. Bauen

```
pio run -e viewe-5inch -t upload
```

Die Umgebung nutzt **Arduino-Core 3.1.1** (ESP-IDF 5.3) über die pioarduino-Platform, weil `ESP32_Display_Panel` Core 3 voraussetzt. Mockup und T-RSS3 bleiben auf Core 2.0.17 – beide Stände übersetzen aus derselben Quelle, siehe Abschnitt 5.

## 3. Die Touch-Oberfläche

```
┌────────────────────────────────────────────────────┐
│ rs232  192.168.4.1     Port 1  9600 8N1   1 Browser│  Statusleiste
├────────────────────────────────────────────────────┤
│ [ 1 Core-SW ] [ 2 Firewall ]                       │  Port-Reiter
├────────────────────────────────────────────────────┤
│ coreswitch#show version                            │
│ Cisco IOS Software, C9300 Software ...             │  Terminal
│ …                                                  │  96 × 17 Zeichen
├────────────────────────────────────────────────────┤
│ Befehl eingeben …                                  │  Eingabezeile
├────────────────────────────────────────────────────┤
│ Tastatur │ Tab │ Ctrl+C │ Esc │ Break │ Auto-Baud │…│  Tasten
└────────────────────────────────────────────────────┘
```

- **Terminal:** 96 Spalten × 17 Zeilen in 8×16-Monospace. CR, LF, Backspace und Tab werden umgesetzt, ANSI-Escapes verworfen – Farben bringen auf 5 Zoll nichts, kosten aber Lesbarkeit.
- **Eingabe:** Bildschirmtastatur, zeilenweise mit Enter abgeschickt. Auf einem Touchscreen ist das praktikabler als zeichenweises Senden; für Sondertasten gibt es die Tastenleiste.
- **Aufz.:** startet und beendet den Mitschnitt auf die SD-Karte.
- **Menü:** Hotspot-Name und -Passwort samt **QR-Code** zum Einscannen, Adressen, SD-Belegung, freier Speicher, Version.
- Die Meldungen, die im Web-UI als Einblendung erscheinen (Auto-Baud, BREAK, Akku), laufen auch über das Panel.

Ports laufen im Hintergrund weiter, auch wenn ihr Reiter nicht offen ist – nur der Bildschirmpuffer zeigt jeweils den aktiven Port. Web-Oberfläche, Raw-TCP/Telnet und Panel sind gleichberechtigt und gleichzeitig nutzbar.

## 4. SD-Karte

Die GUI selbst liegt im Firmware-Image, nicht auf der Karte – LVGL-Oberflächen werden einkompiliert, und der Bildpuffer (800×480×2 = 768 kB) liegt im PSRAM. Die Karte bekommt das, wofür sie taugt:

```
/logs/port1-001.log     Mitschnitte, pro Port und Sitzung durchnummeriert
/configs/*.txt          Konfigurationen zum Abspielen
/xfer/*                 Images für XMODEM/YMODEM
```

Geschrieben wird gepuffert (1 kB oder alle 2 s), damit die Karte den seriellen Datenstrom nicht ausbremst. Ohne Karte läuft alles unverändert weiter, nur die Aufzeichnung fehlt.

## 5. Was die Portierung auf Core 3 gekostet hat

Eine Quelle baut für beide Cores. Die Unterschiede stecken in zwei Kompatibilitätsköpfen:

**`src/compat_eap.h` – 802.1X.** IDF 5 hat `esp_wpa2.h` durch `esp_eap_client.h` ersetzt und alle Funktionen umbenannt (`esp_wifi_sta_wpa2_ent_*` → `esp_eap_client_*`). Der alte Header existiert in IDF 5.3 noch als deprecated Shim, ist aber zur Entfernung angekündigt. Der Code nutzt jetzt die neuen Namen, der Header bildet sie für Core 2 zurück.

**`src/compat_mbedtls.h` – Zertifikate.** mbedTLS 3.x trifft die Zertifikatsverwaltung an mehreren Stellen:

| Bruch | Lösung |
|---|---|
| Strukturfelder privat (`ca_istrue`, `ext_types`, `ecp_keypair.grp`) | Zugriff über `MBEDTLS_PRIVATE()` bzw. `mbedtls_ecp_keypair_get_group_id()` |
| `mbedtls_pk_parse_key` / `pk_check_pair` wollen eine Zufallsquelle | Wrapper mit `esp_fill_random` als RNG-Callback |
| `mbedtls_pkcs5_pbkdf2_hmac` ohne Kontext (`…_ext`) | Wrapper, der auf Core 2 den md-Kontext selbst aufbaut |
| `x509write_csr_set_extension` mit `critical`-Flag | Wrapper, setzt 0 |
| `x509write_crt_set_serial` durch `…_serial_raw` ersetzt | Wrapper, schreibt die MPI als Rohbytes |
| TLS-Version als Enum statt Major/Minor | `rsSslMinTls12()` |

Dazu drei umbenannte Arduino-Funktionen, jeweils mit `ESP_ARDUINO_VERSION_MAJOR` abgefangen: `neopixelWrite` → `rgbLedWrite`, `WiFiServer::available` → `accept`, `UDP::flush` → `clear`.

**Zu testen, sobald das Board da ist:** EAP-TLS gegen einen echten RADIUS und der Zertifikats-Upload (PKCS#12). Der Code übersetzt, aber die mbedTLS-3-Pfade sind noch nie gelaufen.

## 6. Bekannte offene Punkte

- **PSRAM-Bandbreite:** Bildpuffer und WLAN teilen sich den PSRAM. Bei viel Funkverkehr kann das Panel flackern oder reißen. `lvgl_v8_port.h` hat dafür Stellschrauben (Anti-Tearing-Modus, Bounce-Buffer); sinnvoll einstellen lässt sich das erst am Gerät.
- **Touch-Interrupt:** ab Werk nicht verdrahtet, der Treiber pollt. Reicht normalerweise.
- **Statusleuchte:** die WS2812 sitzt auf GPIO0, gemeinsam mit der BOOT-Taste. Funktioniert, ist aber kein sauberer Aufbau – bei Problemen `PIN_LED` in `config.h` auf −1 setzen.
- **Werksreset:** am Panel-Board gibt es keine Bedientaste dafür (`PIN_KEY` ist −1). Reset geht über Web-UI → Setup.
- **`lvgl_v8_port.cpp/h` und `lv_conf.h`** in `src/panel/` stammen aus dem Beispiel des Herstellers (Espressif, CC0-1.0). In `lv_conf.h` sind gegenüber dem Original zwei Schalter gesetzt: `LV_FONT_UNSCII_16` (Terminalschrift) und `LV_USE_QRCODE`.
