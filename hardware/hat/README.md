# RS232 CONSOLE HAT v3.0: Zwei-Port-Prototyp und sein Gehäuse

Stand 15. November 2026. Die Platine ist gefertigt und bestückt und am 7. November 2026 mit beiden Ports an
einem Cisco-Switch geprüft worden. Das Gehäuse dazu ist einmal gedruckt worden (Revision 0.4) und danach neu
aufgebaut; **die jetzige Fassung (Revision 0.5) ist rechnerisch geprüft, nicht gedruckt.**

## Die Platine

Einfacher, nicht isolierter Prototyp: ein MAX3232ESE, zwei RJ45-Buchsen (Amphenol 54602-908LF), je Port zwei
Lötbrücken (JP1 bis JP4: S = gerade, R = Rollover), 150 Ω in jeder RS232-Leitung. Sie steckt nicht auf dem
Modul, sondern hängt mit sechs Drähten an J3:

| J3 | 3V3 | GND | TX1 | RX1 | TX2 | RX2 |
|---|---|---|---|---|---|---|
| Modul | 3V3 | GND | GPIO21 | GPIO44 (RXD) | GPIO13 | GPIO15 |

`gerber/` enthält die Fertigungsdaten, mit denen bestellt wurde (Gerber, Bohrdaten, Bestückungsliste,
Stückliste). `board.py` liest daraus alles, was das Gehäuse braucht:

| Maß | Wert (mm) |
|---|---|
| Platine | 50,00 × 50,00 × 1,60, Ecken eckig |
| Befestigungslöcher | vier, Ø 2,7, je 3,6 von den Kanten |
| Buchsen | Mitten bei x = 15,945 und 33,945 (18,00 Abstand), Körper 15,30 × 17,74, Front 0,52 hinter der Platinenkante |
| J3 | sechs Lötaugen im Raster 2,54 an der gegenüberliegenden Kante |
| Bauteile außer den Buchsen | höchstens 1,75 hoch |

**Nicht aus den Dateien:** die Höhe der Buchsen (angenommen 13,6 mm) und die Länge der Lötstifte unter der
Platine (angenommen bis 3,3 mm). Beides steht in `board.py` (`JACK_H`, `PIN_BELOW`); vor dem Druck einmal
nachmessen. Das Gehäuse hat über den Buchsen 1,0 mm Luft.

## Das Gehäuse (Revision 0.5, nach dem ersten Probedruck)

Dieselbe Konstruktion wie für die Vier-Port-Trägerplatine (`../README.md`, Abschnitt „Gehäuse“), aus
demselben Skript: `enclosure/case.py --hat`. Die erste Fassung ist am 15. November 2026 in PLA gedruckt
worden; was dabei schiefging und was daraus folgt, steht dort in der Tabelle. Diese Fassung ist danach neu
aufgebaut und **noch nicht gedruckt**.

Außen **56,8 × 88,2 × 35,8 mm** (vorher 81,0 lang: der Akku braucht den Platz). Drei gedruckte Teile:
`body`, `frame`, `base`. Die beiden Tasten sind Teil des Rahmens, es gibt nichts Loses. Kein Teil braucht
Stützen. Keine Schraube, kein Kleber.

### Anordnung

Die sechs Drähte sind Steckbrücken: in J3 gelötet, am Modul auf die Stifte gesteckt. Ein solcher Stecker ist
14 mm lang, und darunter braucht der Draht Platz für den Bogen. Deshalb liegt die Platine ganz unten und das
Modul hoch darüber. Der Akku (40 × 30 × 7,8 mm, 1000 mAh) ist 30 mm breit und passt damit nicht zwischen die
Stecker der beiden Stiftreihen (27,9 mm). Er liegt flach auf dem Boden hinter der Platine, unter der
USB-C-Hälfte des Moduls. Dafür sitzt das Modul 7,0 mm weiter zum USB-C-Ende als in der ersten Fassung, und
das Gehäuse ist um so viel länger. Höher ist es nicht geworden.

| von unten nach oben | Höhe über der Platine (mm) |
|---|---|
| Bodenplatte | −7,4 bis −5,4 |
| Akku, 0,8 mm in die Bodenplatte eingelassen | −6,2 bis 1,6 |
| Platine | 0 (Oberseite) |
| Drahtbogen unter den Steckern | 1,0 bis 4,0 |
| Stecker der Steckbrücken | 4,0 bis 18,0 |
| Spitzen der unbenutzten Stifte | 12,2 (10,6 über dem Akku) |
| Modulplatine | 20,5 bis 22,1 |
| Rahmen | 20,6 bis 26,6 |
| Glas, 0,1 mm unter der Oberseite | 27,7 |

- **Modul**: im Rahmen geklemmt (zwei Federbacken, 7 N je Backe; 9 bis 14 N, um es herauszuschieben). Das
  Glas liegt ringsum auf dem Rahmen und bündig in der Öffnung der Deckhaut.
- **Rahmen**: liegt unter der Deckhaut. Zwei Beine stehen auf den hinteren Ecken der Platine (dort ist sie
  frei von Bauteilen). Das Bein auf der Backenseite ist nur 3 mm dick, weil dort nur der äußere Holm des
  Rahmens bleibt; es ist deshalb eine 12 mm lange Wand. An den beiden Ecken des USB-C-Endes steht je ein
  Winkel (3 mm dick), der bis 0,3 mm über den Akku reicht.
- **Tasten** (RST, BOOT): je eine Fläche von 6,7 × 6,8 mm links und rechts der USB-C-Buchse, aus einem Stück
  mit dem Rahmen. Der Kopf der Taste hängt an einer Blattfeder (0,84 × 6 mm, 9,1 mm lang), die auf dem
  Druckbett steht und sich in der Schichtebene biegt; eine Nase reicht bis 0,2 mm vor den Stößel des
  Tasters. BOOT liegt 0,2 mm unter der Außenfläche, RST 1,0 mm (löst nicht versehentlich aus). Gerechnet:
  1,25 N/mm an der Nase, Randdehnung 0,36 % beim Schalten, 0,58 % am Anschlag nach 0,8 mm. Einzelheiten:
  `../README.md`, „Die Tasten“.
- **Platine**: liegt oben an den Beinen des Rahmens und an zwei Pfosten des Körpers (5 mm dick, bei den
  Buchsen), unten auf vier Pfosten des Bodens. Flache Kuppen (Ø 2,3 × 1,2 mm) in ihren vier Löchern
  zentrieren sie. Keine Zapfen mehr.
- **Akku**: in einer 0,8 mm tiefen Wanne der Bodenplatte zwischen zwei niedrigen Leisten. An der Platine
  eine Anschlagrippe mit Lippe, unter die er geschoben wird; am USB-C-Ende halten ihn die beiden Winkel des
  Rahmens unten. Er liegt 22 mm von der Antenne des Moduls entfernt und 19 mm tiefer.
- **Stecker**: Fünf der sechs sitzen auf der Stiftreihe 1 bis 14 (3V3, GND, GPIO44, GPIO15, GPIO13), einer auf
  der anderen (GPIO21). Drei von ihnen (GPIO15, GPIO13, GPIO21) hängen über der Kante des Akkus: Dort
  bleiben 2,4 mm zwischen Stecker und Akku, die Drähte biegen zur Seite ab.
- Die Buchsen sitzen in einem Schlitz, der zum Rand offen ist; die Schwelle des Bodens schließt ihn unter
  den Buchsen. Zwischen den beiden Buchsen bleibt ein Spalt von 2,7 mm offen (ein Steg dort wäre zu dünn,
  um zu halten).

Zeichnung mit Maßen und Reihenfolge des Zusammenbaus: `enclosure/out_hat/hat_layout.png`. Schnitte von
Rastarm, Glasauflage, Backe, Taste und USB-C-Ende: `enclosure/out_hat/detail_sections.png`. Bilder:
`enclosure/out_hat/render_*.png`, die Tasten in `render_keys.png`, `render_keys_module.png` und
`render_keys_top.png`.

### Zusammenbau

1. Lötbrücken JP1 bis JP4 setzen (sie liegen später unter dem Modul). Die sechs Steckbrücken in J3 löten.
2. Modul von der Glasseite in den Rahmen drücken, Stifte voran, bis das Glas aufliegt. Die Tasten sind
   schon am Rahmen.
3. Körper mit der Oberseite nach unten legen. Rahmen mit Modul einsetzen: Das Glas geht in seine Öffnung,
   die Tasten gleiten durch die Öffnung der Stirnwand.
4. Die sechs Drähte auf die Stifte des Moduls stecken, den Akku am Modul anstecken.
5. Platine mit der Bauteilseite zum Modul auf die Beine des Rahmens und die beiden Pfosten des Körpers legen.
6. Akku unter die Lippe am Boden schieben. Boden eindrücken, bis die vier Arme einrasten.

### Drucken (PLA, Bambu Lab, 0,4-mm-Düse)

PLA, 0,2 mm Schichthöhe, **Stützen im Slicer aus**. In `enclosure/out_hat/print/` liegen die Teile so, wie
sie auf das Druckbett kommen (Bild: `enclosure/out_hat/render_print_bed.png`):

| Datei | Lage auf dem Bett | Größe (mm) |
|---|---|---|
| `body_print.stl` | Oberseite unten | 56,8 × 88,2 × 35,8 |
| `frame_print.stl` | Glasauflage unten, Tasten, Winkel und Beine nach oben | 51,6 × 64,5 × 26,6 |
| `base_print.stl` | flach, Zunge und Schwelle stehen nach oben | 53,3 × 88,2 × 22,5 |

Nicht neu ausrichten. `python3 printcheck.py --hat` rechnet die Druckbarkeit nach (Brücken höchstens 6 mm,
freie Kanten höchstens 0,6 mm): Im ganzen Gehäuse gibt es keine Brücke mehr. Frei beginnen nur zwei Kanten
von 0,6 mm unter den Nasen der Tasten und die vertieften Ziffern und Buchstaben. Nach dem Druck jede Taste
einmal durchdrücken.

### Zu beachten

- **Buchsenhöhe** nachmessen (siehe oben).
- **Akku**: gebaut für 40 × 30 × 7,8 mm (`BATTERY` in `board.py`). Ein längerer Akku macht das Gehäuse länger.
- **Stecker**: gerechnet ist mit 14 mm langen Einzelsteckern und 3 mm für den Drahtbogen (`PLUG_LEN`,
  `WIRE_BEND` in `board.py`). Längere Stecker machen das Gehäuse entsprechend höher.
- **Beine und Winkel des Rahmens**: stehend gedruckt. Sie tragen nur Druck; beim Hantieren mit dem losen
  Rahmen nicht seitlich daran hebeln.
- **Tasten**: Die Blattfedern sind hier kürzer als im großen Gehäuse (9,1 statt 14 mm) und damit steifer
  und stärker gedehnt. Wirkt eine Taste zu hart, `key_leaf_t` in `case.py` kleiner wählen.
- **Firmware**: Port 1 RX 44 / TX 21, Port 2 RX 15 / TX 13. Die Software-Kreuzung der Vier-Port-Platine gibt
  es hier nicht; gekreuzt wird mit den Lötbrücken.

### Was `case.py --hat` prüft

Alles, was es beim großen Gehäuse prüft (Durchdringungen, was sich berühren muss, Montagewege, Tasten,
USB-C-Stecker, auch gegen das Herstellermodell des Moduls), dazu: Modul gegen Platine, die sechs Stecker
samt Drahtbogen gegen Platine, Rahmen, Boden und Körper, der Akku gegen alles, die Lippe über dem Akku und
dass die Winkel des Rahmens über ihm stehen.

## Neu erzeugen

```bash
cd hardware/hat && python3 board.py                      # zeigt, was aus den Fertigungsdaten gelesen wurde
cd ../enclosure && python3 case.py --hat && python3 render.py --hat
```
