# Test-RADIUS-Server für 802.1X

Ein FreeRADIUS im Container, um den WLAN-Client der Firmware gegen einen echten RADIUS-Server zu prüfen: EAP-TLS mit Zertifikat, PEAP-MSCHAPv2 und EAP-TTLS. **Nur zum Testen** — ein Benutzer, ein Zertifikat, und das Log (`docker compose logs -f radius`) nennt jeden Schritt einer Anmeldung, auch warum sie abgelehnt wurde.

```bash
cd tests/radius
./setup.sh                 # einmal: eigene Zertifizierungsstelle, Zertifikate, Benutzer, Shared Secret -> data/
docker compose up -d       # UDP 1812 (Anmeldung) und 1813 (Abrechnung) auf diesem Rechner
./test.sh                  # meldet sich an wie ein WLAN-Client (eapol_test) - sechs Fälle
cat data/README.txt        # was wo einzutragen ist, mit Secret und Passwörtern
```

`data/` steht nicht im Repository. `./setup.sh --force` fängt neu an (neue Zertifizierungsstelle — das Gerät braucht dann die neuen Dateien). Liegt das Netz der Access Points nicht in `192.168.10.0/24`: `RADIUS_LAN=10.0.0.0/8 ./setup.sh --force`.

## Access Point

Eine neue SSID mit **WPA2-Enterprise (802.1X, AES)** anlegen und als RADIUS-Server diesen Rechner eintragen: Adresse, Port **1812** (Accounting 1813), das Shared Secret aus `data/README.txt`.

- **Controller (AireOS, Mobility Express):** Security → AAA → RADIUS → Authentication → New; dann WLANs → Create New → Security → Layer 2 „WPA2 + 802.1X“ und unter AAA Servers den Server wählen.
- **Catalyst 9800 / Embedded Wireless Controller:** Configuration → Security → AAA: Server, Servergruppe und eine Methodenliste „dot1x“ anlegen; im WLAN unter Security → Layer 2 „802.1X“ und unter AAA die Methodenliste wählen.
- **Autonomer AP (IOS):** `aaa new-model`, `radius server … address ipv4 <Rechner> auth-port 1812 acct-port 1813`, `key <Secret>`, `aaa authentication login eap_methods group radius`; an der SSID `authentication open eap eap_methods`, `authentication network-eap eap_methods`, `authentication key-management wpa version 2`.

Kommt am Server nichts an (das Log bleibt still): Die Firewall des Rechners muss UDP 1812 aus dem Netz der Access Points durchlassen — Docker tut das für veröffentlichte Ports selbst —, und der Access Point muss den Rechner erreichen.

## Gerät

Weboberfläche, Tab Netz (dazu „Einstellungen für Administratoren zeigen“ im Tab Gerät):

| Verfahren | Eintragen |
|---|---|
| EAP-TLS | Slot „Client“: `data/device/rs232-console.p12` (Passwort in `data/README.txt`) oder `rs232-console.pem`; Slot „CA“: `data/device/radius-ca.pem`; Identität leer |
| PEAP-MSCHAPv2, EAP-TTLS | Benutzer `rs232`, Passwort aus `data/README.txt`; Slot „CA“: `radius-ca.pem` und „RADIUS-Server prüfen“ |

Nach dem Hochladen der Zertifikate neu starten. Der Grund einer Ablehnung steht am Gerät (Tab Gerät, „Grund 23 …“) und genauer im Log des Servers.

## Stand

13. Oktober 2026: Server aufgesetzt, `./test.sh` grün (EAP-TLS, PEAP, TTLS mit MSCHAPv2 und PAP; falsches Passwort und fremdes Zertifikat abgelehnt). Am selben Tag am Waveshare-Board über einen Cisco-Controller (Mobility Express) geprüft: PEAP-MSCHAPv2, EAP-TTLS, EAP-TLS, jeweils mit CA-Prüfung; falsches Passwort und ein Server mit fremder CA abgelehnt. Das Shared Secret ist seitdem von Hand auf einen kurzen Wert gesetzt (`data/secret`).
