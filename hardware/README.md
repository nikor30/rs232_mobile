# Hardware: Trägerplatine mit vier galvanisch getrennten RS232-Ports und Gehäuse

Stand 14. November 2026, **Revision 0.4** (Neuanfang auf Grundlage des 3D-Modells des Herstellers).
Entworfen, geroutet, KiCad-DRC ohne elektrische Fehler, Gehäuse rechnerisch gegen das Herstellermodell
geprüft — **nicht gebaut, nicht gedruckt.** Vor der Bestellung die Liste „Vor der Bestellung prüfen“ abarbeiten.

> **Revision 0.3.1 und älter nicht bauen.** Dort waren die beiden Buchsenleisten für das Modul spiegelverkehrt
> belegt: Die Pinbelegung stammte aus einem Foto der Modulrückseite und wurde wie eine Ansicht von oben
> eingetragen. Jedes Signal lag am falschen Pin, VBAT und Masse eingeschlossen. Revision 0.4 nimmt die Lage
> der Stifte aus dem 3D-Modell des Herstellers, und `generate.py` prüft sie bei jedem Lauf dagegen (`HEADER`).

## Was neu ist in Revision 0.4

- **Modul exakt vermessen**: Alle Maße kommen aus dem STEP-Modell von Waveshare (Abschnitt „Das Modul“).
- **Buchsenleisten richtig herum**, Pin 1 liegt 7,60 mm vom antennenseitigen Platinenrand (vorher 7,3 mm geschätzt).
- **TX/RX per Software kreuzen**: Die Lötbrücken sind entfallen. Je Port schalten zwei Halbleiterrelais
  (CPC2330N) die beiden Leitungen an der Buchse um; die Firmware steuert das über GPIO2/4/6/7.
- **Prüfung der Schaltung, Korrekturen**: isolierter Wandler mit 3,3 V statt 5 V Ausgang (der B0505S liefert
  im Leerlauf über 5,5 V, mehr als der MAX3232 verträgt), Ladungspumpen-Kondensatoren 470 nF, Teiler des
  Aufwärtswandlers 270k/36k, BSS138 statt 2N7002 (schaltet mit 3,3 V sicher), TVS-Dioden an jeder Buchse,
  Verpolschutz am Akkuanschluss, zweiter Akkustecker (MX1.25 wie am Modul).
- **Gehäuse neu in echtem CAD** (CadQuery/OpenCascade, STEP und STL): Das Displayglas liegt in einer Tasche
  der Oberseite auf einer umlaufenden Auflage und schließt bündig ab; der Boden rastet mit fünf Federarmen;
  um die USB-C-Buchse liegt eine flache Mulde mit **zwei Tasten für RST und BOOT**. Das alte OpenSCAD-Gehäuse
  ist entfernt (es war zusätzlich seitenverkehrt).

## Das Modul

Waveshare ESP32-S3-Touch-LCD-2. Der Hersteller stellt ein vollständiges 3D-Modell bereit
(`esp32-s3-touch-lcd-2_20241108.stp`, STEP AP203 aus Creo, 314 Körper, von CATIA V5 und 3DEXPERIENCE direkt
lesbar), dazu Maßzeichnung und Schaltplan. Die Dateien liegen **nicht im Repository** (Herkunft und Lizenz
beim Hersteller); `vendor/README.md` nennt die Adressen. `enclosure/case.py` prüft das Gehäuse gegen dieses
Modell, wenn es in `hardware/vendor/` liegt.

Koordinaten des Herstellermodells: x quer, y längs (USB-C bei −y, Antenne bei +y), z nach oben zur
Displayseite; Glasoberseite z = +3,60, Platine z = −3,60 … −2,00.

| Maß | Wert (mm) |
|---|---|
| Deckglas | 37,10 × 58,80 × 1,10, Ecken R 2,60 |
| Platine | 35,02 × 48,22 × 1,60 |
| Glasüberstand über die Platine | 5,08 an der USB-C-Seite, 5,50 an der Antennenseite, 1,04 je Längsseite |
| Stapel | Glas 1,10, Touchfolie 0,40, LCD 2,10, Acrylplatte 2,00, Platine 1,60 |
| Glasoberseite bis Platinenunterseite | 7,20 |
| Glasoberseite bis Unterkante der Stiftleistenkörper | 9,70 |
| Glasoberseite bis Stiftspitzen | 15,50 |
| Stiftleisten | 2 × 14 Stifte, Raster 2,54, Reihen 30,48 auseinander; Pin 1 und Pin 28 an der Antennenseite, 7,60 vom Platinenrand |
| Abstandsbolzen M2 | vier, 29,00 × 42,20 |
| Sichtfläche des Displays | 30,99 × 41,20 (240 × 320 Bildpunkte) |
| USB-C-Buchse | Schale außen 8,94 × 3,16, Öffnung 8,34 × 2,56, Achse 1,67 unter der Platinenunterseite, Vorderkante 1,18 über dem Platinenrand und 3,90 hinter der Glaskante; ein Steckerblech taucht 4,78 ein |
| Taster RST und BOOT | TS24CA, seitlich betätigt, Gehäuse 4,55 × 2,30 × 1,80; Stößel 2,00 × 1,00, 1,20 vor dem Tastergehäuse, Spitze 0,28 hinter dem Platinenrand, Achse 0,90 unter der Platinenunterseite; Stößel 18,92 auseinander (RST bei x = +9,47, BOOT bei x = −9,45) |

Das Maßblatt `enclosure/out/waveshare_module_dimensions.pdf` zeigt alles in drei Ansichten. Alle Zahlen stehen
in `carrier/generate.py` im Wörterbuch `WS`; Platine und Gehäuse lesen sie von dort.

Aus dem Schaltplan des Herstellers: Stift 14 (VBAT) ist das Akkunetz des Laders, am MX1.25-Stecker ist Pin 1
der Pluspol, der Stift „5V“ ist USB-VBUS. **Das Modul hat nur eine Keramikantenne und keine IPEX-Buchse**;
eine externe Antenne ginge nur mit Umbau am Modul. Das Gehäuse hat deshalb keine SMA-Bohrung mehr.

## Verzeichnis

| Pfad | Inhalt |
|---|---|
| `carrier/generate.py` | **Die Quelle.** Maße des Moduls (`WS`), Schaltplan, Platine, DRC-Regeln, `bom.csv`, `preview.png` |
| `carrier/kigen.py` | Schreiber für KiCad-7-Dateien (Symbole und Footprints eingebettet, auch für die Unterseite) |
| `carrier/route.py` | Eigener Gitter-Autorouter (zwei Lagen, Vias, Bauteile auf beiden Seiten, Masseflächen) |
| `carrier/drc.py` | Zonen füllen, KiCad-DRC, Fertigungsdaten nach `kicad/fab/` |
| `carrier/check_pours.py` | Prüft unabhängig vom DRC, dass jede Massefläche zusammenhängt, und nennt den Ort einer abgetrennten Insel |
| `carrier/kicad/` | KiCad-7-Projekt; `channel.kicad_sch` ist ein Port und viermal eingebunden |
| `carrier/kicad/fab/` | **Gerber und Bohrdaten als ZIP**, Bestückungsdaten, Schaltplan- und Platinen-PDF, DRC-Bericht |
| `enclosure/case.py` | Gehäuse: Körper, Boden, Taste; STEP, STL, Prüfungen. Braucht CadQuery |
| `enclosure/render.py` | Bilder (POV-Ray) und die Schnittzeichnung `out/detail_sections.png` |
| `enclosure/module_sheet.py` | Maßblatt des Moduls |
| `enclosure/out/` | `body.step/.stl`, `base.step/.stl`, `key.step/.stl`, `rs232_console_assembly.step`, Bilder |
| `vendor/` | Hinweis, woher das Herstellermodell kommt (die Dateien selbst nicht) |
| `hat/` | **Zwei-Port-Prototyp** „RS232 CONSOLE HAT v3.0“: Fertigungsdaten, `board.py`, Beschreibung des Gehäuses dazu (`hat/README.md`) |
| `enclosure/out_hat/` | Gehäuse für den Zwei-Port-Prototyp (`case.py --hat`): `body`, `base`, `key`, `rest` als STEP und STL, Zusammenbau, Bilder |

Neu erzeugen (KiCad 7, POV-Ray und `pip install cadquery`):

```bash
cd hardware/carrier && python3 generate.py && python3 route.py && python3 drc.py && python3 check_pours.py
cd ../enclosure && python3 case.py && python3 render.py && python3 module_sheet.py
```

`generate.py` ist deterministisch. Änderungen gehören dorthin, nicht in die KiCad-Dateien. Immer in dieser
Reihenfolge laufen lassen und nie zwei Läufe zugleich: `route.py` hängt seine Bahnen an die Platinendatei an.

Der Router ist seit diesem Stand wiederholbar (gleiche Eingabe, gleiches Ergebnis). Drei Masseanschlüsse
bekommen keine eigene Bahn und hängen nur an der Massefläche (`route.py` meldet „left to the pour“). Ob die
Fläche sie wirklich erreicht, entscheidet erst die Füllung in KiCad; `drc.py` und `check_pours.py` prüfen das.
Meldet eines der beiden nach einer Änderung an der Platzierung eine offene Verbindung, hilft ein anderer
Startwert: `ROUTE_SEED=2 python3 route.py`.

## Pinbelegung am Modul

| Port | RX (ESP-Eingang) | TX (ESP-Ausgang) | Versorgung an | Kreuzen |
|---|---|---|---|---|
| 1 | GPIO44 | GPIO21 | GPIO16 | GPIO2 |
| 2 | GPIO15 | GPIO13 | GPIO17 | GPIO4 |
| 3 | GPIO11 | GPIO12 | GPIO18 | GPIO6 |
| 4 | GPIO14 | GPIO9 | GPIO10 | GPIO7 |

GPIO2/4/6/7 gehören am Modul zum Kamerastecker und sind ohne Kamera frei. GPIO43 (U0TXD) bleibt frei, weil
das ROM beim Reset dort schreibt; er liegt mit 3V3, Masse, I²C (GPIO47/48), GPIO8 und VBUS an Stecker J3.
Das Häkchen „RX/TX am Pegelwandler tauschen“ der Firmware darf mit dieser Platine nicht gesetzt werden
(die Optokoppler haben eine feste Richtung).

## Signalweg eines Ports

```
ESP TX ─330Ω/LED─▶ TLP2361 ─▶ T1IN  MAX3232E  T1OUT ─┐ zwei CPC2330N  ┌─▶ RJ45 Pin 3 ─ TVS
ESP RX ◀─100Ω──── TLP2361 ◀─ 74LVC1G17 ◀─ R1OUT  R1IN ◀┘ (umschaltend) └── RJ45 Pin 6 ─ TVS
           3V3-Seite ║ 1,5 kV ║ isolierte 3,3 V (B0503S-1WR3 aus dem geschalteten 5 V)
```

Beide Koppler-LEDs sind in Ruhe (UART-Mark) aus. Der Schmitt-Puffer treibt die RX-LED, weil der
Empfängerausgang des MAX3232 dafür zu schwach ist.

### TX/RX per Software kreuzen

Jedes CPC2330N enthält einen Schließer und einen Öffner. Zwei davon je Port bilden einen zweipoligen
Umschalter für die Buchsenstifte 3 und 6. Die Relais sitzen auf der Unterseite über der Trenngrenze: ihre
LEDs auf der ESP-Seite, die Kontakte auf der Insel (1500 V Isolation laut Datenblatt).

- **Aus** (Pegel low, auch ohne Firmware und bei abgeschaltetem Port): Pin 3 sendet, Pin 6 empfängt. Das ist die
  Belegung für das **blaue Rollover-Kabel** an Cisco-, HP- und Juniper-Konsolen.
- **Ein**: gekreuzt, Pin 6 sendet, Pin 3 empfängt. Das ist die Belegung für ein **gerades Patchkabel**.

Die LEDs hängen an der geschalteten Versorgung des Ports (je Relais zwei LEDs in Reihe über 470 Ω, rund
5 mA, geschaltet von einem BSS138 mit 100 kΩ nach Masse). Ein Port ohne Versorgung ist deshalb immer
ungekreuzt. In der Weboberfläche: Einstellungen des Ports, „TX/RX an der RJ45-Buchse kreuzen“.

Firmware dazu (`config.h`: `PINS_PORT_SWAP`; `serial_bridge.cpp`: `portCross()`, `crossOk()`; Einstellung
`cross` je Port, gespeichert, in `/api/status`, im Befehl `serial` und in den Einstellungen der Leitstelle):
**nicht kompiliert und nicht am Gerät geprüft** (in der Arbeitsumgebung war die PlatformIO-Registry nicht
erreichbar). `tools/i18n_check.py` läuft durch.

## Versorgung

VBAT (Stift 14) → Aufwärtswandler MT3608 auf 5,1 V → je Port ein High-Side-Schalter (BSS138 + AO3401A) →
isolierter Wandler B0503S-1WR3. Standard ist „aus“ (Pull-down). Der Akku steckt auf der Unterseite an J4
(JST-PH 2,0) oder J5 (MX1.25, wie am Modul); Q5 schützt vor Verpolung. Geladen wird über das Modul.

## Gehäuse

Außen **83,8 × 118,6 × 32,3 mm**. Vier gedruckte Teile, keine Schraube:

- **`body`** — Oberseite und alle vier Wände. Das Glas liegt in einer 1,10 mm tiefen Tasche mit 0,25 mm Spiel
  ringsum auf einer Auflage (0,74 mm an den Längsseiten, rund 5 mm an den Stirnseiten, zusammen 423 mm²) und
  ist bündig mit der Oberseite. Der Druck beim Tippen geht ins Gehäuse, nicht in die Stiftleisten. Vier
  Fenster für die RJ45-Buchsen, darüber die Ziffern 1 bis 4 vertieft.
- **`base`** — Bodenplatte, liegt innen in den Wänden. Fünf Federarme (16 × 2 × 5 mm) biegen sich in der
  Plattenebene und rasten mit 0,9 mm Nase in Taschen der Wände; 30° Einführschräge, 45° Haltefläche mit
  0,2 mm Vorspannung. Rechnerisch 1,07 % Dehnung beim Einrasten (PETG verträgt rund 2 %), etwa 28 N zum
  Eindrücken, etwa 49 N zum Abziehen. Zwei Kerben an den Längsseiten zum Aufhebeln. Vier Pfosten klemmen die
  Trägerplatine gegen die Pfosten des Körpers, vier Winkel halten einen Akku bis 40 × 60 × 7 mm.
- **`key`** — zweimal drucken. Tasten für RST und BOOT, siehe unten.

### USB-C-Ende: Stecker und Tasten

Um die Buchse liegt eine 1 mm tiefe Mulde mit 45°-Flanken (28 × 11,6 mm). Darin:

- **Öffnung für den USB-C-Stecker**, 13,4 × 7,5 mm, auf der Achse der Buchse. Die Buchse sitzt wegen des
  Glasüberstands 6,55 mm hinter der Außenfläche; ein Steckerblech ist nur 6,65 mm lang, also muss der
  Kunststoffgriff des Steckers mit hinein. Ganz eingesteckt endet er 4,68 mm unter der Außenfläche. Die
  USB-IF erlaubt Griffe bis 12,35 × 6,5 mm; es bleiben 0,53 mm je Seite und 0,50 mm oben und unten. Dickere
  Stecker (manche gewebeummantelte Kabel) passen nicht.
- **Zwei Tasten**, 3 × 6 mm, links und rechts der Buchse. Blick auf das USB-C-Ende, Display oben:
  **links BOOT, rechts RST**. Jede Taste ist ein flacher Schieber, der in einem Tunnel unter der Deckplatte
  läuft und mit einer Nase unter den Platinenrand bis vor den Stößel des Tasters reicht (0,15 mm Luft). Die
  Kappe steht 0,8 mm über dem Muldenboden und bleibt 0,2 mm unter der Außenfläche, damit in der Tasche nichts
  versehentlich drückt. Schultern am Schieber verhindern, dass er nach außen herausfällt; nach innen hält ihn
  der Taster. Zurück federt der Taster selbst.

Schnitte mit Maßen: `enclosure/out/detail_sections.png`.

### Zusammenbau

1. Beide Tasten von innen in ihre Tunnel schieben, Kappe voran, bis die Schultern an der Wand anliegen.
2. Körper mit der Oberseite nach oben. Modul von oben einsetzen, bis das Glas auf der Auflage liegt. Die
   Tasten dabei ganz außen lassen; rutscht eine nach innen, liegt der Taster auf ihrer Nase auf.
3. Körper umdrehen (Glas auf eine weiche Unterlage), Trägerplatine auf die Stifte drücken. Die Buchsen fahren
   in ihre Fenster.
4. Akku anstecken, Boden eindrücken, bis die fünf Arme einrasten.

Der SD-Slot ist nicht nach außen geführt (USB-Stick-Modus der Firmware oder Boden abnehmen).

### Drucken

PETG oder ABS, nicht PLA (die Federarme). Körper mit der Oberseite nach unten: Stützmaterial nur in der
1,1 mm tiefen Glastasche. Fenster, Stecköffnung und Tastentunnel werden überbrückt (längste Brücke 16,5 mm). Boden flach, die Arme liegen dann in der Schichtebene. Tasten so, wie `key.stl`
liegt (flach auf der Seite, 8,0 × 7,7 × 3,0 mm), ohne Stützen. **Der erste Druck ist eine Probe:** Spiel der
Glastasche (`glass_clr`), Rastnase (`bump`) und Tastenspiel (`key_clr`, `key_gap`) stehen oben in `case.py`.

### Was `case.py` prüft

Boolesche Durchdringung, jeweils 0 mm³ gefordert: Körper gegen Modul (Hüllmodell und Herstellermodell),
Körper und Boden gegen Trägerplatine, Modul beim Einsetzen von oben, Trägerplatine beim Herausziehen, Boden
beim Eindrücken (nur die fünf Nasen dürfen berühren), Tasten in Ruhe und 0,6 mm gedrückt, Tasten beim
Einschieben, Modul beim Einsetzen an den Tasten vorbei, USB-C-Stecker mit dem größten erlaubten Griff
eingesteckt und auf dem Weg hinein. Dazu: Die Tastennase trifft den Stößel des Herstellermodells.

### Variante für den Zwei-Port-Prototyp

`case.py --hat` baut dasselbe Gehäuse um die vorhandene Zwei-Port-Platine (50 × 50 mm, nicht isoliert, per
Draht am Modul): außen 56,8 × 81,0 × 32,8 mm, Oberseite und USB-C-Ende unverändert. Weil das Modul dort in
keiner Buchsenleiste steckt, tragen es Pfosten des Bodens und ein Steg auf der Platine an seinen
M2-Abstandsbolzen. Alles Weitere: `hat/README.md`.

### Akku sichern

Unter beiden Platinen stehen Lötstifte. Den Akku mit doppelseitigem Klebeband auf den Boden kleben, damit er
nicht dagegen wandert.

## Bestellen bei PCBWay

`carrier/kicad/fab/rs232_carrier-gerbers-pcbway.zip`. 2 Lagen, **78 × 110 mm**, Ecken R 3, FR-4 1,6 mm,
1 oz Kupfer, HASL bleifrei oder ENIG, kleinste Bahn 0,25 mm, kleinster Abstand 0,2 mm, Via 0,7/0,3 mm.
**Bestückung auf beiden Seiten** (unten: Relais, TVS-Dioden, vier Widerstände 0805, Akkustecker).
Bestückungsdaten `rs232_carrier-pos.csv`, Stückliste `carrier/bom.csv`.

Isolation: 5 mm kupferfreies Band zwischen ESP-Seite und Inseln (DRC-Regel 3 mm), 1,5 mm zwischen den
Inseln. KiCad-DRC: 0 unverbundene Anschlüsse, 0 elektrische Fehler, 19 Silkscreen-Warnungen (kosmetisch).
`check_pours.py`: alle fünf Masseflächen (GND und vier Inseln) hängen je in einem Stück zusammen.

## Vor der Bestellung prüfen

1. **CPC2330N**: Anschlussbelegung im Datenblatt gegen `channel.kicad_sch` prüfen (angenommen: 1/2 und 3/4
   die beiden LEDs, 5/6 und 7/8 die beiden Kontakte; welcher Kontakt Schließer und welcher Öffner ist,
   entscheidet, was „aus“ bedeutet). Ebenso den LED-Strom (ausgelegt rund 5 mA).
2. **RJ45-Buchse**: `RJ45_Generic` ist ein Platzhalter. Buchse festlegen (etwa Amphenol 54602-908LF) und den
   Footprint in `generate.py` an ihr Datenblatt anpassen. Die Vorderkante liegt bündig mit dem Platinenrand.
3. **B0503S-1WR3** (nicht B0505S): Pins auf 0 / 2,54 / 10,16 / 12,7 mm, Pin 1 = −Vin.
4. **TLP2361**: Pin 1 Anode, 3 Katode, 4 GND, 5 Ausgang, 6 Vcc.
5. **MT3608**: 0,6 V · (1 + 270k/36k) = 5,1 V. Spule L1 mit mindestens 1 A Sättigungsstrom.
6. **Akku**: Polung am Stecker mit dem Multimeter prüfen. Q5 schützt die Platine, nicht den Akku.
7. **Buchsenleisten**: 8,5 mm hoch. Damit liegt die Platinenunterseite des Moduls 11,0 mm über der
   Trägerplatine; das Gehäuse lässt 0,3 mm Luft, damit zuerst das Glas aufliegt.
8. **Taster**: Der Hub des TS24CA war nicht aus einem Datenblatt zu bestätigen (üblich rund 0,25 mm); die
   Taste hat 0,6 mm Weg. Am gedruckten Teil prüfen, dass RST und BOOT sicher schalten und nicht hängen.
9. **USB-C**: Mit den eigenen Kabeln prüfen, ob der Griff in die Öffnung passt.

Nicht gemessen: die Optokoppler-Schaltung (nach Datenblattwerten ausgelegt). Der Autorouter kennt keine
Längenregeln (bei 115 200 Baud belanglos). Maße der USB-C-Stecker nach dem Compliance-Dokument der USB-IF.

## Entscheidungen

- **Trägerplatine statt eigener Hauptplatine**: Die Firmware ist auf dieses Modul eingerichtet.
- **RJ45 statt DB9**: Vier DB9 wären 130 mm breit; mit Rollover-Kabel passt es direkt an Netzwerkgeräte.
- **Optokoppler statt digitaler Isolatoren** (Wunsch), ein Wandler je Port, damit die Ports auch
  untereinander getrennt sind.
- **Halbleiterrelais zum Kreuzen** statt Lötbrücken (Wunsch): Ein Analogschalter auf der Insel hätte eine
  weitere Leitung über die Trenngrenze gebraucht und verträgt keine ±15 V; die Relais trennen selbst.
- **Modul von oben eingesetzt**: Nur so kann das Glas auf einer Auflage liegen und bündig sein.
- **Tasten als lose Schieber** statt Federzungen in der Wand: Eine Zunge müsste beim Druck des Körpers frei
  in der Luft beginnen oder quer zu den Schichten biegen.
- **Kappen unter der Außenfläche**: RST und BOOT sollen in der Werkzeugtasche nicht auslösen.
