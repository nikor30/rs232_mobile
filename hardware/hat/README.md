# RS232 CONSOLE HAT v3.0: Zwei-Port-Prototyp und sein Gehäuse

Stand 14. November 2026. Die Platine ist gefertigt und bestückt und am 7. November 2026 mit beiden Ports an
einem Cisco-Switch geprüft worden. Das Gehäuse dazu ist **entworfen und rechnerisch geprüft, nicht gedruckt.**

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

## Das Gehäuse

Dieselbe Konstruktion wie für die Vier-Port-Trägerplatine (`../README.md`, Abschnitt „Gehäuse“), aus
demselben Skript: `enclosure/case.py --hat`. Oberseite und USB-C-Ende sind unverändert: Glas bündig auf seiner
Auflage, Mulde mit Stecköffnung und den zwei Tasten für BOOT und RST, Boden mit fünf Rastarmen.

Außen **56,8 × 81,0 × 35,7 mm**. Fünf gedruckte Teile: `body`, `base`, zweimal `key`, einmal `cradle`.

### Anordnung: Platine unten, Akku in der Mitte, Modul oben

Die sechs Drähte sind Steckbrücken: in J3 gelötet, am Modul auf die Stifte gesteckt. Ein solcher Stecker ist
14 mm lang, steht also 8 mm über die Stiftspitze hinaus, und darunter braucht der Draht Platz für den Bogen.
Deshalb liegt die Platine ganz unten und das Modul hoch darüber:

| von unten nach oben | Höhe über der Platine (mm) |
|---|---|
| Boden, darüber 3,8 mm für die Lötstifte der Buchsen | |
| Platine | 0 (Oberseite) |
| Drahtbogen unter den Steckern | 1,0 bis 4,0 |
| Stecker der Steckbrücken | 4,0 bis 18,0 |
| Akkutasche, zwischen den beiden Stiftreihen | 6,6 bis 13,0 |
| Spitzen der unbenutzten Stifte | 12,2 |
| Modulplatine | 20,5 bis 22,1 |
| Glas, bündig mit der Oberseite | 27,7 |

- **Platine**: Die vier Pfosten des Körpers haben Zapfen (Ø 2,4), auf die die Platine mit ihren Löchern
  gesetzt wird. Vier kurze Pfosten des Bodens drücken von unten dagegen und nehmen die Zapfenenden auf.
- **Modul**: Es steckt in keiner Buchsenleiste. Das Glas liegt wie immer auf der Auflage; von unten tragen es
  seine vier M2-Abstandsbolzen. Unter den beiden am USB-C-Ende stehen zwei Pfosten des Bodens, durch einen
  Steg zu einem Portal verbunden. Die beiden an der Antennenseite liegen über der Platine und sitzen auf dem
  Bügel der **Wiege** (`cradle`).
- **Wiege**: ein Druckteil mit zwei Aufgaben. Ihr Bügel steht auf der Platine im freien Streifen zwischen
  der Widerstandsreihe und den Lötbrücken; Hörner an seinen Enden halten ihn zwischen den Abstandsbolzen. Vom
  Bügel läuft ein Rohr zum USB-C-Ende, zwischen den beiden Stiftreihen: die **Akkutasche**, innen
  21 × 30,5 × 4 mm für einen Akku von 20 × 30 × 3 mm. Zwei Rippen unter dem Rohr stehen auf der Platine und
  lassen J3 mit seinen Drähten frei.
- **Stecker**: Fünf der sechs sitzen auf der Stiftreihe 1 bis 14 (3V3, GND, GPIO44, GPIO15, GPIO13), einer auf
  der anderen (GPIO21). Zwischen Akkutasche und Steckern bleiben 2,3 mm auf jeder Seite.
- Die Buchsenfronten liegen 3,1 mm hinter der Außenfläche.

Zeichnung mit Maßen: `enclosure/out_hat/hat_layout.png`. Schnitte von Rastarm, Glasauflage, Taste und
USB-C-Stecker: `enclosure/out_hat/detail_sections.png`.

### Zusammenbau

1. Lötbrücken JP1 bis JP4 setzen (sie liegen später unter dem Modul). Die sechs Steckbrücken in J3 löten.
2. Beide Tasten von innen in ihre Tunnel schieben. Modul von oben in den Körper setzen, Körper umdrehen.
3. Akku in das Rohr der Wiege schieben, Kabel am offenen Ende heraus. Wiege auf die beiden Abstandsbolzen
   neben den Buchsenfenstern legen, Hörner außen, Rohr zwischen die Stiftreihen.
4. Die sechs Drähte auf die Stifte des Moduls stecken, den Akku am Modul anstecken.
5. Platine mit der Bauteilseite zum Modul auf die vier Zapfen setzen. Boden eindrücken, bis die fünf Arme
   einrasten. Der Steg zwischen den hohen Pfosten steht dann vor dem offenen Rohrende und hält den Akku drin.

### Drucken

Wie beim großen Gehäuse: PETG oder ABS, Körper mit der Oberseite nach unten, Boden flach. `key.stl` zweimal,
flach wie es liegt. `cradle.stl` einmal, stehend wie es liegt (der Bügel auf dem Druckbett, das Rohr wächst
40 mm nach oben), ohne Stützen. Die Zapfen an den Pfosten des Körpers sind 3,6 mm lang und 2,4 mm dick:
langsam drucken und beim Aufsetzen der Platine nicht verkanten.

### Zu beachten

- **Buchsenhöhe** nachmessen (siehe oben).
- **Akku**: Die Tasche ist für 20 × 30 × 3 mm gebaut. Ein anderer Akku: `BATTERY` in `board.py` ändern. Breiter
  als 24 mm geht nicht, dann stößt die Tasche an die Stecker.
- **Stecker**: gerechnet ist mit 14 mm langen Einzelsteckern und 3 mm für den Drahtbogen (`PLUG_LEN`,
  `WIRE_BEND` in `board.py`). Längere Stecker machen das Gehäuse entsprechend höher.
- **WLAN**: Die Keramikantenne des Moduls liegt rund 4 mm hinter den Rückseiten der Buchsen und 7 mm über
  ihnen; der Akku endet 14 mm vor ihr. Die Reichweite mit eingesteckten Kabeln ausprobieren.
- **Firmware**: Port 1 RX 44 / TX 21, Port 2 RX 15 / TX 13. Die Software-Kreuzung der Vier-Port-Platine gibt
  es hier nicht; gekreuzt wird mit den Lötbrücken.

### Was `case.py --hat` prüft

Alles, was es beim großen Gehäuse prüft (Durchdringungen, Montagewege, Tasten, USB-C-Stecker, auch gegen das
Herstellermodell des Moduls), dazu: Modul gegen Platine, Wiege gegen Platine, Modul, Körper und Boden, die
sechs Stecker samt Drahtbogen gegen Platine, Wiege, Boden und Körper, der Akku in seiner Tasche, und dass
Bodenpfosten und Wiege die Abstandsbolzen wirklich treffen.

## Neu erzeugen

```bash
cd hardware/hat && python3 board.py                      # zeigt, was aus den Fertigungsdaten gelesen wurde
cd ../enclosure && python3 case.py --hat && python3 render.py --hat
```
