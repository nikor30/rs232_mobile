# Hardware: Trägerplatine mit vier galvanisch getrennten RS232-Ports und Gehäuse

Stand 15. November 2026: Platine **Revision 0.4** (Neuanfang auf Grundlage des 3D-Modells des Herstellers),
Gehäuse **Revision 0.5** (neu aufgebaut nach dem ersten Probedruck).
Entworfen, geroutet, KiCad-DRC ohne elektrische Fehler, Gehäuse rechnerisch gegen das Herstellermodell
geprüft — **Platine nicht gebaut, Gehäuse in dieser Fassung nicht gedruckt.** Vor der Bestellung die Liste
„Vor der Bestellung prüfen“ abarbeiten.

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
- **Gehäuse neu in echtem CAD** (CadQuery/OpenCascade, STEP und STL), seit dem ersten Probedruck in
  Revision 0.5: Das Displayglas liegt bündig in einer Öffnung der Oberseite auf einem eigenen Rahmen, in dem
  das Modul geklemmt ist; der Boden rastet mit vier Federarmen; am USB-C-Ende **zwei Tasten für RST und
  BOOT, mitgedruckt als Federzungen des Rahmens**. Drei Teile, nichts Loses, kein Teil braucht Stützen. Das alte OpenSCAD-Gehäuse ist entfernt (es war zusätzlich seitenverkehrt).

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
| `enclosure/case.py` | Gehäuse: Körper, Rahmen (mit den beiden Tasten), Boden; STEP, STL, Prüfungen. Braucht CadQuery |
| `enclosure/printcheck.py` | Prüft die druckfertigen Dateien: Was beginnt über dem Bett in der Luft, und was hält es |
| `enclosure/render.py` | Bilder (POV-Ray) und die Schnittzeichnung `out/detail_sections.png` |
| `enclosure/module_sheet.py` | Maßblatt des Moduls |
| `enclosure/out/` | `body`, `frame`, `base` als STEP und STL, `print/` mit den Teilen in Drucklage, `rs232_console_assembly.step`, Bilder |
| `vendor/` | Hinweis, woher das Herstellermodell kommt (die Dateien selbst nicht) |
| `hat/` | **Zwei-Port-Prototyp** „RS232 CONSOLE HAT v3.0“: Fertigungsdaten, `board.py`, Beschreibung des Gehäuses dazu (`hat/README.md`) |
| `enclosure/out_hat/` | Gehäuse für den Zwei-Port-Prototyp (`case.py --hat`): dieselben Dateien |

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

## Gehäuse (Revision 0.5, nach dem ersten Probedruck)

Der erste Druck des Zwei-Port-Gehäuses (PLA, Bambu Lab, 0,4-mm-Düse) hat fünf Fehler der Konstruktion
gezeigt. Revision 0.5 ist deshalb neu aufgebaut, für beide Gehäuse gleich:

| Im Probedruck | Ursache | Jetzt |
|---|---|---|
| Abbrechrippen in der Glastasche kaum zu entfernen, Auflage rau | Die Auflage begann beim Druck in der Luft | Die Auflage ist ein eigenes Teil (Rahmen), flach gedruckt. Im Körper hängt nichts mehr über |
| Kanten über den Portfenstern hingen durch | Brücken von 16 mm | Die Ports sitzen in einem Schlitz, der zum Rand offen ist; der Boden schließt ihn |
| Zapfen für die Platine abgebrochen | 2,4 mm dünn, stehend gedruckt | Keine Zapfen mehr; die Platine wird geklemmt |
| Modul von nichts gehalten | Es lag nur auf | Zwei Federbacken im Rahmen klemmen es |
| Akkuträger zu klein | falsches Maß | Akku 40 × 30 × 7,8 mm, eigener Platz |
| Tasten als lose Schieber in Tunneln (Löcher, Brücken) | so konstruiert | Tasten sind Teil des Rahmens: Wippe an einer Blattfeder, nur senkrechte Wände |

**Regeln, nach denen jetzt konstruiert ist** (aus Konstruktionsrichtlinien für FDM und aus diesem Druck):
kein Überhang flacher als 45°, keine Brücke über 5 mm (`printcheck.py` lässt höchstens 6 mm zu), nichts
Schlankes steht unter Biegung (keine Zapfen, Pfosten mindestens 5 mm), was federn muss, liegt flach auf dem
Druckbett und biegt sich in der Schichtebene, keine Stützen im Teil und keine vom Slicer.

Außen **83,8 × 118,6 × 33,9 mm** (vier Ports). Drei gedruckte Teile, nichts Loses, keine Schraube, kein Kleber:

- **`body`** — Deckhaut, Seitenwände, Eckstücke. Über dem Display ist die Oberseite nur eine Haut von
  1,2 mm, so dick wie das Glas, mit einer Öffnung in der Größe des Glases (0,25 mm Spiel ringsum): Das Glas
  liegt 0,1 mm unter der Oberseite, praktisch bündig. Beide Stirnseiten sind bis zum Rand offen, sodass im
  Körper nichts überbrückt werden muss. Über den Buchsen die Ziffern der Ports, vertieft.
- **`frame`** — der Rahmen unter der Deckhaut, in ihm ist das Modul geklemmt. Seine Oberseite ist die
  Auflage des Glases (337 mm²; am USB-C-Ende ist er zwischen den Tasten offen). Die Modulplatine steckt in seiner Öffnung: an einer Längsseite ein
  fester Anschlag, am USB-C-Ende einer mit 0,15 mm Spiel, am anderen Ende zwei Quetschrippen, die die
  Platinenkante beim Eindrücken passend schabt; an der anderen Längsseite **zwei Federbacken**
  (19 × 2,6 × 6 mm), die 0,4 mm in den Platz der Platine stehen und sie gegen die feste Seite drücken.
  Gerechnet: 7 N je Backe bei 0,54 % Dehnung; es braucht 9 bis 14 N, um das Modul herauszuschieben. Am
  USB-C-Ende die **beiden Tasten**, aus einem Stück mit dem Rahmen (siehe unten). Zwei Beine stehen auf der
  Platine.
- **`base`** — Bodenplatte innen in den Wänden, vier Federarme (18 × 1,6 × 5 mm) rasten in Taschen der
  Wände (0,65 % Dehnung beim Einrasten, 13 N zum Eindrücken, 22 N zum Abziehen). Der Boden schließt die
  offenen Stirnseiten: eine **Schwelle** unter den Buchsen, eine **Zunge** unter USB-C-Buchse und Tasten.
  Seine Pfosten drücken die Platine gegen die Beine des Rahmens und die Pfosten des Körpers, und über die
  Beine den Rahmen gegen die Deckhaut. Der Akku liegt zwischen vier Winkeln unter der Platine.

Was hält was: Glas auf dem Rahmen, Rahmen unter der Deckhaut, Modulplatine zwischen den Backen und der
festen Seite des Rahmens, Platine zwischen den Pfosten von unten und Beinen und Pfosten von oben, alles
zusammen durch die vier Rastarme des Bodens (0,2 mm Vorspannung nehmen die Toleranzen der Kette auf).

Schnitte mit Maßen: `enclosure/out/detail_sections.png`. Bilder: `enclosure/out/render_*.png`, darunter
`render_clamp.png` (Rahmen mit Modul), `render_frame.png`, `render_keys.png`, `render_keys_module.png` und
`render_keys_top.png` (die Tasten, blau hervorgehoben), `render_exploded.png`, `render_print_bed.png`
(alle Teile in Drucklage). Studiobilder für Präsentationen: `python3 render.py --studio`.

### USB-C-Ende: Stecker und Tasten

Die Stirnwand hat eine Öffnung von 28,4 mm Breite. Darin sitzen links und rechts die beiden Tasten und
dazwischen die Buchse; darunter schließt die Zunge des Bodens.

- **Platz für den USB-C-Stecker**, 14,2 × 7,9 mm, auf der Achse der Buchse. Die Buchse sitzt 6,55 mm hinter
  der Außenfläche; ein Steckerblech ist nur 6,65 mm lang, also muss der Griff des Steckers mit hinein. Die
  USB-IF erlaubt Griffe bis 12,35 × 6,5 mm; es bleiben 0,93 mm zu jeder Taste, 0,90 mm oben, 0,50 mm unten.
- **Zwei Tasten**, je eine Fläche von 6,7 × 6,8 mm. Blick auf das USB-C-Ende, Display oben: **links BOOT,
  rechts RST**. BOOT liegt 0,2 mm unter der Außenfläche und ist mit dem Finger zu drücken; **RST liegt
  1,0 mm tief**, damit es in der Hand und in der Tasche nicht auslöst (Fingernagel oder Stift).
- Die Modulplatine liegt am USB-C-Ende mit 0,15 mm Spiel am Rahmen an. So zieht ein Stecker beim Abziehen
  das Modul nicht gegen die Tasten.

### Die Tasten: mitgedruckt, ohne Loch und ohne loses Teil

Die erste Fassung hatte zwei lose Schieber (3 × 6 mm) in Tunneln des Rahmens: vier Brücken, zwei Kleinteile.
Jetzt ist jede Taste **ein Stück mit dem Rahmen**:

- Der **Kopf** der Taste ist ein Klotz unter dem Glasrand. Außen trägt er die Fläche, auf die man drückt,
  innen eine Nase, die unter den Platinenrand bis 0,2 mm vor den Stößel des Tasters reicht.
- Der Kopf hängt an einer **Blattfeder** (0,84 mm dick, das sind zwei Bahnen der 0,4-mm-Düse; 6 mm hoch;
  14 mm lang, im Zwei-Port-Gehäuse 9,1 mm). Sie läuft an der Stirnkante des Rahmens entlang bis zu ihrer
  Wurzel nahe der Ecke; dahinter ein Schlitz von 1,0 mm, der an der Wurzel rund endet.
- **Druck**: Feder und Kopf stehen in ganzer Höhe auf dem Druckbett. Die Feder biegt sich in der
  Schichtebene, also längs der gelegten Bahnen und nicht quer zu den Schichten. Es gibt nur senkrechte Wände
  und 45°-Flächen; der einzige waagrechte Überhang ist eine Kante von 0,6 mm unter jeder Nase.
- Die Fläche nach außen wächst unter 45° aus dem Kopf. Die Wand des Körpers über der Öffnung ist innen
  ebenso abgeschrägt; so endet die Taste in der Ebene der Wand, ohne dass etwas in der Luft beginnt.
- **Anschlag**: Nach 0,8 mm Weg der Nase trifft die Feder einen Höcker am festen Teil. Die Kante des
  Höckers am Druckbett ist gebrochen, damit die erste Schicht ihn nicht mit der Feder verschweißt.
- Gerechnet (PLA, 3300 MPa): Federrate an der Nase 0,48 N/mm (zwei Ports: 1,25 N/mm). Der Taster schaltet
  nach rund 0,5 mm (0,2 mm Luft und angenommene 0,3 mm Hub): Randdehnung der Feder 0,19 % (zwei Ports
  0,36 %), am Anschlag 0,30 % (0,58 %). Die Feder ist weich gegen den Taster (der bei rund 1,6 N schaltet):
  Ist die Nase 0,3 mm zu lang gedruckt, drückt sie mit 0,14 N (0,38 N) auf den Stößel und löst nicht aus.
- Der Kopf dreht sich beim Drücken um einen Punkt auf der Feder (2,2° bis zum Schalten). Seine Innenseite
  ist um dieses Maß schräg, sie steht am Anschlag parallel zur Platinenkante und 0,15 mm davor.

Warum keine Taste, die erst beim ersten Druck freibricht: Ein freigebrochenes Teil ist lose und braucht
einen Käfig, der es hält, und damit wieder Löcher und Überhänge. Sollbruchstege reißen bei PLA außerdem
nicht verlässlich an der gewollten Stelle. Die Feder bleibt am Teil und braucht beides nicht.

### Zusammenbau

1. Modul von der Glasseite in den Rahmen drücken, Stifte voran, bis das Glas aufliegt. Die Backen weichen
   aus und halten dann die Platine. Die Tasten sind schon am Rahmen.
2. Körper mit der Oberseite nach unten legen. Rahmen mit Modul einsetzen: Das Glas geht in seine Öffnung,
   die Tasten gleiten durch die Öffnung der Stirnwand.
3. Trägerplatine auf die Stifte des Moduls drücken; sie liegt dann auf den Beinen des Rahmens und den
   Pfosten des Körpers.
4. Akku anstecken und zwischen die Winkel des Bodens legen. Boden eindrücken, bis die vier Arme einrasten.

Öffnen: an den beiden Kerben der Längsseiten aufhebeln. Das Modul kommt aus dem Rahmen, wenn man von unten
gegen seine Platine drückt.

### Drucken (PLA, Bambu Lab, 0,4-mm-Düse)

PLA, 0,4-mm-Düse, 0,2 mm Schichthöhe, **Stützen im Slicer aus**. In `enclosure/out/print/` liegen die Teile
so, wie sie auf das Druckbett kommen:

| Datei | Lage auf dem Bett | Größe (mm) |
|---|---|---|
| `body_print.stl` | Oberseite unten | 83,8 × 118,6 × 33,9 |
| `frame_print.stl` | Glasauflage unten, Tasten und Beine nach oben | 79,0 × 64,5 × 17,4 |
| `base_print.stl` | flach, Zunge und Schwelle stehen nach oben | 80,3 × 118,6 × 20,6 |

- Im Slicer nicht neu ausrichten. Drei Wände, 15 % Füllung, Bauteillüfter an.
- Wo die erste Schicht auf dem Bett breitgedrückt wird und es auf das Maß ankommt, ist die Kante gebrochen:
  eine Fase von 0,3 mm um die Glasöffnung, 0,5 mm um den Rahmen. Die Schlitze hinter den Federbacken sind
  0,8 mm breit, die hinter den Blattfedern der Tasten 1,0 mm; schmiert der Slicer sie zu, den
  Elefantenfuß-Ausgleich einschalten. Nach dem Druck jede Taste einmal durchdrücken.
- `python3 printcheck.py` rechnet die Druckbarkeit nach: Es schneidet jedes Teil in senkrechte Säulen
  (0,2-mm-Raster) und prüft für jede Fläche, die über dem Bett in der Luft beginnt, was sie in der Schicht
  darunter hält. Erlaubt sind Brücken bis 6 mm und freie Kanten bis 0,6 mm. Ergebnis: Im Körper bleiben nur
  die vertieften Ziffern, im Rahmen zwei Kanten von 0,6 mm (unter den Nasen der Tasten), im Boden die
  vertiefte Schrift.
- Die Deckhaut über dem Display ist die erste Schicht auf der Druckplatte: Eine strukturierte PEI-Platte
  gibt ihr eine gleichmäßig matte Fläche.
- **PLA wird ab etwa 55 °C weich.** Das Gerät nicht im Auto in der Sonne liegen lassen. PETG geht mit
  denselben Dateien.

**Auch dieser Stand ist nur gerechnet.** Was der nächste Druck zeigen muss, mit dem Maß in `case.py`:
Sitz des Glases in der Öffnung (`glass_clr`), Klemmkraft der Backen (`jaw_bite`), Rastkraft (`bump`),
Gefühl und Weg der Tasten (`key_leaf_t`, `key_leaf_l`, `key_gap`), Passung von Schwelle und Zunge (`cut_clr`).

### Was `case.py` prüft

Boolesche Durchdringung, jeweils 0 mm³ gefordert: Körper, Rahmen, Boden, Modul (Hüllmodell und
Herstellermodell), Platine und Akku paarweise; beim Rahmen gegen das Modul nur der Biss der beiden Backen.
Dazu, dass sich berührt, was halten soll (Glas auf dem Rahmen, Rahmen unter der Deckhaut, Beine und Pfosten
auf der Platine, Platine zwischen den Anschlägen). Montagewege: Modul in den Rahmen, Rahmen in den Körper
(die Tasten gleiten die Öffnung hinauf), Platine, Boden mit Akku. Tasten: in Ruhe, nach 0,5 mm und am
Anschlag (der Kopf um seinen Drehpunkt gedreht) gegen festen Rahmen, Körper, Boden und Modul; die Nase
trifft den Stößel des Herstellermodells; Dehnung und Kraft der Blattfeder. USB-C-Stecker mit dem größten
erlaubten Griff, eingesteckt und auf dem Weg hinein, auch bei gedrückten Tasten.

### Variante für den Zwei-Port-Prototyp

`python3 case.py --hat` baut dasselbe Gehäuse um die vorhandene Platine „RS232 CONSOLE HAT v3.0“ (50 × 50 mm,
Steckbrücken am Modul): außen 56,8 × 88,2 × 35,8 mm. Die Platine liegt unten, das Modul oben, der Akku flach
auf dem Boden hinter der Platine. Beschreibung: `hat/README.md`, Ausgabe in `enclosure/out_hat/`.

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
   Trägerplatine; das Gehäuse lässt 0,3 mm Luft, damit zuerst das Glas auf dem Rahmen aufliegt.
8. **Taster**: Hub und Schaltkraft des TS24CA waren nicht aus einem Datenblatt zu bestätigen (üblich rund
   0,25 mm und 1,6 N); die Nase hat 0,2 mm Luft und 0,8 mm Weg bis zum Anschlag. Am gedruckten Teil
   prüfen, dass RST und BOOT sicher schalten, von selbst zurückkommen und in Ruhe nicht gedrückt sind.
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
- **Glas bündig** (Wunsch), nicht hinter einem Rahmen. Über dem Glas kann deshalb nichts halten; das Modul
  wird an den Kanten seiner Platine geklemmt. Am Modul gibt es keine Fläche, hinter die ein Haken greifen
  könnte (Platine, Abstandshalter und Anzeige sind gleich breit).
- **Rahmen als eigenes Teil**: Die Auflage des Glases wäre im Körper eine Fläche, die beim Druck in der
  Luft beginnt. Als eigenes Teil liegt sie beim Druck auf dem Bett.
- **Offene Stirnseiten, vom Boden geschlossen**: So muss kein Fenster überbrückt werden.
- **Tasten als Federzungen des Rahmens** (Wunsch: mitgedruckt, ohne Löcher und Überhänge), nicht als
  Zungen in der Wand des Körpers: Dort müsste eine Zunge quer zu den Schichten biegen oder ihr Schlitz
  überbrückt werden. Im Rahmen liegt die Feder in der Schichtebene und steht auf dem Druckbett.
- **Tasten unter der Außenfläche**, RST tiefer als BOOT: Sie sollen in der Werkzeugtasche nicht auslösen.
