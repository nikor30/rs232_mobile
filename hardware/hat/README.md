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

Außen **56,8 × 81,0 × 32,8 mm**. Fünf gedruckte Teile: `body`, `base`, zweimal `key`, einmal `rest`.

- **Platine**: Die vier Pfosten des Körpers haben Zapfen (Ø 2,4), auf die die Platine mit ihren Löchern
  gesetzt wird. Vier Pfosten des Bodens drücken von unten dagegen und nehmen die Zapfenenden auf.
- **Modul**: Es steckt hier in keiner Buchsenleiste. Das Glas liegt wie immer auf der Auflage; von unten
  tragen es seine vier M2-Abstandsbolzen. Unter den beiden am USB-C-Ende stehen Pfosten des Bodens. Die
  beiden an der Antennenseite liegen über der Platine, dort reicht kein Pfosten hin. Sie sitzen auf der
  **Modulauflage** (`rest`), einem Steg, der auf der Platine im freien Streifen zwischen der
  Widerstandsreihe und den Lötbrücken steht. Hörner an seinen Enden halten ihn zwischen den Bolzen.
- **Lage**: Das Modul liegt über der flachen Hälfte der Platine, die Stiftspitzen enden 2,1 mm über ihr.
  Die Buchsen stehen dahinter unter dem Glasüberstand. Die Buchsenfronten liegen 3,1 mm hinter der Außenfläche.
- **Akku** unter der Platine, bis 35 × 52 × 7 mm.

Zeichnung mit Maßen: `enclosure/out_hat/hat_layout.png`. Schnitte von Rastarm, Glasauflage, Taste und
USB-C-Stecker: `enclosure/out_hat/detail_sections.png`.

### Zusammenbau

1. Lötbrücken JP1 bis JP4 setzen (sie liegen später unter dem Modul). Die sechs Drähte von J3 an die Stifte
   des Moduls löten, rund 50 mm lang, damit sich die Platine über das Modul klappen lässt.
2. Beide Tasten von innen in ihre Tunnel schieben. Modul von oben in den Körper setzen, Körper umdrehen.
3. Modulauflage auf die beiden Abstandsbolzen neben den Buchsenfenstern legen, Hörner außen.
4. Platine mit der Bauteilseite zum Modul auf die vier Zapfen setzen. Die Buchsen stehen dann hinter ihren
   Fenstern.
5. Akku einlegen und anstecken, Boden eindrücken, bis die fünf Arme einrasten.

### Drucken

Wie beim großen Gehäuse: PETG oder ABS, Körper mit der Oberseite nach unten, Boden flach. `key.stl` zweimal
und `rest.stl` einmal, flach wie sie liegen, ohne Stützen. Die Zapfen an den Pfosten des Körpers sind 3,6 mm
lang und 2,4 mm dick: langsam drucken und beim Aufsetzen der Platine nicht verkanten.

### Zu beachten

- **Akku sichern**: Unter der Platine stehen die Lötstifte der Buchsen. Der Akku liegt 0,7 mm darunter, wenn
  er 7 mm dick ist, ein dünnerer kann sich bewegen. Den Akku mit doppelseitigem Klebeband auf den Boden
  kleben und die Lötstifte kürzen oder mit Kaptonband abdecken.
- **Buchsenhöhe** nachmessen (siehe oben).
- **WLAN**: Die Keramikantenne des Moduls liegt rund 4 mm vor den Rückseiten der Buchsen. Die Reichweite kann
  darunter leiden; mit eingesteckten Kabeln ausprobieren.
- **Firmware**: Port 1 RX 44 / TX 21, Port 2 RX 15 / TX 13. Die Software-Kreuzung der Vier-Port-Platine gibt
  es hier nicht; gekreuzt wird mit den Lötbrücken.

### Was `case.py --hat` prüft

Alles, was es beim großen Gehäuse prüft (Durchdringungen, Montagewege, Tasten, USB-C-Stecker, auch gegen das
Herstellermodell des Moduls), dazu: Modul gegen Platine, Modulauflage gegen Platine, Modul und Körper, Boden
gegen Modul, und dass Bodenpfosten und Modulauflage die Abstandsbolzen wirklich treffen.

## Neu erzeugen

```bash
cd hardware/hat && python3 board.py                      # zeigt, was aus den Fertigungsdaten gelesen wurde
cd ../enclosure && python3 case.py --hat && python3 render.py --hat
```
