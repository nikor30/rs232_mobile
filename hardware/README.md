# Hardware: Trägerplatine mit vier galvanisch getrennten RS232-Ports und Gehäuse

Stand 27. Oktober 2026, Revision 0.1 — **entworfen und per KiCad-DRC geprüft, noch nicht gebaut.**
Vor der Bestellung die Liste unter „Vor der Bestellung prüfen“ abarbeiten.

## Was es ist

Eine Trägerplatine (78 × 95 mm, zweilagig), auf die das **Waveshare ESP32-S3-Touch-LCD-2** mit seinen
beiden 14-poligen Stiftleisten gesteckt wird. Das Modul bringt Display, Touch, Lagesensor (QMI8658),
SD-Slot, LiPo-Lader und USB-C mit; die Trägerplatine ergänzt:

- **vier RS232-Ports auf RJ45** (Cisco-Konsolenbelegung: Pin 3 TX, Pin 6 RX, Pin 4/5 GND), jeder Port
  galvanisch getrennt — zwei Optokoppler **TLP2361** (10 MBd, einer je Richtung), ein **MAX3232E** und ein
  eigener isolierter 1-W-Wandler **B0505S-1WR3** (1,5 kV). Die vier Inseln sind auch voneinander getrennt.
- **Versorgung der Ports aus dem Akku**: VBAT → Aufwärtswandler MT3608 (5,1 V) → je Port ein High-Side-Schalter
  (2N7002 + AO3401), den die Firmware über GPIO16/17/18/10 schaltet. Ein abgeschalteter Port zieht nichts;
  ein eingeschalteter Port kostet rund 30–40 mA am Akku (Wandler im Leerlauf + MAX3232 + LED).
- **Lötbrücken** je Port, um TX/RX an der Buchse zu tauschen (JPx01/JPx02), und je Port eine Lötbrücke,
  ob der Port ohne Firmware-Unterstützung dauerhaft versorgt wird (JP11–JP14, Brücke 2-3 = immer an).
- Akku liegt **unter** der Trägerplatine im Gehäuse, Kabel durch einen Schlitz zum MX1.25-Anschluss des Moduls.
- Stecker J3: 3V3, GND, I²C (GPIO47/48) und GPIO2/4/6 für spätere Erweiterungen.

Der Lagesensor für die automatische Drehung sitzt auf dem Modul; auf der Trägerplatine ist dafür nichts nötig.

### Externe Antenne

Das Modul hat eine Keramikantenne und — nach den Produktseiten der Schwesterboards — vermutlich eine
IPEX-Buchse mit einem 0-Ω-Umschaltwiderstand; für das 2″-Modul konnte das nicht nachgelesen werden (die
Waveshare-Seiten waren aus der Sandbox nicht erreichbar). Das Gehäuse hat links eine Bohrung (6,5 mm) für eine
RP-SMA-Einbaubuchse; ein U.FL/IPEX-Pigtail von dort zum Modul. **Am Modul nachsehen**, ob eine IPEX-Buchse
bestückt ist; wenn nicht, bleibt nur die Keramikantenne (das Gehäuse setzt sie an die linke, metallfreie Seite,
rund 50 mm von den RJ45-Buchsen entfernt).

## Verzeichnis

| Pfad | Inhalt |
|---|---|
| `carrier/generate.py` | **Die Quelle.** Erzeugt Schaltplan, Platine, Projekt, DRC-Regeln, `bom.csv` und `preview.png`. Alle Maße stehen hier (`VERIFY`, Platzierungen). |
| `carrier/kigen.py` | Schreiber für KiCad-7-Dateien (Symbole, Footprints, eingebettet — keine Bibliothek nötig) |
| `carrier/route.py` | Eigener Gitter-Autorouter (zwei Lagen, Vias, netzklassen-abhängige Abstände, Rip-up) |
| `carrier/drc.py` | Zonen füllen, KiCad-DRC, Fertigungsdaten exportieren (`kicad/fab/`) |
| `carrier/kicad/` | KiCad-7-Projekt: `rs232_carrier.kicad_pro/.kicad_sch/.kicad_pcb`, `channel.kicad_sch` (ein Port, viermal eingebunden), `.kicad_dru` |
| `carrier/kicad/fab/` | **Gerber + Bohrdaten als ZIP für PCBWay**, Bestückungsdaten (`-pos.csv`), Schaltplan-PDF, Platinen-PDF, DRC-Bericht |
| `carrier/bom.csv` | Stückliste mit Vorschlägen für Bauteile |
| `enclosure/generate_enclosure.py` | Erzeugt `rs232_console_case.scad` aus denselben Maßen und rendert die STL-Dateien |
| `enclosure/tub.stl`, `bezel.stl` | Unterschale und Blende zum Drucken; `preview_*.png` zur Ansicht |

Neu erzeugen (KiCad 7 und OpenSCAD müssen installiert sein):

```bash
cd hardware/carrier && python3 generate.py && python3 route.py && python3 drc.py
cd ../enclosure && python3 generate_enclosure.py
```

`generate.py` ist deterministisch (feste UUIDs); Änderungen an Maßen oder Bauteilen dort eintragen, nicht in
den KiCad-Dateien — die werden überschrieben. Wer lieber in KiCad weiterarbeitet, nimmt die erzeugten Dateien
als Ausgangspunkt und lässt den Generator danach in Ruhe.

## Pinbelegung am Modul

| Port | RX (ESP-Eingang) | TX (ESP-Ausgang) | Versorgung an |
|---|---|---|---|
| 1 | GPIO44 | GPIO21 | GPIO16 |
| 2 | GPIO15 | GPIO13 | GPIO17 |
| 3 | GPIO11 | GPIO12 | GPIO18 |
| 4 | GPIO14 | GPIO9 | GPIO10 |

Port 1 behält die bisherige Belegung (RX 44 / TX 21). GPIO43 (U0TXD) bleibt frei, weil das ROM beim Reset dort
schreibt. Die TX-LED des Optokopplers liegt zwischen 3V3 und dem GPIO (Katode am GPIO): während des Resets ist
der Pin hochohmig, die LED aus, die RS232-Leitung in Ruhe — **beim Reset erscheint nichts auf dem Sendepin.**
Das Häkchen „RX/TX tauschen“ der Firmware darf mit dieser Platine nicht gesetzt werden (die Optokoppler haben
eine feste Richtung; 100 Ω in der RX-Leitung begrenzen den Strom, falls es doch passiert).

Firmware (geändert in diesem Stand, **nicht kompiliert und nicht am Gerät geprüft**):
`config.h` (Pin-Freigabe, `PINS_PORT_EN`), `serial_bridge.cpp/.h` (`portPower()`, `allPortsOff()`),
`lcd_ui.cpp` (Versorgungen vor dem Tiefschlaf aus und gehalten, nach dem Aufwachen freigegeben).
Ein aktivierter Port bekommt beim Start seine Versorgung, deaktivierte Ports bleiben stromlos.

## Signalweg eines Ports

```
ESP GPIO (TX) ─330Ω/LED─▶ TLP2361 ─▶ T1IN  MAX3232E  T1OUT ─JP01─▶ RJ45 Pin 3
ESP GPIO (RX) ◀─100Ω──── TLP2361 ◀─ 74LVC1G17 ◀─ R1OUT   R1IN ◀─JP02─ RJ45 Pin 6
                 3V3-Seite  ║ 1,5 kV ║  ISO5V-Seite (B0505S aus dem geschalteten 5 V)
```

Beide LEDs sind in Ruhe (UART-Mark) aus, also kein Ruhestrom durch die Koppler. Der Schmitt-Puffer treibt die
RX-LED, weil der Empfängerausgang des MAX3232 dafür zu schwach ist.

## Mechanik

- Modul in Querlage, 11 mm über der Trägerplatine (Buchsenleisten 8,5 mm + Stiftleistenkörper 2,5 mm),
  USB-C zeigt nach rechts, SD-Slot nach oben, Keramikantenne nach links. Vier M2-Abstandsbolzen (11 mm) an den
  Bohrungen des Moduls.
- Trägerplatine auf acht Stützen der Unterschale (4 × M2,5 an H5–H8 mit 14-mm-Distanzbolzen bis zur Blende,
  4 × M2 an den Modulbohrungen). Schrauben von unten (M2,5 × 16 bzw. M2 × 16), Blende von oben
  (M2,5 × 6 in die Distanzbolzen, M2 × 8 in zwei Wandaugen).
- Gehäuse außen **87 × 101 × 28 mm**: Unterschale (`tub.stl`, Wände 2,5 mm, Seitenwände 4 mm, Akkufach
  60 × 55 × 6,5 mm unter der Platine), Blende (`bezel.stl`, 2,1 mm, Displayglas liegt bündig im Fenster).
  Öffnungen: 4 × RJ45 vorn, USB-C-Mulde und zwei Nadellöcher (BOOT/RST) rechts, SD-Schlitz oben, SMA links.
  Drucken: Unterschale mit dem Boden nach unten ohne Stützen, Blende mit der Oberseite nach unten.
- Isolationsabstand auf der Platine: 5 mm kupferfreies Band zwischen ESP-Seite und Inseln (DRC-Regel 3 mm),
  1,8 mm zwischen den Inseln (DRC-Regel 1,5 mm). Unter den Metall-Distanzbolzen ist das Kupfer ausgespart.

## Bestellen bei PCBWay

Datei `carrier/kicad/fab/rs232_carrier-gerbers-pcbway.zip` hochladen. Einstellungen: 2 Lagen, 78 × 95 mm,
FR-4 1,6 mm, 1 oz Kupfer, Lötstopplack beliebig, HASL bleifrei oder ENIG, kleinste Leiterbahn/Abstand
0,25/0,2 mm (Mindestwerte der Platine: 0,25 mm Bahn, 0,2 mm Abstand, Via 0,7/0,3 mm). Keine besonderen
Anforderungen. Für Bestückung durch PCBWay: `rs232_carrier-pos.csv` und `bom.csv` (Bauteile dort ergänzen).

## Vor der Bestellung prüfen

Die Maße des Waveshare-Moduls stammen aus den Produktfotos (`esp32-s3-ports.jpg`, Maßstab über den
2,54-mm-Rasterabstand der Stiftleisten), nicht aus einer Zeichnung — die Waveshare-Seiten waren aus der Sandbox
nicht erreichbar. Alles steht in `carrier/generate.py` unter `VERIFY`; nach der Korrektur die drei Skripte
neu laufen lassen.

1. **Abstand der beiden Stiftleisten** des Moduls (angenommen 29,8 mm, Mitte zu Mitte) und die Lage von
   Pin 1 (7,24 mm vom Rand). Ein Fehler von mehr als 0,3 mm und das Modul passt nicht in die Buchsen.
2. **Außenmaße des Moduls** (34,2 × 47,4 mm), **Bohrungen** (2,2 mm, 2,8/2,7 mm von den Ecken) und das
   **Glas** (35,5 × 56,5 mm, 4,6 mm Überstand an beiden kurzen Seiten) — bestimmen Gehäusefenster und
   Standorte der M2-Bolzen.
3. **Stapelhöhe** 11 mm (Buchsenleiste 8,5 mm + Stiftleistenkörper 2,5 mm) und Glasoberkante 3,5 mm über
   der Modulplatine. Bestimmt die Höhe der Distanzbolzen und die Blende.
4. **RJ45-Buchse**: Der Footprint `RJ45_Generic` ist ein Platzhalter (Pins 1,27 mm Raster, zwei Reihen 2,54 mm,
   Zapfen 3,25 mm im Abstand 11,43 mm). Vor der Bestellung die Buchse festlegen (z. B. Amphenol 54602-908LF
   oder RJHSE-5380) und den Footprint in `generate.py` an deren Datenblatt anpassen — oder in KiCad den
   Bibliotheks-Footprint zuweisen. Pin 1 ist links, wenn man von vorn in die Buchse sieht, die Rastnase unten.
5. **B0505S-1WR3**: Pins auf 0 / 2,54 / 10,16 / 12,7 mm, Pin 1 = −Vin. Im Datenblatt gegenprüfen.
6. **TLP2361**: Pin 1 Anode, 3 Katode, 4 GND, 5 Ausgang, 6 Vcc (Pin 2 fehlt). Ebenso für ACPL-M61L.
   LED-Vorwiderstände (330 Ω / 680 Ω ≈ 5 mA) an den gewählten Typ anpassen.
7. **MT3608**: Vout = 0,6 V · (1 + 75k/10k) = 5,1 V — innerhalb der 4,5–5,5 V des B0505S.
8. **Spule** L1: Footprint ist ein generisches 5 × 5 mm mit zwei Pads; Bauteil mit ≥ 1 A Sättigungsstrom.
9. **Antenne**: IPEX-Buchse am Modul vorhanden? Sonst SMA-Bohrung verschließen.

Was sonst noch offen ist: Die Firmwareänderung ist ungetestet (kein PlatformIO in der Sandbox); die
Optokoppler-Schaltung ist nach Datenblattwerten ausgelegt, nicht gemessen; der Autorouter hat keine
Längen- oder Impedanzregeln (bei 115 200 Baud belanglos). Die Silkscreen-Warnungen des DRC (Texte
überlappen Bauteilumrisse) sind kosmetisch.

## Entscheidungen

- **Trägerplatine statt eigener Hauptplatine**: Die Firmware ist auf dieses Modul (Display, Touch, SD,
  Lagesensor, Lader) eingerichtet; ein Nachbau dieser Funktionen auf einer eigenen Platine wäre ein
  Vielfaches an Aufwand ohne Nutzen. Nachteil: die Antenne bleibt die des Moduls.
- **RJ45 statt DB9**: Vier DB9 wären 130 mm breit. Mit der Cisco-Belegung passt das vorhandene blaue
  Rollover-Kabel direkt an Cisco/HP/Juniper-Konsolen; an DB9-Geräte kommt man mit dem üblichen
  RJ45-DB9-Adapter des Kabelsatzes. Wer eine andere Belegung braucht, lötet JPx01/JPx02 um.
- **Optokoppler statt digitaler Isolatoren** (Wunsch): TLP2361 ist mit 10 MBd und 2,7–5,5 V Versorgung
  für beide Seiten geeignet; ein ADuM1201 wäre kleiner und ohne LED-Strom, aber eine andere Schaltung.
- **Ein Wandler je Port** statt einem gemeinsamen: nur so sind die Ports auch untereinander getrennt
  (zwei Geräte mit verschiedenem Erdpotential gleichzeitig).
- **Schaltbare Versorgung**: Die vier Wandler zögen sonst dauerhaft 30–60 mA; am 1000-mAh-Akku wären das
  Stunden. Standard ist „aus“ (Pull-down), die Firmware schaltet ein.
- **Abstände**: 19 mm Portraster, weil 17,5/18 mm für Ladungspumpen-Kondensatoren, MAX3232 und Wandler in
  einer Spalte nicht reichten — die Platine wurde dafür von 72 auf 78 mm breiter und von 90 auf 95 mm länger.
