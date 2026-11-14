#!/usr/bin/env python3
"""RS232 CONSOLE HAT v3.0 - the two-port prototype board, as the case generator sees it.

Everything geometric is read from the fabrication files in gerber/ (the set the boards were ordered
with): outline, drill files and placement list.  Board 50 x 50 x 1.6 mm, four 2.7 mm mounting holes
3.6 mm from the edges, two Amphenol 54602-908LF jacks on one edge, six pads J3 on the opposite one.
J3 is wired to the module (3V3, GND, TX1 -> GPIO21, RX1 -> GPIO44, TX2 -> GPIO13, RX2 -> GPIO15); the
board is not plugged onto the module's pins.

Coordinates as in KiCad: x to the right, y down, origin at the corner next to hole 1; the jacks open
towards y = 50.

    python3 board.py        # prints what was read
"""
import csv
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
GERBER = os.path.join(HERE, "gerber")


def _outline():
    xs, ys = [], []
    for line in open(os.path.join(GERBER, "rs232_hat-Edge_Cuts.gbr")):
        m = re.match(r"X(-?\d+)Y(-?\d+)D0[12]\*", line.strip())
        if m:
            xs.append(int(m.group(1)) / 1e6)
            ys.append(-int(m.group(2)) / 1e6)
    return min(xs), min(ys), max(xs), max(ys)


def _drills(name):
    """Excellon file -> {diameter: [(x, y)]}."""
    tools, out, cur = {}, {}, None
    for line in open(os.path.join(GERBER, name)):
        line = line.strip()
        m = re.match(r"T(\d+)C([\d.]+)", line)
        if m:
            tools[m.group(1)] = float(m.group(2))
            continue
        m = re.match(r"T(\d+)$", line)
        if m:
            cur = tools.get(m.group(1))
            continue
        m = re.match(r"X(-?[\d.]+)Y(-?[\d.]+)", line)
        if m and cur:
            out.setdefault(cur, []).append((float(m.group(1)), -float(m.group(2))))
    return out


def _placements():
    out = {}
    for row in csv.DictReader(open(os.path.join(GERBER, "rs232_hat-pos.csv"))):
        out[row["Ref"]] = (row["Package"], float(row["PosX"]), -float(row["PosY"]), float(row["Rot"]))
    return out


_x1, _y1, _x2, _y2 = _outline()
W, H = _x2 - _x1, _y2 - _y1
T = json.load(open(os.path.join(GERBER, "rs232_hat-job.gbrjob")))["GeneralSpecs"]["BoardThickness"]
NPTH, PTH = _drills("rs232_hat-NPTH.drl"), _drills("rs232_hat-PTH.drl")
PARTS = _placements()

HOLE_D = 2.7
HOLES = sorted(NPTH[HOLE_D])                       # the four mounting holes

# RJ45: Amphenol 54602-908LF.  Body outline from KiCad's footprint RJ45_Amphenol_54602-x08_Horizontal,
# relative to pin 1: x -3.205 .. 12.095, y -3.77 .. 13.97 (15.30 wide, 17.74 deep).
JACK_BODY = (-3.205, -3.77, 12.095, 13.97)
JACK_W = JACK_BODY[2] - JACK_BODY[0]
JACK_H = 13.6          # above the board: NOT from the files - measure a jack before printing
JACK_PEG = 3.2         # the two board locks
PIN_BELOW = 3.3        # longest solder tail under the board (jack pins, board locks)
JACKS = []             # (name, x1, y1, x2, y2) of each jack body
for ref in ("J1", "J2"):
    _, px, py, _ = PARTS[ref]
    JACKS.append((ref, px + JACK_BODY[0], py + JACK_BODY[1], px + JACK_BODY[2], py + JACK_BODY[3]))
JACK_X = [(j[1] + j[3]) / 2 for j in JACKS]        # centres of the two jacks
JACK_FRONT = max(j[4] for j in JACKS)              # y of the jack fronts: 0.52 inside the board edge
JACK_BACK = min(j[2] for j in JACKS)

# J3: six pads, wires soldered in
J3 = sorted(PTH[1.0])

# Where the module sits over the board: centred across, its standoffs at the antenna end over the
# free strip between the resistor row (y = 22) and the jumper row (y = 27).
STANDOFF_Y = 24.5
WS_XC = W / 2


def ws_yc(ws):
    """KiCad y of the module's origin (ws: the measurement table of carrier/generate.py)."""
    return STANDOFF_Y - max(y for _, y in ws["standoffs"])


def boxes():
    """Parts as (name, x1, y1, x2, y2, height) on the top side."""
    size = {"C_0805_2012Metric": (2.0, 1.25, 1.25), "R_0805_2012Metric": (2.0, 1.25, 0.6), "SOIC-16_3.9x9.9mm_P1.27mm": (6.0, 9.9, 1.75)}
    out = []
    for ref, (pkg, x, y, rot) in sorted(PARTS.items()):
        if pkg not in size:
            continue
        a, b, h = size[pkg]                  # a along x, b along y at rotation 0
        if int(round(rot)) % 180 == 90:
            a, b = b, a
        out.append((ref, x - a / 2, y - b / 2, x + a / 2, y + b / 2, h))
    return out


if __name__ == "__main__":
    print(f"board {W:.2f} x {H:.2f} x {T:.2f} mm")
    print("mounting holes", HOLE_D, HOLES)
    for j in JACKS:
        print("jack %s: x %.3f..%.3f, y %.3f..%.3f" % (j[0], j[1], j[3], j[2], j[4]))
    print(f"jack centres x {JACK_X}, {JACK_X[1] - JACK_X[0]:.2f} apart, fronts {H - JACK_FRONT:.2f} inside the board edge, {JACKS[1][1] - JACKS[0][3]:.2f} between the bodies")
    print("J3 pads", J3)
    for b in boxes():
        print("  %-3s x %.2f..%.2f y %.2f..%.2f h %.2f" % (b[0], b[1], b[3], b[2], b[4], b[5]))
