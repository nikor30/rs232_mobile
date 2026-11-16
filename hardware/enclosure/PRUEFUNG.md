# Prüfanleitung für die Gehäuse (CATIA)

Für alle, die die beiden Gehäuse in einem CAD-System nachprüfen. Stand: Gehäuse-Revision 0.5, 15. November 2026.
Die erste Fassung des Zwei-Port-Gehäuses ist gedruckt worden; **diese Fassung ist danach neu aufgebaut und
noch nicht gedruckt.** Alle Maße in Millimetern.

## Was der Probedruck gezeigt hat

| Im Probedruck (PLA, Bambu Lab, 0,4-mm-Düse) | Jetzt |
|---|---|
| Abbrechrippen in der Glastasche kaum zu entfernen, Auflage rau | Auflage ist ein eigenes Teil (Rahmen), flach gedruckt |
| Kanten über den Portfenstern hingen durch (Brücken von 16 mm) | Ports in einem Schlitz, der zum Rand offen ist |
| Zapfen für die Platine (Ø 2,4, stehend gedruckt) abgebrochen | Keine Zapfen; Platine geklemmt |
| Modul von nichts gehalten | Zwei Federbacken im Rahmen klemmen es |
| Akkuträger zu klein | Akku 40 × 30 × 7,8 mm auf dem Boden |
| (nach dem Druck gewünscht) Tasten ohne lose Teile und ohne Löcher | Tasten sind Federzungen des Rahmens |

## Dateien

STEP AP214 (`AUTOMOTIVE_DESIGN`), geschrieben von Open CASCADE 7.9 über CadQuery. Jedes Einzelteil trägt einen
eigenen Teilenamen, damit sich mehrere Dateien in einer CATIA-Sitzung öffnen lassen.

| Datei | Teilename | Inhalt | Lage |
|---|---|---|---|
| `out/body.step` | `rs232_4port_body` | Körper: Deckhaut, Seitenwände, Eckstücke | eingebaut |
| `out/frame.step` | `rs232_4port_frame` | Rahmen: Glasauflage, Federbacken, die beiden Tasten an ihren Blattfedern, Beine | eingebaut |
| `out/base.step` | `rs232_4port_base` | Boden: Rastarme, Pfosten, Schwelle, Zunge | eingebaut |
| `out/rs232_console_assembly.step` | `rs232_console` | alles zusammen, mit Modul, Platine und Akku | eingebaut |
| `out/waveshare_module_envelope.step` | | Hüllmodell des Waveshare-Moduls, nach dem Herstellermodell gebaut | eingebaut |
| `out_hat/…` | `rs232_2port_…` | dieselben Dateien für die Zwei-Port-Platine | wie oben |

Die Baugruppen enthalten das Modul als benannte Einzelkörper (`glass`, `lcm`, `pcb`, `headers`, `standoffs`,
`usbc`, `buttons` und weitere), die Platine als `carrier` und den Akku als `battery`. Eine zweite Baugruppe
mit dem vollständigen Herstellermodell (`rs232_console_assembly_vendor_module.step`, rund 100 MB) liegt nicht
im Repository; `case.py` schreibt sie, wenn die Herstellerdatei in `hardware/vendor/` liegt.

In CATIA V5: Datei öffnen, Typ STEP. Ein Einzelteil wird ein CATPart, eine Baugruppe ein CATProduct.

## Koordinaten

Gerätekoordinaten, in allen eingebauten Teilen gleich:

- **X** quer, **Y** von der Kante mit den RJ45-Buchsen (Y ≈ 0) zum USB-C-Ende, **Z** nach oben.
- **Z = 0** ist die Oberseite der Platine (Trägerplatine bzw. Zwei-Port-Platine).

Die Zahlen jeder Variante stehen in `out*/dimensions.txt` und maschinenlesbar in `out*/geometry.json`.
Schnitte mit Maßen: `out*/detail_sections.png`, für das Zwei-Port-Gehäuse dazu `out_hat/hat_layout.png`.

## Sollmaße zum Nachmessen

| Stelle | Soll |
|---|---|
| Außenmaße vier Ports / zwei Ports | 83,8 × 118,6 × 33,9 / 56,8 × 88,2 × 35,8 |
| Wand, Wand an den RJ45-Buchsen, Deckplatte, Boden | 2,4 / 1,6 / 3,0 / 2,0 |
| Deckhaut über dem Display | 1,2 (Glas 1,1 und 0,1, die das Glas unter der Oberseite liegt) |
| Öffnung für das Glas | 37,6 × 59,3 (Glas 37,1 × 58,8, ringsum 0,25 Spiel) |
| Rahmen | 6,0 dick, 0,2 Spiel zu den Wänden, Auflage unter dem Glas 337 mm² (am USB-C-Ende zwischen den Tasten offen) |
| Öffnung des Rahmens für die Modulplatine | feste Längsseite 0,10; USB-C-Ende 0,15; anderes Ende 0,35 mit zwei Quetschrippen von 0,45; Backenseite 0,6 frei |
| Federbacke | 19 × 2,6 × 6,0, Schlitz dahinter 0,8, Druckstück 4 lang, 0,4 im Platz der Platine |
| Taste: Fläche | 6,7 breit, 6,8 hoch; BOOT 0,2 unter der Außenfläche, RST 1,0; 0,4 Spiel zu Öffnung, Zunge und Steckerplatz |
| Taste: Blattfeder | 0,84 dick, 6,0 hoch, 14,0 lang (zwei Ports 9,1); Schlitz dahinter 1,0; Anschlag nach 0,8 Weg der Nase (Spalt an der Feder 0,49, zwei Ports 0,38) |
| Taste: Nase | 3,0 breit, 0,2 vor dem Stößel, Oberkante 0,4 unter der Modulplatine; Überhang 0,6, dann 45° |
| Taste: Kopf | 0,7 Spiel zum festen Rahmen auf der Federseite; Innenseite am Anschlag 0,15 vor der Platinenkante |
| Wand über der Öffnung | unten 0,4 stehen gelassen, innen unter 45° abgeschrägt; die Taste liegt 0,5 darunter |
| Öffnung der USB-C-Stirnwand | 28,4 breit; Platz für den Stecker 14,2 × 7,9 auf der Buchsenachse |
| Luft um den größten Steckergriff nach USB-IF (12,35 × 6,5) | 0,93 zu jeder Taste, 0,90 oben, 0,50 unten |
| Schwelle und Zunge des Bodens | 0,2 Spiel zu den Kanten ihrer Öffnungen |
| Federarm des Bodens | 18 × 1,6 × 5, Nase 0,9, Einführschräge 30°, Haltefläche 45°, Vorspannung 0,2 |
| Pfosten und Beine | 5,0 dick; ein Bein des Zwei-Port-Rahmens 3,0 × 11,7 |
| Zwei Ports: Akku | 40 × 30 × 7,8, 0,8 in die Bodenplatte eingelassen, Türme des Rahmens 0,3 darüber |

## Was die Skripte schon nachgerechnet haben

`case.py` bricht ab, wenn eine dieser Prüfungen nicht besteht:

- Durchdringung, jeweils 0 mm³: Körper, Rahmen, Boden, Modul (Hüllmodell und Herstellermodell),
  Platine und Akku paarweise. Rahmen gegen Modul: nur der Biss der beiden Backen.
- Berührung, wo etwas halten soll: Glas auf dem Rahmen, Rahmen unter der Deckhaut, Beine und Pfosten auf
  der Platine, Platine zwischen den Anschlägen des Rahmens.
- Wege: Modul in den Rahmen, Rahmen mit Modul in den Körper (die Tasten gleiten die Öffnung hinauf),
  Platine, Boden mit Akku (nur die Rastnasen berühren).
- Tasten: Kopf in Ruhe, 0,5 mm und 0,8 mm gedrückt (als starre Drehung um einen Punkt auf der Feder) gegen
  festen Rahmen, Körper, Boden und Modul; die Nase trifft den Stößel des Herstellermodells.
- Blattfeder der Tasten: 0,48 N/mm an der Nase (zwei Ports 1,25); Randdehnung beim Schalten 0,19 % (0,36 %),
  am Anschlag 0,30 % (0,58 %), Grenze 1,0 %.
- USB-C-Stecker mit dem größten erlaubten Griff, eingesteckt und auf dem Weg hinein, auch bei gedrückten Tasten.
- Federarm für PLA (E rund 3300 MPa): Auslenkung 0,65 mm, Randdehnung 0,65 % bei einer Grenze von 1,0 %.
- Federbacke: 0,4 mm ausgelenkt, 7,1 N, Randdehnung 0,54 %; bei 0,2 bis 0,6 mm Biss 3,5 bis 10,6 N.

`printcheck.py` prüft die Druckbarkeit ohne Stützen: Brücken höchstens 6 mm, freie Kanten höchstens 0,6 mm.
Im Rahmen gibt es keine Brücke mehr; frei beginnen nur die beiden Kanten unter den Nasen der Tasten (0,6 mm).

## Was nicht aus Herstellerdaten stammt

- **Zwei-Port-Platine**: Höhe der RJ45-Buchsen 13,6 mm über der Platine, Lötstifte 3,3 mm darunter.
- **Steckbrücken**: Stecker 14 mm lang, 2,54 mm im Quadrat, 3 mm für den Drahtbogen darunter.
- **USB-C-Stecker**: gerechnet ist mit dem größten Griff, den die USB-IF erlaubt.
- **Modul**: Maße aus dem STEP-Modell von Waveshare vom 8. November 2024. Fertigungstoleranzen des Moduls
  sind nicht bekannt, insbesondere die Breite der Platine, an der die Backen klemmen.
- **Reibung**: Für die Haltekraft der Backen ist ein Reibwert von 0,3 bis 0,5 angenommen.
- **Taster**: Hub (0,3 mm) und Schaltkraft (1,6 N) der Taster RST und BOOT sind angenommen, nicht aus einem Datenblatt.
- **PLA**: Schwund und Maßhaltigkeit des Druckers sind nicht eingerechnet; PLA gibt unter Dauerlast nach.

## Wozu eine zweite Meinung besonders hilft

1. Federbacken: Reicht die Klemmung über die Zeit? Die Backen stehen dauernd unter 0,54 % Dehnung, und PLA
   kriecht. Wäre ein zusätzlicher Formschluss möglich, ohne über das Glas zu greifen?
2. Deckhaut: 1,2 mm über der Fläche des Rahmens. Reicht das, oder sollte sie dicker sein (das Glas läge
   dann tiefer)?
3. Kette der Toleranzen von der Deckhaut über Rahmen, Beine, Platine und Pfosten bis zu den Rastarmen
   (0,2 mm Vorspannung): Sitzt das Glas am Ende sicher auf?
4. Bein des Zwei-Port-Rahmens auf der Backenseite: 3 mm dick, 20 mm hoch, stehend gedruckt.
5. Zunge und Schwelle des Bodens: stehende Wände von 2,35 und 1,6 mm Dicke.
6. Blattfedern der Tasten: 0,84 mm PLA in der Schichtebene, im Zwei-Port-Gehäuse nur 9,1 mm lang. Hält das
   einige tausend Betätigungen? Die Oberseite von Feder und Kopf liegt in der Ebene der Glasauflage und
   streift beim Drücken unter dem Glas.
