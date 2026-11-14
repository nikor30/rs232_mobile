# Prüfanleitung für die Gehäuse (CATIA)

Für alle, die die beiden Gehäuse in einem CAD-System nachprüfen. Stand: Hardware-Revision 0.4, 14. November 2026.
**Nichts davon ist gedruckt oder gebaut.** Alle Maße in Millimetern.

## Dateien

STEP AP214 (`AUTOMOTIVE_DESIGN`), geschrieben von Open CASCADE 7.9 über CadQuery. Jedes Einzelteil trägt einen
eigenen Teilenamen, damit sich mehrere Dateien in einer CATIA-Sitzung öffnen lassen.

| Datei | Teilename | Inhalt | Lage |
|---|---|---|---|
| `out/body.step` | `rs232_4port_body` | Körper, vier Ports | eingebaut |
| `out/base.step` | `rs232_4port_base` | Boden mit fünf Federarmen | eingebaut |
| `out/key.step` | `rs232_4port_key` | Taste für RST und BOOT, zweimal nötig | flach, wie gedruckt |
| `out/rs232_console_assembly.step` | `rs232_console` | alles zusammen: Körper, Boden, beide Tasten, Modul, Trägerplatine | eingebaut |
| `out/waveshare_module_envelope.step` | | Hüllmodell des Waveshare-Moduls, nach dem Herstellermodell gebaut | eingebaut |
| `out_hat/body.step`, `base.step`, `key.step` | `rs232_2port_…` | dasselbe für die Zwei-Port-Platine | wie oben |
| `out_hat/cradle.step` | `rs232_2port_cradle` | Wiege: trägt das Modul und hält den Akku | stehend, wie gedruckt |
| `out_hat/rs232_console_assembly.step` | `rs232_console` | Zusammenbau der Zwei-Port-Variante | eingebaut |

Die Baugruppen enthalten das Modul als benannte Einzelkörper (`glass`, `lcm`, `pcb`, `headers`, `standoffs`,
`usbc`, `buttons` und weitere) und die Platine als `carrier`. Eine zweite Baugruppe mit dem vollständigen
Herstellermodell (`rs232_console_assembly_vendor_module.step`, rund 100 MB) liegt nicht im Repository, weil
die Herstellerdatei nicht weitergegeben wird; `case.py` schreibt sie, wenn die Datei in `hardware/vendor/` liegt.

In CATIA V5: Datei öffnen, Typ STEP. Ein Einzelteil wird ein CATPart, eine Baugruppe ein CATProduct.

## Koordinaten

Gerätekoordinaten, in allen eingebauten Teilen gleich:

- **X** quer, **Y** von der Kante mit den RJ45-Buchsen (Y ≈ 0) zum USB-C-Ende, **Z** nach oben.
- **Z = 0** ist die Oberseite der Platine (Trägerplatine bzw. Zwei-Port-Platine).
- Vier Ports: Körper X −2,9 … 80,9, Y −1,9 … 116,73, Z −13,8 … 18,6. Achse der USB-C-Buchse X 39,0, Z 9,63.
- Zwei Ports: Körper X −3,4 … 53,4, Y −2,6 … 78,44, Z −8,0 … 27,8. Achse der USB-C-Buchse X 25,0, Z 18,83.

Die Zahlen jeder Variante stehen auch in `out*/dimensions.txt` und maschinenlesbar in `out*/geometry.json`.

## Sollmaße zum Nachmessen

| Stelle | Soll |
|---|---|
| Außenmaße vier Ports / zwei Ports | 83,8 × 118,6 × 32,4 / 56,8 × 81,0 × 35,8 |
| Wand, Wand an den RJ45-Buchsen, Deckplatte, Boden | 2,4 / 1,6 / 3,0 / 2,0 |
| Glastasche | 37,6 × 59,3, Tiefe 1,20 (Glas 37,1 × 58,8 × 1,1, ringsum 0,25 Spiel, 0,1 unter der Oberseite) |
| Auflage unter dem Glas | 0,74 an den Längsseiten, rund 5 an den Stirnseiten, zusammen 423 mm² |
| Öffnung für das Modul | Modulplatine plus 0,30 ringsum |
| Oberkante | Fase 1,2 × 45° |
| USB-C-Öffnung | 13,4 × 7,5, Eckradius 2,0, auf der Buchsenachse |
| Buchsenrand hinter der Außenfläche | 6,55 (hinter dem Muldenboden 5,55) |
| Luft um den größten Steckergriff nach USB-IF (12,35 × 6,5) | 0,53 je Seite, 0,50 oben und unten |
| Taste | Kappe 3,0 × 6,0, Spiel im Tunnel 0,2, Weg 0,6, Nase 0,15 vor dem Stößel |
| Tastenkappe | 0,8 über dem Muldenboden, 0,2 unter der Außenfläche |
| Federarm | 18 × 1,6 × 5, Nase 0,9, Einführschräge 30°, Haltefläche 45°, Vorspannung 0,2 |
| Fenster der RJ45-Buchsen | Buchse plus 0,3 ringsum |
| Zwei Ports: Zapfen für die Platine | Ø 2,4 in Löchern Ø 2,7, Länge 3,6 |
| Zwei Ports: Platine zur Wand | 1,0 ringsum (vier Ports: 0,5, an den Buchsen 0,3) |

## Was die Skripte schon nachgerechnet haben

`case.py` bricht ab, wenn eine dieser Prüfungen nicht besteht. Boolesche Durchdringung, jeweils 0 mm³:

- Körper gegen Modul (Hüllmodell und Herstellermodell), Körper und Boden gegen Platine, Modul gegen Platine.
- Wege: Modul von oben einsetzen, Platine nach unten herausziehen, Boden eindrücken (nur die fünf Nasen
  berühren, höchstens 15,8 mm³ auf dem Weg), Tasten von innen einschieben, Modul an den Tasten vorbei.
- Tasten in Ruhe und 0,6 mm gedrückt gegen Körper und Modul; die Nase trifft den Stößel des Herstellermodells.
- USB-C-Stecker mit dem größten erlaubten Griff, eingesteckt und auf dem Weg hinein.
- Federarm für PLA (E rund 3300 MPa): Auslenkung 0,65 mm, Randdehnung 0,65 % bei einer Grenze von 1,0 %,
  2,9 N je Arm, Eindrücken etwa 16 N, Abziehen etwa 27 N, eingerastet 0,20 % Dehnung durch die Vorspannung.

`printcheck.py` prüft die Druckbarkeit ohne Stützen (siehe `../README.md`, Abschnitt „Drucken“).

## Was nicht aus Herstellerdaten stammt

Diese Werte sind Annahmen. Wer das Teil in der Hand hat, misst sie bitte nach:

- **Zwei-Port-Platine**: Höhe der RJ45-Buchsen 13,6 mm über der Platine, Lötstifte 3,3 mm darunter. Die
  Fertigungsdaten der Platine enthalten nur die Lage der Buchsen, nicht ihre Höhe.
- **Steckbrücken**: Stecker 14 mm lang, 2,54 mm im Quadrat, 3 mm für den Drahtbogen darunter.
- **Akku**: 20 × 30 × 3 mm (zwei Ports), bis 40 × 60 × 7 mm (vier Ports).
- **USB-C-Stecker**: gerechnet ist mit dem größten Griff, den die USB-IF erlaubt. Dickere Griffe passen nicht.
- **Modul**: Maße aus dem STEP-Modell von Waveshare vom 8. November 2024. Fertigungstoleranzen des Moduls
  sind nicht bekannt; für das Glas sind ±0,05 mm angenommen.
- **PLA**: Schwund und Maßhaltigkeit des Druckers sind nicht eingerechnet. Der erste Druck ist eine Probe.

## Wozu eine zweite Meinung besonders hilft

1. Federarme: Reicht die Reserve für PLA, auch nach vielen Öffnungen und bei Wärme? PLA gibt unter Dauerlast nach.
2. Auflage des Glases: 0,74 mm an den Längsseiten ist schmal. Hält sie den Druck beim Tippen, oder braucht
   sie eine Rippe darunter?
3. Tasten: Führung im Tunnel (0,2 mm Spiel) und der Weg von 0,6 mm bei gedruckten Flächen.
4. Zwei Ports: Die Wiege steht nur auf der Platine und wird vom Modul gehalten. Sitzt sie beim Zusammenbau sicher?
5. Zwei Ports: Das Modul liegt oben auf der Glasauflage und unten auf vier Abstandsbolzen (0,15 mm Luft).
   Ist das Spiel richtig gewählt?
