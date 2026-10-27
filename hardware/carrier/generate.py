#!/usr/bin/env python3
"""Generate the KiCad 7 project of the RS232 isolation carrier board.

    python3 generate.py            # writes kicad/rs232_carrier.* and preview.png
    python3 generate.py --check    # only run the placement checks

The carrier takes the Waveshare ESP32-S3-Touch-LCD-2 on two 1x14 sockets and
adds four galvanically isolated RS232 ports (TLP2361 optocouplers, MAX3232E,
one B0505S isolated DC-DC per port), a VBAT->5V boost converter and a
high-side switch per port so unused ports draw nothing from the battery.

All mechanical numbers that came from measuring product photos rather than a
drawing are collected in VERIFY at the top of this file.
"""
import json
import math
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kigen import (LAYERS, SETUP, FootprintDef, Pad, SPin, SymbolDef, U, f, rect_lines)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "kicad")
PROJECT = "rs232_carrier"
SYMLIB = "rs232"
FPLIB = "rs232fp"
REV = "0.1"
DATE = "2026-10-27"

# ============================================================================
# Numbers to verify before ordering (measured from photos / from memory).
# Everything below is a single source of truth: the PCB, the firmware pin map
# and the enclosure read from here.
# ============================================================================
VERIFY = {
    # Waveshare ESP32-S3-Touch-LCD-2, portrait orientation (USB-C at the bottom), from the vendor drawing
    # (dimension sheet 2026-10-27): PCB 35.00 x 48.20, 4 x M2 on 29.00 x 42.20, 3.00 from the edges,
    # display 37.10 x 58.80 R2.60, active area 31.00 x 41.20 (7.51 from the antenna end), side view 7.20 glass->PCB
    # far face and 9.70 incl. the 2.50 mm header body, header columns 30.48 apart, RST/BOOT 18.90 apart.
    "WS_W": 35.0,            # PCB width  (mm)  - drawing
    "WS_L": 48.2,            # PCB length (mm)  - drawing
    "WS_HDR_X": (2.26, 32.74),  # x of the two header rows (left row pins 15-28, right row pins 1-14): 30.48 apart - drawing
    "WS_HDR_Y0": 7.3,        # y of the first pin (pin 28 left / pin 1 right); 14 pins at 2.54 mm. NOT in the drawing: +-0.3 (header body centred would be 7.59)
    "WS_HOLES": [(3.0, 3.0), (32.0, 3.0), (3.0, 45.2), (32.0, 45.2)],  # M2 holes - drawing
    "WS_HOLE_D": 2.2,
    "WS_USBC": (13.05, 21.95, 42.9, 50.1),   # x1 x2 y1 y2 of the USB-C shell, centred, protrudes ~1.9 mm (protrusion from photo)
    "WS_BTN_X": ((6.3, 9.8), (25.2, 28.7)),  # RST, BOOT side-actuated buttons on the bottom edge, 18.90 apart - drawing
    "WS_SD": None,            # micro-SD slot is in the middle of the board, card never reaches the edge: internal
    "WS_BAT": (23.0, 30.0, 18.5, 25.5),    # MX1.25 battery header (photo), opening faces the top edge
    "WS_GLASS": (37.1, 58.8), # touch glass W x L - drawing (R2.6 corners)
    "WS_GLASS_R": 2.6,
    "WS_GLASS_OVERHANG_L": (58.8 - 48.2) / 2,   # glass beyond the PCB at each short end, assumed centred
    "WS_STACK": 11.0,         # carrier top -> Waveshare PCB underside (8.5 socket + 2.5 header body, body confirmed by drawing)
    "WS_PCB_T": 1.6,
    "WS_DISP_T": 5.6,         # Waveshare PCB top -> glass top: 7.20 (drawing, glass face to PCB far face) - 1.6
    "WS_ANT_Y": (5.0, 13.0),  # ceramic antenna at the top edge, x range in portrait (photo)
    # RJ45 jack (generic 8P8C, side entry, pegs).  Replace with the KiCad library
    # footprint of the exact part before ordering.
    "RJ45_W": 15.9, "RJ45_D": 21.3, "RJ45_H": 13.5,
    "RJ45_ROW2_DY": -2.54,    # even pins row relative to the odd pins row
    "RJ45_PEG": (5.715, -6.35, 3.25),  # +-x, y of the two pegs, diameter
    "RJ45_FRONT": -8.9,       # front face relative to the odd pins row
    "RJ45_PROTRUDE": -0.6,    # front face 0.6 mm behind the PCB edge: the jack sits fully inside the case wall
    # B0505S-1WR3 SIP-4: pins 1,2 (in) and 3,4 (out), 2.54 pitch, 7.62 gap between 2 and 3
    "SIP4_PINS": (0.0, 2.54, 10.16, 12.7), "SIP4_BODY": (19.6, 6.0, 10.2),
    # TLP2361 SO6: rows 5.9 mm apart (pads centred +-2.95), pitch 1.27
    "SO6_ROW": 2.95,
}
V = VERIFY

# ---------------------------------------------------------------------------- board geometry
BOARD_W, BOARD_H = 78.0, 107.0
CH_PITCH = 19.0
CH_X0 = 0.9 + CH_PITCH / 2              # centre of channel 1
CH_CENTERS = [CH_X0 + i * CH_PITCH for i in range(4)]
WS_X0 = (BOARD_W - V["WS_W"]) / 2       # Waveshare (portrait, USB-C up), centred on the carrier
WS_Y0 = 1.0                             # Waveshare top edge (USB-C edge) on the carrier
BARRIER = (58.0, 63.0)                  # y band without copper between ESP side and isolated side

# GPIO assignment on the Waveshare headers (header pin -> GPIO)
HDR_A = {1: "3V3", 2: "GND", 3: "IO43", 4: "IO44", 5: "IO47", 6: "IO48", 7: "IO15", 8: "IO13",
         9: "IO11", 10: "IO12", 11: "IO14", 12: "IO9", 13: "GND", 14: "VBAT"}
HDR_B = {28: "IO2", 27: "IO4", 26: "IO6", 25: "IO16", 24: "IO17", 23: "IO18", 22: "IO21", 21: "IO8",
         20: "IO7", 19: "IO10", 18: "IO20", 17: "IO19", 16: "GND", 15: "5V"}
PORT_PINS = {   # port -> (rx gpio, tx gpio, enable gpio)
    1: (44, 21, 16),
    2: (15, 13, 17),
    3: (11, 12, 18),
    4: (14, 9, 10),
}
GPIO_NET = {}   # gpio -> net name on the carrier
for p, (rx, tx, en) in PORT_PINS.items():
    GPIO_NET[rx] = f"ESP_RX{p}"
    GPIO_NET[tx] = f"ESP_TX{p}"
    GPIO_NET[en] = f"EN{p}"
GPIO_NET.update({47: "SCL", 48: "SDA", 2: "IO2", 4: "IO4", 6: "IO6", 8: "IO8", 7: "IO7"})


def hdr_net(label: str) -> str:
    if label in ("3V3",):
        return "+3V3"
    if label in ("GND", "VBAT"):
        return label
    if label == "5V":
        return "VBUS"
    g = int(label[2:])
    return GPIO_NET.get(g, "NC")


# ---------------------------------------------------------------------------- symbols
def box(name, pins, width=10.16, prefix="U", **kw):
    return SymbolDef(name, [SPin(*p) for p in pins], "box", width, prefix, **kw)


SYMS: Dict[str, SymbolDef] = {}
for s in [
    SymbolDef("R", [SPin("1", "~", "L"), SPin("2", "~", "R")], "res", ref_prefix="R"),
    SymbolDef("L", [SPin("1", "~", "L"), SPin("2", "~", "R")], "res", ref_prefix="L"),
    SymbolDef("C", [SPin("1", "~", "L"), SPin("2", "~", "R")], "cap", ref_prefix="C"),
    SymbolDef("LED", [SPin("1", "K", "L"), SPin("2", "A", "R")], "led", ref_prefix="D"),
    SymbolDef("D", [SPin("1", "K", "L"), SPin("2", "A", "R")], "diode", ref_prefix="D"),
    SymbolDef("SolderJumper_3", [SPin("1", "A", "L"), SPin("2", "C", "B"), SPin("3", "B", "R")], "jumper3", ref_prefix="JP"),
    SymbolDef("MountingHole", [], "hole", ref_prefix="H"),
    SymbolDef("GND", [SPin("1", "GND", "B", "power_in")], "pwr", power=True, ref_prefix="#PWR"),
    SymbolDef("+3V3", [SPin("1", "+3V3", "T", "power_in")], "pwr", power=True, ref_prefix="#PWR"),
    SymbolDef("VBAT", [SPin("1", "VBAT", "T", "power_in")], "pwr", power=True, ref_prefix="#PWR"),
    SymbolDef("+5V", [SPin("1", "+5V", "T", "power_in")], "pwr", power=True, ref_prefix="#PWR"),
    SymbolDef("PWR_FLAG", [SPin("1", "pwr", "T", "power_out")], "flag", power=True, ref_prefix="#FLG"),
    box("TLP2361", [("1", "A", "L", "input", 0), ("3", "K", "L", "input", 1),
                    ("6", "VCC", "R", "power_in", 0), ("5", "VO", "R", "output", 1), ("4", "GND", "R", "power_in", 2)],
        extra_props={"Note": "10 Mbd optocoupler, SO6, pin 2 absent. Alternative: ACPL-M61L"}),
    box("MAX3232E", [("1", "C1+", "L", "passive", 0), ("3", "C1-", "L", "passive", 1), ("4", "C2+", "L", "passive", 2),
                     ("5", "C2-", "L", "passive", 3), ("2", "V+", "L", "passive", 4), ("6", "V-", "L", "passive", 5),
                     ("11", "T1IN", "L", "input", 6), ("10", "T2IN", "L", "input", 7),
                     ("16", "VCC", "R", "power_in", 0), ("15", "GND", "R", "power_in", 1),
                     ("14", "T1OUT", "R", "output", 2), ("13", "R1IN", "R", "input", 3),
                     ("12", "R1OUT", "R", "output", 4), ("7", "T2OUT", "R", "output", 5),
                     ("8", "R2IN", "R", "input", 6), ("9", "R2OUT", "R", "output", 7)], width=12.7),
    box("74LVC1G17", [("2", "A", "L", "input", 0), ("1", "NC", "L", "no_connect", 1),
                      ("5", "VCC", "R", "power_in", 0), ("4", "Y", "R", "output", 1), ("3", "GND", "R", "power_in", 2)]),
    box("B0505S-1WR3", [("2", "+Vin", "L", "power_in", 0), ("1", "-Vin", "L", "power_in", 1),
                        ("4", "+Vout", "R", "power_out", 0), ("3", "-Vout", "R", "power_out", 1)], prefix="PS",
        extra_props={"Note": "1 W isolated DC-DC, SIP-4, 1.5 kV"}),
    box("RJ45", [(str(i), f"P{i}", "L", "passive", i - 1) for i in range(1, 9)], prefix="J"),
    box("MT3608", [("5", "IN", "L", "power_in", 0), ("4", "EN", "L", "input", 1), ("3", "FB", "L", "input", 2),
                   ("1", "SW", "R", "passive", 0), ("6", "NC", "R", "no_connect", 1), ("2", "GND", "R", "power_in", 2)]),
    box("Q_NMOS", [("1", "G", "L", "input", 0), ("3", "D", "R", "passive", 0), ("2", "S", "R", "passive", 1)], prefix="Q"),
    box("Q_PMOS", [("1", "G", "L", "input", 0), ("2", "S", "R", "passive", 0), ("3", "D", "R", "passive", 1)], prefix="Q"),
    box("PinSocket_1x14", [(str(i), f"{i}", "L", "passive", i - 1) for i in range(1, 15)], prefix="J"),
    box("PinSocket_1x14_B", [(str(28 - i), f"{28 - i}", "L", "passive", i) for i in range(14)], prefix="J"),
    box("Conn_1x7", [(str(i), f"{i}", "L", "passive", i - 1) for i in range(1, 8)], prefix="J"),
    box("Conn_1x2", [("1", "BAT-", "L", "passive", 0), ("2", "BAT+", "L", "passive", 1)], prefix="J"),
]:
    SYMS[s.name] = s

# ---------------------------------------------------------------------------- footprints
FPS: Dict[str, FootprintDef] = {}


def two_pad(name, px, pw, ph, cy, body, h=0.6, polar=False, note=""):
    silk = []
    if polar:
        silk.append((-px - pw / 2 - 0.3, -ph / 2 - 0.3, -px - pw / 2 - 0.3, ph / 2 + 0.3))
    FPS[name] = FootprintDef(name, [Pad("1", -px, 0, pw, ph), Pad("2", px, 0, pw, ph)], (-cy[0], -cy[1], cy[0], cy[1]),
                             fab=(-body[0] / 2, -body[1] / 2, body[0] / 2, body[1] / 2), silk_lines=silk, height=h, note=note)


two_pad("R_0603", 0.7875, 0.875, 0.95, (1.48, 0.73), (1.6, 0.8))
two_pad("C_0603", 0.7875, 0.875, 0.95, (1.48, 0.73), (1.6, 0.8))
two_pad("LED_0603", 0.7875, 0.875, 0.95, (1.48, 0.73), (1.6, 0.8), polar=True)
two_pad("C_0805", 0.9125, 1.025, 1.45, (1.68, 0.98), (2.0, 1.25))
two_pad("D_SOD-123", 1.6, 1.0, 1.4, (2.3, 1.0), (2.8, 1.8), h=1.2, polar=True)
two_pad("D_SMA", 2.0, 2.5, 1.8, (3.5, 1.5), (4.3, 2.6), h=2.3, polar=True)
two_pad("L_5x5", 1.9, 2.0, 4.5, (2.9, 2.9), (5.0, 5.0), h=3.0, note="generic 4x4/5x5 mm shielded power inductor")


def sot23(name, pins, note=""):
    pads = []
    for num, (x, y) in pins.items():
        pads.append(Pad(num, x, y, 1.1, 0.6 if len(pins) > 3 else 0.9))
    FPS[name] = FootprintDef(name, pads, (-1.9, -1.7, 1.9, 1.7), fab=(-0.7, -1.5, 0.7, 1.5),
                             silk_lines=[(-0.7, -1.7, 0.7, -1.7), (-0.7, 1.7, 0.7, 1.7)], height=1.2, note=note)


sot23("SOT-23", {"1": (-1.05, -0.95), "2": (-1.05, 0.95), "3": (1.05, 0)})
sot23("SOT-23-5", {"1": (-1.05, -0.95), "2": (-1.05, 0), "3": (-1.05, 0.95), "4": (1.05, 0.95), "5": (1.05, -0.95)})
sot23("SOT-23-6", {"1": (-1.05, -0.95), "2": (-1.05, 0), "3": (-1.05, 0.95), "4": (1.05, 0.95), "5": (1.05, 0), "6": (1.05, -0.95)})

# SOIC-16 narrow: pins 1-8 left top->bottom, 9-16 right bottom->top
pads = []
for i in range(8):
    y = -4.445 + 1.27 * i
    pads.append(Pad(str(i + 1), -2.7, y, 1.6, 0.6))
    pads.append(Pad(str(16 - i), 2.7, y, 1.6, 0.6))
FPS["SOIC-16"] = FootprintDef("SOIC-16", pads, (-3.75, -5.3, 3.75, 5.3), fab=(-1.95, -4.95, 1.95, 4.95),
                              silk_lines=[(-1.95, -5.1, 1.95, -5.1), (-1.95, 5.1, 1.95, 5.1), (-3.6, -5.1, -3.6, -4.6)], height=1.6)

# SO6 optocoupler, 5 leads: 1,3 top row (left,right), 4,5,6 bottom row (right->left)
r = V["SO6_ROW"]
FPS["SO6_Opto"] = FootprintDef("SO6_Opto", [Pad("1", -1.27, -r, 0.8, 1.6), Pad("3", 1.27, -r, 0.8, 1.6),
                                           Pad("4", 1.27, r, 0.8, 1.6), Pad("5", 0, r, 0.8, 1.6), Pad("6", -1.27, r, 0.8, 1.6)],
                               (-2.4, -4.0, 2.4, 4.0), fab=(-2.3, -1.8, 2.3, 1.8),
                               silk_lines=[(-2.4, -1.85, 2.4, -1.85), (-2.4, 1.85, 2.4, 1.85), (-2.4, -1.85, -2.4, -1.0)], height=2.2,
                               note="TLP2361 / ACPL-M61L, pin 2 absent")

# SIP-4 isolated DC-DC, pin 1 at the origin, pins along +x
px = V["SIP4_PINS"]
bl, bw, bh = V["SIP4_BODY"]
x0 = px[0] - (bl - (px[3] - px[0])) / 2
FPS["SIP-4_DCDC"] = FootprintDef("SIP-4_DCDC", [Pad(str(i + 1), px[i], 0, 1.7, 1.7, "tht", "circle" if i else "rect", 0.9) for i in range(4)],
                                 (x0 - 0.2, -bw / 2 - 0.1, x0 + bl + 0.2, bw / 2 + 0.1), fab=(x0, -bw / 2, x0 + bl, bw / 2),
                                 silk_lines=rect_lines(x0 - 0.1, -bw / 2 - 0.1, x0 + bl + 0.1, bw / 2 + 0.1), attr="through_hole", height=bh,
                                 note="Mornsun B0505S-1WR3 or pin compatible")

# RJ45 (generic). Origin = odd pin row centre.
w, d = V["RJ45_W"], V["RJ45_D"]
front = V["RJ45_FRONT"]
rear = front + d
rj = [Pad(str(i + 1), -4.445 + 1.27 * i, 0 if i % 2 == 0 else V["RJ45_ROW2_DY"], 1.5, 1.5, "tht", "circle", 0.9) for i in range(8)]
pgx, pgy, pgd = V["RJ45_PEG"]
rj += [Pad("", -pgx, pgy, pgd, pgd, "npth", drill=pgd), Pad("", pgx, pgy, pgd, pgd, "npth", drill=pgd)]
FPS["RJ45_Generic"] = FootprintDef("RJ45_Generic", rj, (-w / 2 - 0.3, front - 0.3, w / 2 + 0.3, rear + 0.3), fab=(-w / 2, front, w / 2, rear),
                                   silk_lines=[(-w / 2 - 0.1, front + 2.5, -w / 2 - 0.1, rear + 0.1), (w / 2 + 0.1, front + 2.5, w / 2 + 0.1, rear + 0.1),
                                               (-w / 2 - 0.1, rear + 0.1, w / 2 + 0.1, rear + 0.1)], attr="through_hole",
                                   height=V["RJ45_H"], note="VERIFY against the chosen jack; swap for the KiCad library footprint")


def socket(name, n, numbers):
    pads = [Pad(numbers[i], 2.54 * i, 0, 1.7, 1.7, "tht", "rect" if i == 0 else "circle", 1.0) for i in range(n)]
    L = 2.54 * n
    FPS[name] = FootprintDef(name, pads, (-1.6, -1.6, L - 2.54 + 1.6, 1.6), fab=(-1.27, -1.27, L - 2.54 + 1.27, 1.27),
                             silk_lines=rect_lines(-1.33, -1.33, L - 2.54 + 1.33, 1.33), attr="through_hole", height=8.5)


socket("PinSocket_1x14", 14, [str(i) for i in range(1, 15)])
socket("PinSocket_1x14_B", 14, [str(28 - i) for i in range(14)])
socket("PinHeader_1x7", 7, [str(i) for i in range(1, 8)])

FPS["JST_PH_2_Vertical"] = FootprintDef("JST_PH_2_Vertical", [Pad("1", 0, 0, 1.2, 1.2, "tht", "rect", 0.7), Pad("2", 2.0, 0, 1.2, 1.2, "tht", "circle", 0.7)],
                                        (-2.2, -2.3, 4.2, 2.3), fab=(-1.95, -1.7, 3.95, 1.7), silk_lines=rect_lines(-2.05, -1.8, 4.05, 1.8),
                                        attr="through_hole", height=6.0, note="JST B2B-PH-K-S, top entry, mounted on the bottom side")
FPS["SolderJumper_3_Bridged12"] = FootprintDef(
    "SolderJumper_3_Bridged12", [Pad("1", -1.3, 0, 1.0, 1.5, shape="rect"), Pad("2", 0, 0, 1.0, 1.5, shape="rect"), Pad("3", 1.3, 0, 1.0, 1.5, shape="rect")],
    (-2.1, -1.0, 2.1, 1.0), silk_lines=rect_lines(-2.0, -0.95, 2.0, 0.95),
    polys=[("F.Cu", [(-1.3, -0.3), (0, -0.3), (0, 0.3), (-1.3, 0.3)])], net_tie="1,2", height=0.1)
FPS["MountingHole_2.2"] = FootprintDef("MountingHole_2.2", [Pad("", 0, 0, 2.2, 2.2, "npth", drill=2.2)], (-2.0, -2.0, 2.0, 2.0),
                                       silk_lines=[], attr="through_hole", height=0)
FPS["MountingHole_2.7"] = FootprintDef("MountingHole_2.7", [Pad("", 0, 0, 2.7, 2.7, "npth", drill=2.7)], (-2.3, -2.3, 2.3, 2.3),
                                       silk_lines=[], attr="through_hole", height=0)


# ---------------------------------------------------------------------------- design model
@dataclass
class Part:
    ref: str
    sym: str
    fp: str
    value: str
    pins: Dict[str, str]
    x: float = 0.0
    y: float = 0.0
    rot: float = 0.0
    sx: float = 0.0
    sy: float = 0.0
    dnp: bool = False
    props: Dict[str, str] = field(default_factory=dict)
    side: str = "F"


# ----- channel template (ref suffix only; positions relative to the channel centre)
def channel_parts() -> List[Part]:
    P = Part
    return [
        # ESP side
        P("R01", "R", "R_0603", "330", {"1": "+3V3", "2": "TXA"}, -6.0, 52, 90, 25.4, 38.1, props={"Note": "TX LED, ~5 mA at 3.3 V"}),
        P("U01", "TLP2361", "SO6_Opto", "TLP2361", {"1": "TXA", "3": "ESP_TX", "6": "ISO5V", "5": "T1IN", "4": "ISOGND"}, -5.0, 60.5, 0, 50.8, 38.1),
        P("U02", "TLP2361", "SO6_Opto", "TLP2361", {"1": "RXA", "3": "BUF_OUT", "6": "+3V3", "5": "RX_O", "4": "GND"}, 0.0, 60.5, 180, 50.8, 63.5),
        P("R03", "R", "R_0603", "100", {"1": "RX_O", "2": "ESP_RX"}, 1.5, 52, 90, 25.4, 63.5),
        P("C07", "C", "C_0603", "100n", {"1": "+3V3", "2": "GND"}, -2.3, 52, 90, 25.4, 76.2),
        # isolated supply
        P("PS01", "B0505S-1WR3", "SIP-4_DCDC", "B0505S-1WR3", {"2": "ISO_IN", "1": "GND", "4": "ISO5V", "3": "ISOGND"}, 7.2, 55, 270, 50.8, 96.52),
        P("C10", "C", "C_0805", "2.2u", {"1": "ISO_IN", "2": "GND"}, 7.2, 49.6, 180, 25.4, 96.52),
        P("C09", "C", "C_0603", "4.7u", {"1": "ISO5V", "2": "ISOGND"}, 3.3, 70.2, 90, 78.74, 104.14),
        P("C05", "C", "C_0603", "100n", {"1": "ISO5V", "2": "ISOGND"}, 3.3, 66.6, 90, 96.52, 91.44),
        # isolated side
        P("U03", "MAX3232E", "SOIC-16", "MAX3232E", {"1": "C1P", "3": "C1N", "4": "C2P", "5": "C2N", "2": "VP", "6": "VN",
                                                      "11": "T1IN", "10": "ISO5V", "16": "ISO5V", "15": "ISOGND", "14": "T1OUT",
                                                      "13": "R1IN", "12": "R1OUT", "7": "NC", "8": "NC", "9": "NC"}, -2.4, 73, 0, 134.62, 45.72),
        P("C01", "C", "C_0603", "100n", {"1": "C1P", "2": "C1N"}, -7.6, 69.83, 270, 96.52, 40.64),
        P("C02", "C", "C_0603", "100n", {"1": "C2P", "2": "C2N"}, -7.6, 73, 270, 96.52, 53.34),
        P("C03", "C", "C_0603", "100n", {"1": "VP", "2": "ISOGND"}, -7.6, 66.8, 90, 96.52, 66.04),
        P("C04", "C", "C_0603", "100n", {"1": "VN", "2": "ISOGND"}, -7.6, 76.2, 270, 96.52, 78.74),
        P("U04", "74LVC1G17", "SOT-23-5", "74LVC1G17", {"2": "R1OUT", "1": "NC", "5": "ISO5V", "4": "BUF_OUT", "3": "ISOGND"}, 5.9, 75.5, 0, 134.62, 88.9),
        P("C08", "C", "C_0603", "100n", {"1": "ISO5V", "2": "ISOGND"}, 5.9, 78.5, 0, 96.52, 104.14),
        P("R04", "R", "R_0603", "2k2", {"1": "ISO5V", "2": "LEDA"}, 5.7, 80.4, 0, 134.62, 106.68),
        P("D01", "LED", "LED_0603", "LED green", {"2": "LEDA", "1": "ISOGND"}, 5.4, 82.4, 0, 154.94, 106.68),
        P("R02", "R", "R_0603", "680", {"1": "ISO5V", "2": "RXA"}, -2.0, 79.6, 0, 78.74, 71.12, props={"Note": "RX LED, ~5 mA at 5 V"}),
        P("JP01", "SolderJumper_3", "SolderJumper_3_Bridged12", "TX->3", {"1": "RJ3", "2": "T1OUT", "3": "RJ6"}, -4.6, 82.4, 0, 167.64, 45.72),
        P("JP02", "SolderJumper_3", "SolderJumper_3_Bridged12", "RX<-6", {"1": "RJ6", "2": "R1IN", "3": "RJ3"}, 0.8, 82.4, 0, 167.64, 60.96),
        P("J01", "RJ45", "RJ45_Generic", "RJ45", {"1": "NC", "2": "NC", "3": "RJ3", "4": "ISOGND", "5": "ISOGND", "6": "RJ6", "7": "NC", "8": "NC"},
          0.0, 0.0, 180, 195.58, 48.26),
    ]


CH_LOCAL_NETS = {"TXA", "RXA", "T1IN", "R1OUT", "R1IN", "T1OUT", "BUF_OUT", "RX_O", "RJ3", "RJ6", "ISO5V", "ISOGND",
                 "C1P", "C1N", "C2P", "C2N", "VP", "VN", "LEDA"}
CH_PORTS = {"ESP_TX": "input", "ESP_RX": "output", "ISO_IN": "input"}
CH_ESP_LOCALS = {"TXA", "RX_O"}            # channel-local nets that live on the ESP side of the optocouplers
CH_ISO_LOCALS = CH_LOCAL_NETS - CH_ESP_LOCALS
MOUNT_HOLES = [(3.5, 45.0), (75.0, 45.0), (20.0, 82.0), (58.0, 82.0)]   # M2.5, carrier -> enclosure + bezel spacers
HOLE_KEEPOUT_R = {"M2.5": 3.0, "M2": 2.6}          # no copper under the metal spacers
GLOBAL_POWER = {"GND", "+3V3", "VBAT", "+5V"}


def root_parts() -> List[Part]:
    P = Part
    parts = [
        P("J1", "PinSocket_1x14", "PinSocket_1x14", "WS header 1-14", {str(k): hdr_net(v) for k, v in HDR_A.items()},
          *ws_to_carrier(V["WS_HDR_X"][1], V["WS_HDR_Y0"]), 90, 30.48, 45.72, props={"Note": "Waveshare right header (3V3 .. VBAT)"}),
        P("J2", "PinSocket_1x14_B", "PinSocket_1x14_B", "WS header 15-28", {str(k): hdr_net(v) for k, v in HDR_B.items()},
          *ws_to_carrier(V["WS_HDR_X"][0], V["WS_HDR_Y0"]), 90, 30.48, 101.6, props={"Note": "Waveshare left header (GPIO2 .. 5V)"}),
        P("J3", "Conn_1x7", "PinHeader_1x7", "EXT", {"1": "+3V3", "2": "GND", "3": "SDA", "4": "SCL", "5": "IO2", "6": "IO4", "7": "IO6"},
          2.5, 2.0, 0, 30.48, 157.48),
        # boost converter VBAT -> +5V
        P("U5", "MT3608", "SOT-23-6", "MT3608", {"5": "VBAT", "4": "PWR_EN", "3": "FB", "1": "SW", "6": "NC", "2": "GND"}, 10.5, 8.0, 0, 96.52, 45.72),
        P("L1", "L", "L_5x5", "22u", {"1": "VBAT", "2": "SW"}, 6.5, 14.0, 0, 96.52, 27.94),
        P("D5", "D", "D_SMA", "SS14", {"2": "SW", "1": "+5V"}, 14.5, 13.5, 0, 121.92, 27.94),
        P("C51", "C", "C_0805", "10u", {"1": "VBAT", "2": "GND"}, 4.0, 8.0, 90, 71.12, 27.94),
        P("C52", "C", "C_0805", "22u", {"1": "+5V", "2": "GND"}, 15.5, 19.5, 90, 147.32, 27.94),
        P("C53", "C", "C_0805", "22u", {"1": "+5V", "2": "GND"}, 15.5, 24.0, 90, 147.32, 38.1),
        P("C54", "C", "C_0603", "100n", {"1": "VBAT", "2": "GND"}, 16.5, 8.0, 90, 71.12, 38.1),
        P("R51", "R", "R_0603", "75k", {"1": "+5V", "2": "FB"}, 4.5, 19.5, 0, 121.92, 60.96, props={"Note": "Vout = 0.6 V * (1 + R51/R52) = 5.1 V"}),
        P("R52", "R", "R_0603", "10k", {"1": "FB", "2": "GND"}, 10.0, 19.5, 0, 121.92, 71.12),
        P("R53", "R", "R_0603", "100k", {"1": "PWR_EN", "2": "GND"}, 4.5, 25.0, 0, 71.12, 60.96),
    ]
    for n in range(1, 5):
        parts.append(P(f"D5{n}", "D", "D_SOD-123", "1N4148W", {"2": f"EN{n}", "1": "PWR_EN"}, 7.0 + 4.0 * (n - 1), 30.0, 90, 71.12, 76.2 + 10.16 * (n - 1)))
    rows = [6.0, 16.0, 26.0, 36.0]
    for n in range(1, 5):
        y = rows[n - 1]
        sy = 109.22 + 17.78 * (n - 1)
        parts += [
            P(f"JP1{n}", "SolderJumper_3", "SolderJumper_3_Bridged12", f"EN{n} dflt", {"1": "GND", "2": f"ENP{n}", "3": "+3V3"}, 62.0, y, 0, 71.12, sy,
              props={"Note": "1-2: port off until the firmware enables it, 2-3: always on"}),
            P(f"R6{n}", "R", "R_0603", "10k", {"1": f"EN{n}", "2": f"ENP{n}"}, 67.0, y, 0, 96.52, sy),
            P(f"Q{n}1", "Q_NMOS", "SOT-23", "2N7002", {"1": f"EN{n}", "3": f"PG{n}", "2": "GND"}, 72.0, y, 180, 121.92, sy),
            P(f"R7{n}", "R", "R_0603", "100k", {"1": "+5V", "2": f"PG{n}"}, 67.0, y + 4.5, 0, 147.32, sy),
            P(f"Q{n}2", "Q_PMOS", "SOT-23", "AO3401A", {"1": f"PG{n}", "2": "+5V", "3": f"ISO_IN{n}"}, 72.0, y + 4.5, 0, 172.72, sy),
        ]
    parts.append(P("J4", "Conn_1x2", "JST_PH_2_Vertical", "BAT JST-PH", {"1": "GND", "2": "VBAT"}, 10.0, 40.0, 0, 30.48, 175.26,
                   props={"Note": "battery, bottom side (in the battery bay). VBAT reaches the module through header pin 14 - verify that pin is the cell terminal"}, side="B"))
    # mounting holes: 4 x M2 at the Waveshare positions (standoffs to the module), 5 x M2.5 to the enclosure
    for i, (hx, hy) in enumerate(ws_holes_landscape()):
        parts.append(P(f"H{i + 1}", "MountingHole", "MountingHole_2.2", "M2 Waveshare", {}, hx, hy, 0, 30.48 + 12.7 * i, 185.42))
    for i, (hx, hy) in enumerate(MOUNT_HOLES):
        parts.append(P(f"H{i + 5}", "MountingHole", "MountingHole_2.7", "M2.5 enclosure", {}, hx, hy, 0, 30.48 + 12.7 * i, 195.58))
    return parts


def hole_keepouts():
    """(x, y, r) of the copper keep-outs under the metal spacers."""
    return [(x, y, HOLE_KEEPOUT_R["M2.5"]) for x, y in MOUNT_HOLES] + [(x, y, HOLE_KEEPOUT_R["M2"]) for x, y in ws_holes_landscape()]


def ws_to_carrier(px: float, py: float) -> Tuple[float, float]:
    """Waveshare portrait coordinates (x right, y down, USB-C at the bottom, as in the product photo)
    -> carrier coordinates.  The module sits in portrait, rotated 180 deg: USB-C and buttons at the
    top edge of the carrier, SD slot on the left edge, antenna end towards the ports."""
    return (WS_X0 + V["WS_W"] - px, WS_Y0 + V["WS_L"] - py)


def ws_holes_landscape():
    return [ws_to_carrier(x, y) for x, y in V["WS_HOLES"]]


# ---------------------------------------------------------------------------- schematic writer
class Sheet:
    def __init__(self, name: str, uid: str, parts: List[Part], local_nets: set, ports: Dict[str, str], project: str,
                 instance_paths: List[Tuple[str, Dict[str, str]]], title: str):
        self.name, self.uid, self.parts = name, uid, parts
        self.local_nets, self.ports, self.project = local_nets, ports, project
        self.instance_paths = instance_paths   # [(path, {template_ref: instance_ref})]
        self.title = title
        self.items: List[str] = []
        self.used_syms: set = set()
        self.power_counter = 0

    # ---- helpers
    def wire(self, x1, y1, x2, y2):
        self.items.append(f"  (wire (pts (xy {f(x1)} {f(y1)}) (xy {f(x2)} {f(y2)})) (stroke (width 0) (type default)) (uuid {U(self.name + 'w' + str(len(self.items)))}))")

    def label(self, net, x, y, ang, kind):
        just = {0: "left", 180: "right", 90: "left", 270: "right"}[ang]
        uid = U(self.name + 'l' + net + str(x) + str(y))
        if kind == "local":
            self.items.append(f'  (label "{net}" (at {f(x)} {f(y)} {ang}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid {uid}))')
        elif kind == "global":
            self.items.append(f'  (global_label "{net}" (shape passive) (at {f(x)} {f(y)} {ang}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {just})) (uuid {uid})\n'
                              f'    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide))\n  )')
        else:
            self.items.append(f'  (hierarchical_label "{net}" (shape {self.ports[net]}) (at {f(x)} {f(y)} {ang}) (fields_autoplaced) (effects (font (size 1.27 1.27)) (justify {just})) (uuid {uid}))')

    def no_connect(self, x, y):
        self.items.append(f"  (no_connect (at {f(x)} {f(y)}) (uuid {U(self.name + 'nc' + str(x) + str(y))}))")

    def net_kind(self, net: str) -> str:
        if net in self.ports:
            return "hier"
        if net in self.local_nets:
            return "local"
        return "global"

    def place(self, p: Part):
        s = SYMS[p.sym]
        self.used_syms.add(p.sym)
        sym_uid = U(self.name + ":" + p.ref)
        o = [f'  (symbol (lib_id "{SYMLIB}:{p.sym}") (at {f(p.sx)} {f(p.sy)} 0) (unit 1)',
             f"    (in_bom {'no' if s.power or s.kind == 'hole' else 'yes'}) (on_board yes) (dnp {'yes' if p.dnp else 'no'}) (fields_autoplaced)",
             f"    (uuid {sym_uid})"]
        h = s.box_height() if s.kind == "box" else 5.08
        hide = " hide" if s.power else ""
        o.append(f'    (property "Reference" "{p.ref}" (at {f(p.sx + s.width/2 + 1.27 if s.kind == "box" else p.sx)} {f(p.sy - h/2 - 1.27)} 0) (effects (font (size 1.27 1.27)) (justify left){hide}))')
        o.append(f'    (property "Value" "{p.value}" (at {f(p.sx + s.width/2 + 1.27 if s.kind == "box" else p.sx)} {f(p.sy + h/2 + 1.27)} 0) (effects (font (size 1.27 1.27)) (justify left)))')
        o.append(f'    (property "Footprint" "{FPLIB}:{p.fp}" (at {f(p.sx)} {f(p.sy)} 0) (effects (font (size 1.27 1.27)) hide))')
        o.append(f'    (property "Datasheet" "" (at {f(p.sx)} {f(p.sy)} 0) (effects (font (size 1.27 1.27)) hide))')
        for k, v in p.props.items():
            o.append(f'    (property "{k}" "{v}" (at {f(p.sx)} {f(p.sy)} 0) (effects (font (size 1.27 1.27)) hide))')
        for pin in s.pins:
            o.append(f'    (pin "{pin.number}" (uuid {U(sym_uid + "p" + pin.number)}))')
        inst = []
        for path, refmap in self.instance_paths:
            inst.append(f'      (path "{path}" (reference "{refmap.get(p.ref, p.ref)}") (unit 1))')
        o.append(f'    (instances (project "{self.project}"\n' + "\n".join(inst) + "\n    ))")
        o.append("  )")
        self.items.append("\n".join(o))
        # stubs + labels
        for pin in s.pins:
            net = p.pins.get(pin.number)
            if net is None:
                continue
            px, py, ang = s.pin_pos(pin)
            x, y = p.sx + px, p.sy - py
            if net == "NC":
                self.no_connect(x, y)
                continue
            dx, dy = {0: (-2.54, 0), 180: (2.54, 0), 90: (0, 2.54), 270: (0, -2.54)}[ang]
            self.wire(x, y, x + dx, y + dy)
            lang = {0: 180, 180: 0, 90: 270, 270: 90}[ang]
            self.label(net, x + dx, y + dy, lang, self.net_kind(net))

    def power_symbol(self, net: str, x: float, y: float, flag=False):
        name = "PWR_FLAG" if flag else net
        s = SYMS[name]
        self.used_syms.add(name)
        self.power_counter += 1
        ref = f"#FLG{self.power_counter:02d}" if flag else f"#PWR{self.power_counter:02d}"
        uid = U(self.name + "pwr" + ref)
        o = [f'  (symbol (lib_id "{SYMLIB}:{name}") (at {f(x)} {f(y)} 0) (unit 1)', "    (in_bom no) (on_board yes) (dnp no)", f"    (uuid {uid})",
             f'    (property "Reference" "{ref}" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide))',
             f'    (property "Value" "{name}" (at {f(x + 2.54)} {f(y)} 0) (effects (font (size 1.27 1.27)) (justify left)))',
             f'    (property "Footprint" "" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide))',
             f'    (property "Datasheet" "" (at {f(x)} {f(y)} 0) (effects (font (size 1.27 1.27)) hide))',
             f'    (pin "1" (uuid {U(uid + "p1")}))']
        inst = "\n".join(f'      (path "{path}" (reference "{ref}") (unit 1))' for path, _ in self.instance_paths)
        o.append(f'    (instances (project "{self.project}"\n{inst}\n    ))')
        o.append("  )")
        self.items.append("\n".join(o))

    def text(self, s: str, x: float, y: float, size=2.0):
        self.items.append(f'  (text "{s}" (at {f(x)} {f(y)} 0) (effects (font (size {size} {size})) (justify left bottom)) (uuid {U(self.name + "t" + s)}))')

    def sheet_symbol(self, name: str, file: str, uid: str, x: float, y: float, w: float, h: float, pins: Dict[str, Tuple[str, str, str]], page: str):
        """pins: name -> (shape, side 'L'/'R', root net)"""
        o = [f"  (sheet (at {f(x)} {f(y)}) (size {f(w)} {f(h)}) (fields_autoplaced)", "    (stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000))",
             f"    (uuid {uid})",
             f'    (property "Sheetname" "{name}" (at {f(x)} {f(y - 0.7)} 0) (effects (font (size 1.27 1.27)) (justify left bottom)))',
             f'    (property "Sheetfile" "{file}" (at {f(x)} {f(y + h + 0.6)} 0) (effects (font (size 1.27 1.27)) (justify left top)))']
        li = ri = 0
        for pn, (shape, side, net) in pins.items():
            if side == "L":
                li += 1
                px, py, ang, just = x, y + 2.54 * li, 180, "right"
            else:
                ri += 1
                px, py, ang, just = x + w, y + 2.54 * ri, 0, "left"
            o.append(f'    (pin "{pn}" {shape} (at {f(px)} {f(py)} {ang}) (effects (font (size 1.27 1.27)) (justify {just})) (uuid {U(uid + pn)}))')
            dx = -2.54 if side == "L" else 2.54
            self.wire(px, py, px + dx, py)
            self.label(net, px + dx, py, 180 if side == "L" else 0, "global")
        o.append(f'    (instances (project "{self.project}" (path "/{self.instance_paths[0][0].strip("/")}" (page "{page}"))))')
        o.append("  )")
        self.items.append("\n".join(o))

    def write(self, path: str, page: str):
        libs = "\n".join(SYMS[n].sexp(SYMLIB) for n in sorted(self.used_syms))
        body = "\n".join(self.items)
        with open(path, "w") as fh:
            fh.write(f"(kicad_sch (version 20230121) (generator rs232_generate)\n\n  (uuid {self.uid})\n\n  (paper \"A3\")\n\n"
                     f'  (title_block\n    (title "{self.title}")\n    (date "{DATE}")\n    (rev "{REV}")\n    (comment 1 "generated by hardware/carrier/generate.py")\n  )\n\n'
                     f"  (lib_symbols\n{libs}\n  )\n\n{body}\n\n  (sheet_instances\n    (path \"/\" (page \"{page}\"))\n  )\n)\n")


# ---------------------------------------------------------------------------- PCB writer
def write_pcb(path: str, placed: List[Tuple[Part, Dict[str, str], str]], nets: List[str]):
    """placed: (part with absolute PCB coords, pin->full net name, schematic path)"""
    net_idx = {n: i + 1 for i, n in enumerate(nets)}
    o = [f"(kicad_pcb (version 20221018) (generator rs232_generate)\n\n  (general\n    (thickness 1.6)\n  )\n\n  (paper \"A4\")",
         f'  (title_block\n    (title "RS232 isolation carrier for Waveshare ESP32-S3-Touch-LCD-2")\n    (date "{DATE}")\n    (rev "{REV}")\n  )',
         LAYERS, SETUP, '  (net 0 "")']
    for n in nets:
        o.append(f'  (net {net_idx[n]} "{n}")')
    for p, pin_nets, spath in placed:
        fp = FPS[p.fp]
        nm = {k: (net_idx[v], v) for k, v in pin_nets.items() if v != "NC"}
        o.append(fp.sexp(FPLIB, p.ref, p.value, p.x, p.y, p.rot, nm, spath, U("fp:" + spath), p.dnp, p.props, p.side))
    # board outline
    W, H = BOARD_W, BOARD_H
    for i, (x1, y1, x2, y2) in enumerate(rect_lines(0, 0, W, H)):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (tstamp {U("edge" + str(i))}))')
    # battery cable slot (closed contour on Edge.Cuts)
    sx1, sy1, sx2, sy2 = BAT_SLOT
    for i, (x1, y1, x2, y2) in enumerate(rect_lines(sx1, sy1, sx2, sy2)):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (tstamp {U("slot" + str(i))}))')
    # Waveshare outline + glass + keep-out height note on User.Comments
    wx1, wy1 = WS_X0, WS_Y0
    wx2, wy2 = WS_X0 + V["WS_W"], WS_Y0 + V["WS_L"]
    for i, (x1, y1, x2, y2) in enumerate(rect_lines(wx1, wy1, wx2, wy2)):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.15) (type dash)) (layer "Cmts.User") (tstamp {U("ws" + str(i))}))')
    gy1 = wy1 - V["WS_GLASS_OVERHANG_L"]
    gy2 = wy2 + V["WS_GLASS_OVERHANG_L"]
    gx1 = (wx1 + wx2) / 2 - V["WS_GLASS"][0] / 2
    gx2 = (wx1 + wx2) / 2 + V["WS_GLASS"][0] / 2
    for i, (x1, y1, x2, y2) in enumerate(rect_lines(gx1, gy1, gx2, gy2)):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type dot)) (layer "Cmts.User") (tstamp {U("gl" + str(i))}))')
    ux1, ux2, uy1, uy2 = V["WS_USBC"]
    ax, ay = ws_to_carrier(ux1, uy1)
    bx, by = ws_to_carrier(ux2, uy2)
    for i, (x1, y1, x2, y2) in enumerate(rect_lines(min(ax, bx), min(ay, by), max(ax, bx), max(ay, by))):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type default)) (layer "Cmts.User") (tstamp {U("usb" + str(i))}))')

    def text(s, x, y, layer="F.SilkS", size=1.0, rot=0, just="left"):
        words = [w for w in (("" if just == "center" else just), ("mirror" if layer.startswith("B.") else "")) if w]
        jst = f" (justify {' '.join(words)})" if words else ""
        o.append(f'  (gr_text "{s}" (at {f(x)} {f(y)} {rot}) (layer "{layer}") (tstamp {U("gt" + s + str(x) + str(y))})\n'
                 f"    (effects (font (size {size} {size}) (thickness {f(size * 0.15)})){jst})\n  )")

    text("Waveshare ESP32-S3-Touch-LCD-2, USB-C up", wx1 + 1, wy1 + 20, "Cmts.User", 1.0, 90)
    text(f"RS232 ISO CARRIER v{REV} {DATE} nikor30/rs232_mobile", 39.0, 54.5, "B.SilkS", 1.2, 0, "center")
    text("ISOLATION BARRIER - no copper, 1.5 kV", 39.0, 60.8, "B.SilkS", 1.0, 0, "center")
    for i, xc in enumerate(CH_CENTERS):
        text(f"PORT {i + 1}", xc - 2.8, H - 21.5, "F.SilkS", 1.0)
        text("Cisco: 3=TX 6=RX 4/5=GND", xc - 7.5, H - 22.9, "F.SilkS", 0.6)
    text("BAT cable", BAT_SLOT[0] - 0.5, BAT_SLOT[3] + 1.5, "F.SilkS", 0.7)
    # zones
    def zone(net, layer, pts, name="", keepout=False):
        p = " ".join(f"(xy {f(a)} {f(b)})" for a, b in pts)
        if keepout:
            o.append(f'  (zone (net 0) (net_name "") (layers "F&B.Cu") (tstamp {U("kz" + str(pts))}) (name "{name}") (hatch edge 0.5)\n'
                     f"    (connect_pads (clearance 0))\n    (min_thickness 0.25) (filled_areas_thickness no)\n"
                     f"    (keepout (tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n"
                     f"    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))\n    (polygon (pts {p}))\n  )")
        else:
            o.append(f'  (zone (net {net_idx[net]}) (net_name "{net}") (layer "{layer}") (tstamp {U("z" + net + layer)}) (name "{name}") (hatch edge 0.5)\n'
                     f"    (connect_pads (clearance 0.25))\n    (min_thickness 0.25) (filled_areas_thickness no)\n"
                     f"    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))\n    (polygon (pts {p}))\n  )")

    b1, b2 = BARRIER
    zone("GND", "B.Cu", [(0.5, 0.5), (W - 0.5, 0.5), (W - 0.5, b1), (0.5, b1)], "GND plane (ESP side)")
    zone(None, "", [(0.5, b1), (W - 0.5, b1), (W - 0.5, b2), (0.5, b2)], "isolation barrier", keepout=True)
    for i, xc in enumerate(CH_CENTERS):
        hw = CH_PITCH / 2 - 0.9
        pts = [(xc - hw, b2), (xc + hw, b2), (xc + hw, H - 0.5), (xc - hw, H - 0.5)]
        zone(f"/CH{i + 1}/ISOGND", "B.Cu", pts, f"ISOGND port {i + 1}")
        if i < 3:
            gx = xc + CH_PITCH / 2
            zone(None, "", [(gx - 0.9, b1), (gx + 0.9, b1), (gx + 0.9, H - 0.5), (gx - 0.9, H - 0.5)], f"gap port {i + 1}/{i + 2}", keepout=True)
    for hx, hy, hr in hole_keepouts():
        pts = [(hx + hr * math.cos(2 * math.pi * k / 24), hy + hr * math.sin(2 * math.pi * k / 24)) for k in range(24)]
        zone(None, "", pts, "metal spacer", keepout=True)
    o.append(")\n")
    with open(path, "w") as fh:
        fh.write("\n".join(o))


BAT_SLOT = (WS_X0 + 5.5, 32.0, WS_X0 + 10.0, 42.0)   # fallback: battery pigtail from below to the module's MX1.25 header


# ---------------------------------------------------------------------------- project / rules
def write_project(path: str, root_uid: str, sheet_uids: List[str]):
    pro = {
        "board": {
            "3dviewports": [],
            "design_settings": {
                "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2, "silk_line_width": 0.12, "silk_text_size_h": 1.0,
                             "silk_text_size_v": 1.0, "silk_text_thickness": 0.15},
                "rules": {"min_clearance": 0.2, "min_connection": 0.0, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.25,
                          "min_hole_to_hole": 0.25, "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1, "min_resolved_spokes": 1,
                          "min_silk_clearance": 0.0, "min_text_height": 0.5, "min_text_thickness": 0.08, "min_through_hole_diameter": 0.3,
                          "min_track_width": 0.2, "min_via_annular_width": 0.15, "min_via_diameter": 0.6, "solder_mask_to_copper_clearance": 0.0,
                          "use_height_for_length_calcs": True},
                "track_widths": [0.0, 0.25, 0.4, 0.8],
                "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.7, "drill": 0.3}],
                "zones_allow_external_fillets": False,
                "rule_severities": {"lib_footprint_issues": "ignore", "lib_footprint_mismatch": "ignore", "silk_overlap": "warning",
                                    "silk_over_copper": "warning", "silk_edge_clearance": "warning", "text_height": "warning",
                                    "starved_thermal": "warning", "footprint_type_mismatch": "ignore"},
            },
            "layer_presets": [], "viewports": [],
        },
        "boards": [], "cvpcb": {"equivalence_files": []},
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": f"{PROJECT}.kicad_pro", "version": 1},
        "net_settings": {
            "classes": [
                {"bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Default", "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
                 "track_width": 0.25, "via_diameter": 0.7, "via_drill": 0.3, "wire_width": 6},
                {"bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": "Power", "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
                 "track_width": 0.6, "via_diameter": 0.8, "via_drill": 0.4, "wire_width": 6},
            ] + [
                {"bus_width": 12, "clearance": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0,
                 "microvia_diameter": 0.3, "microvia_drill": 0.1, "name": f"ISO{n}", "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
                 "track_width": 0.3, "via_diameter": 0.7, "via_drill": 0.3, "wire_width": 6} for n in range(1, 5)
            ],
            "meta": {"version": 3},
            "net_colors": None, "netclass_assignments": None,
            "netclass_patterns": [{"netclass": "Power", "pattern": "+5V"}, {"netclass": "Power", "pattern": "VBAT"}, {"netclass": "Power", "pattern": "GND"},
                                  {"netclass": "Power", "pattern": "ISO_IN*"}, {"netclass": "Power", "pattern": "SW"}]
                                 + [{"netclass": f"ISO{n}", "pattern": f"/CH{n}/{ln}"} for n in range(1, 5) for ln in sorted(CH_ISO_LOCALS)],
        },
        "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "", "plot": "", "pos_files": "", "specctra_dsn": "", "step": "", "svg": "", "vrml": ""},
                   "page_layout_descr_file": ""},
        "schematic": {"drawing": {"label_size_ratio": 0.375, "pin_symbol_size": 25.0, "text_offset_ratio": 0.15},
                      "legacy_lib_dir": "", "legacy_lib_list": []},
        "sheets": [[root_uid, ""]] + [[u, f"CH{i + 1}"] for i, u in enumerate(sheet_uids)],
        "text_variables": {},
    }
    with open(path, "w") as fh:
        json.dump(pro, fh, indent=2)
    dru = ["(version 1)", "# Galvanic isolation: isolated islands keep >= 3 mm to the ESP side and >= 1.5 mm to each other."]
    iso = " || ".join(f"B.NetClass == 'ISO{m}'" for m in range(1, 5))
    for n in range(1, 5):
        dru.append(f'(rule "iso{n}_vs_esp"\n  (condition "A.NetClass == \'ISO{n}\' && B.NetName != \'\' && !({iso})")\n  (constraint clearance (min 3.0mm)))')
        for m in range(1, 5):
            if m != n:
                dru.append(f'(rule "iso{n}_vs_iso{m}"\n  (condition "A.NetClass == \'ISO{n}\' && B.NetClass == \'ISO{m}\'")\n  (constraint clearance (min 1.5mm)))')
    dru.append('(rule "edge"\n  (constraint edge_clearance (min 0.4mm)))')
    with open(path.replace(".kicad_pro", ".kicad_dru"), "w") as fh:
        fh.write("\n".join(dru) + "\n")


# ---------------------------------------------------------------------------- build
def build():
    root_uid = U("root-sheet")
    sheet_uids = [U(f"sheet-ch{n}") for n in range(1, 5)]
    root_path = f"/{root_uid}"
    tmpl = channel_parts()

    def inst_ref(ref: str, n: int) -> str:
        # U01 -> U101, PS01 -> PS1, C01 -> C101, J01 -> J1 ... keep it readable
        import re
        m = re.match(r"([A-Z]+)(\d+)", ref)
        pre, num = m.group(1), int(m.group(2))
        if pre in ("PS", "J"):
            return f"{pre}{n}" if pre == "PS" else f"{pre}{10 + n}"
        return f"{pre}{n}{num:02d}"

    refmaps = [{p.ref: inst_ref(p.ref, n) for p in tmpl} for n in range(1, 5)]
    chan_paths = [(f"{root_path}/{sheet_uids[n - 1]}", refmaps[n - 1]) for n in range(1, 5)]

    # ---- channel sheet (shared file, 4 instances)
    ch = Sheet("channel", U("channel-file"), tmpl, CH_LOCAL_NETS, CH_PORTS, PROJECT, chan_paths,
               "Isolated RS232 port (one of four)")
    ch.text("ESP side (3V3 / GND)", 20.32, 25.4)
    ch.text("---- isolation barrier ---- optocouplers + DC-DC ----", 60.96, 25.4)
    ch.text("isolated side (ISO5V / ISOGND), RJ45 Cisco pinout: 3 TX out, 6 RX in, 4+5 GND", 96.52, 30.48, 1.5)
    ch.text("JP01/JP02 bridged 1-2 = Cisco console pinout (rollover cable to a switch). Bridge 2-3 on both to swap pins 3 and 6.", 20.32, 125.0, 1.3)
    ch.text("LED off while the line idles (UART mark = LED off) -> no quiescent LED current.", 20.32, 115.0, 1.3)
    for p in tmpl:
        ch.place(p)
    for n, net in enumerate(["ISO5V", "ISOGND"]):
        ch.label(net, 215.9, 40.64 + 2.54 * n, 0, "local")
    ch.write(os.path.join(OUT, "channel.kicad_sch"), "2")

    # ---- root sheet
    rparts = root_parts()
    root = Sheet("root", root_uid, rparts, set(), {}, PROJECT, [(root_path, {})], "RS232 isolation carrier - Waveshare ESP32-S3-Touch-LCD-2")
    root.text("Waveshare ESP32-S3-Touch-LCD-2 on two 1x14 sockets", 20.32, 25.4)
    root.text("Port power: VBAT -> MT3608 boost 5.1 V -> one high-side switch per port -> B0505S (channel sheet). EN high = port on.", 63.5, 20.32, 1.4)
    root.text("EN1..EN4 = GPIO16/17/18/10 (10k pull-down via JP11..JP14; bridge 2-3 for always-on without firmware support)", 63.5, 104.14, 1.3)
    root.text("Spare GPIO 2/4/6 + I2C (GPIO47/48) on J3 for future add-ons (buzzer, status LEDs, expander)", 20.32, 152.4, 1.3)
    for p in rparts:
        root.place(p)
    # power flags
    yflag = 190.5
    for i, net in enumerate(["GND", "+3V3", "VBAT", "+5V"]):
        x = 120.65 + 20.32 * i
        root.power_symbol(net, x, yflag)
        root.power_symbol(net, x + 5.08, yflag, flag=True)
        root.wire(x, yflag, x + 5.08, yflag)
    root.text("power flags: GND / 3V3 / VBAT come from the Waveshare module, +5V from the boost converter", 110.49, 198.12, 1.3)
    # channel sheets
    for n in range(1, 5):
        root.sheet_symbol(f"CH{n}", "channel.kicad_sch", sheet_uids[n - 1], 215.9, 30.48 + 25.4 * (n - 1), 30.48, 12.7,
                          {"ESP_TX": ("input", "L", f"ESP_TX{n}"), "ISO_IN": ("input", "L", f"ISO_IN{n}"), "ESP_RX": ("output", "R", f"ESP_RX{n}")},
                          str(n + 1))
    root.write(os.path.join(OUT, f"{PROJECT}.kicad_sch"), "1")

    # ---- PCB
    placed = []
    nets = set()
    for p in rparts:
        pin_nets = {k: v for k, v in p.pins.items()}
        nets.update(v for v in pin_nets.values() if v != "NC")
        placed.append((p, pin_nets, f"{root_path}/{U('root:' + p.ref)}"))
    for n in range(1, 5):
        xc = CH_CENTERS[n - 1]
        for tp in tmpl:
            full = {}
            for k, v in tp.pins.items():
                if v == "NC":
                    full[k] = "NC"
                elif v in CH_PORTS:
                    full[k] = {"ESP_TX": f"ESP_TX{n}", "ESP_RX": f"ESP_RX{n}", "ISO_IN": f"ISO_IN{n}"}[v]
                elif v in CH_LOCAL_NETS:
                    full[k] = f"/CH{n}/{v}"
                else:
                    full[k] = v
            nets.update(v for v in full.values() if v != "NC")
            ip = Part(refmaps[n - 1][tp.ref], tp.sym, tp.fp, tp.value, tp.pins, xc + tp.x, tp.y, tp.rot, dnp=tp.dnp, props=tp.props)
            if tp.ref == "J01":
                ip.y = BOARD_H + V["RJ45_PROTRUDE"] + V["RJ45_FRONT"]   # rot 180: front face at y + (-front)
            placed.append((ip, full, f"{root_path}/{sheet_uids[n - 1]}/{U('channel:' + tp.ref)}"))
    net_list = sorted(nets, key=lambda s: (s.startswith("/"), s))
    write_pcb(os.path.join(OUT, f"{PROJECT}.kicad_pcb"), placed, net_list)
    write_project(os.path.join(OUT, f"{PROJECT}.kicad_pro"), root_uid, sheet_uids)
    with open(os.path.join(OUT, "fp-lib-table"), "w") as fh:
        fh.write("(fp_lib_table\n  (version 7)\n)\n")
    with open(os.path.join(OUT, "sym-lib-table"), "w") as fh:
        fh.write("(sym_lib_table\n  (version 7)\n)\n")
    return placed, net_list


# ---------------------------------------------------------------------------- checks + preview
def check(placed) -> int:
    boxes = []
    errs = 0
    for p, _, _ in placed:
        fp = FPS[p.fp]
        b = fp.courtyard_world(p.x, p.y, p.rot)
        boxes.append((p, b, fp))
        if b[0] < 0.3 or b[1] < 0.3 or b[2] > BOARD_W - 0.3 or b[3] > BOARD_H - 0.3:
            if not (p.fp == "RJ45_Generic" and b[3] > BOARD_H):   # the jack front may stand out
                print(f"EDGE   {p.ref:6s} {p.fp:22s} courtyard {tuple(round(v, 2) for v in b)}")
                errs += 1
        # tall parts under the Waveshare module
        wx1, wy1, wx2, wy2 = WS_X0, WS_Y0, WS_X0 + V["WS_W"], WS_Y0 + V["WS_L"]
        if fp.height > V["WS_STACK"] - 1.0 and not (b[2] < wx1 or b[0] > wx2 or b[3] < wy1 or b[1] > wy2) and fp.attr != "through_hole":
            print(f"HEIGHT {p.ref} {fp.height} mm under the Waveshare module")
            errs += 1
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i][1], boxes[j][1]
            if a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]:
                ox = min(a[2], b[2]) - max(a[0], b[0])
                oy = min(a[3], b[3]) - max(a[1], b[1])
                print(f"OVERLAP {boxes[i][0].ref:6s} x {boxes[j][0].ref:6s}  {ox:.2f} x {oy:.2f} mm")
                errs += 1
    # slot vs parts
    for p, b, fp in boxes:
        if b[0] < BAT_SLOT[2] + 0.5 and BAT_SLOT[0] - 0.5 < b[2] and b[1] < BAT_SLOT[3] + 0.5 and BAT_SLOT[1] - 0.5 < b[3]:
            print(f"SLOT    {p.ref} overlaps the battery cable slot")
            errs += 1
    print(f"placement check: {len(placed)} footprints, {errs} problems")
    return errs


def preview(placed, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle, Circle, Polygon
    fig, ax = plt.subplots(figsize=(8.5, 10.5))
    ax.add_patch(Rectangle((0, 0), BOARD_W, BOARD_H, fill=False, lw=1.5, color="k"))
    ax.add_patch(Rectangle((BAT_SLOT[0], BAT_SLOT[1]), BAT_SLOT[2] - BAT_SLOT[0], BAT_SLOT[3] - BAT_SLOT[1], fill=False, lw=1, color="k"))
    ax.add_patch(Rectangle((WS_X0, WS_Y0), V["WS_W"], V["WS_L"], fill=True, alpha=0.08, color="tab:blue", lw=1, ls="--"))
    ax.text(WS_X0 + 2, WS_Y0 + 20, "Waveshare\nESP32-S3-Touch-LCD-2\n(11 mm above, USB-C up)", fontsize=7, color="tab:blue")
    ax.add_patch(Rectangle((0.5, BARRIER[0]), BOARD_W - 1, BARRIER[1] - BARRIER[0], fill=True, alpha=0.15, color="red", lw=0))
    ax.text(BOARD_W / 2, (BARRIER[0] + BARRIER[1]) / 2, "isolation barrier (no copper)", ha="center", va="center", fontsize=7, color="red")
    for i, xc in enumerate(CH_CENTERS):
        ax.add_patch(Rectangle((xc - CH_PITCH / 2 + 0.5, BARRIER[1]), CH_PITCH - 1, BOARD_H - BARRIER[1] - 0.5, fill=True, alpha=0.06, color="tab:green", lw=0))
        ax.text(xc, BOARD_H - 21, f"PORT {i + 1}", ha="center", fontsize=8, color="tab:green")
    for p, _, _ in placed:
        fp = FPS[p.fp]
        b = fp.courtyard_world(p.x, p.y, p.rot)
        col = "tab:orange" if fp.attr == "through_hole" else "tab:gray"
        ax.add_patch(Rectangle((b[0], b[1]), b[2] - b[0], b[3] - b[1], fill=True, alpha=0.35, color=col, lw=0.5, ec="k"))
        r = math.radians(p.rot)
        c, s = math.cos(r), math.sin(r)
        for pad in fp.pads:
            px, py = p.x + pad.x * c + pad.y * s, p.y - pad.x * s + pad.y * c
            if pad.kind == "npth":
                ax.add_patch(Circle((px, py), pad.drill / 2, fill=False, lw=0.6, color="k"))
            else:
                ax.add_patch(Circle((px, py), min(pad.w, pad.h) / 2.2, fill=True, color="goldenrod" if pad.kind == "tht" else "silver", lw=0))
        ax.text((b[0] + b[2]) / 2, (b[1] + b[3]) / 2, p.ref, ha="center", va="center", fontsize=4.5)
    ax.set_xlim(-3, BOARD_W + 3)
    ax.set_ylim(BOARD_H + 4, -3)
    ax.set_aspect("equal")
    ax.set_xlabel("mm")
    ax.set_title(f"rs232_carrier v{REV} - placement (unrouted)")
    fig.tight_layout()
    fig.savefig(path, dpi=160)


MPN = {
    ("TLP2361", "SO6_Opto"): ("Toshiba TLP2361(E,T)", "alt. Broadcom ACPL-M61L-000E (same pinout); 10 Mbd, 2.7-5.5 V"),
    ("MAX3232E", "SOIC-16"): ("TI MAX3232ECDR", "alt. Maxim MAX3232ECSE+, Exar SP3232EEN-L; ESD-protected version"),
    ("74LVC1G17", "SOT-23-5"): ("TI SN74LVC1G17DBVR", "alt. Nexperia 74LVC1G17GV; Schmitt buffer drives the RX LED"),
    ("B0505S-1WR3", "SIP-4_DCDC"): ("Mornsun B0505S-1WR3", "alt. Hi-Link B0505S-1WR3, Mean Well SPU01L-05; verify pin spacing 2.54/7.62"),
    ("RJ45", "RJ45_Generic"): ("Amphenol 54602-908LF", "8P8C tab-down, no magnetics, no LEDs; footprint must match the chosen part"),
    ("MT3608", "SOT-23-6"): ("Aerosemi MT3608", "boost VBAT -> 5.1 V; alt. TPS61023 (different footprint)"),
    ("SS14", "D_SMA"): ("Vishay SS14", "40 V Schottky, SMA"),
    ("1N4148W", "D_SOD-123"): ("Diodes Inc 1N4148W-7-F", ""),
    ("2N7002", "SOT-23"): ("Nexperia 2N7002,215", ""),
    ("AO3401A", "SOT-23"): ("Alpha&Omega AO3401A", "P-channel high-side switch"),
    ("22u", "L_5x5"): ("Bourns SRN5040-220M", "22 uH, >=1 A sat; any 4x4..5x5 mm shielded inductor with 2 pads"),
    ("LED green", "LED_0603"): ("Everlight 19-217/GHC-YR1S2/3T", "any 0603 green LED, cathode mark"),
    ("WS header 1-14", "PinSocket_1x14"): ("1x14 socket 2.54 mm, 8.5 mm", "female header, the Waveshare module plugs in"),
    ("WS header 15-28", "PinSocket_1x14_B"): ("1x14 socket 2.54 mm, 8.5 mm", "female header"),
    ("EXT", "PinHeader_1x7"): ("1x7 pin header 2.54 mm", "optional"),
}


def write_bom(placed, path):
    import csv
    groups = {}
    for p, _, _ in placed:
        if p.fp.startswith("MountingHole") or p.fp.startswith("SolderJumper"):
            continue
        key = (p.value, p.fp)
        groups.setdefault(key, []).append(p.ref)
    rows = []
    for (value, fp), refs in sorted(groups.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        mpn, note = MPN.get((value, fp), ("", ""))
        if not mpn:
            if fp in ("R_0603",):
                mpn, note = f"{value} 0603 1%", ""
            elif fp in ("C_0603", "C_0805"):
                size = fp[2:6]
                mpn, note = f"{value} {size} X7R/X5R >=10 V", "ceramic"
        rows.append({"refs": " ".join(sorted(refs, key=lambda r: (r.rstrip('0123456789'), int(''.join(c for c in r if c.isdigit()) or 0)))),
                     "qty": len(refs), "value": value, "footprint": fp, "suggested part": mpn, "note": note})
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["refs", "qty", "value", "footprint", "suggested part", "note"])
        w.writeheader()
        w.writerows(rows)
    return len(rows)


def firmware_pins():
    """Print the config.h fragment for the firmware."""
    rx = sorted({v[0] for v in PORT_PINS.values()} | {v[1] for v in PORT_PINS.values()})
    defaults = ", ".join(f"{{{v[0]}, {v[1]}}}" for _, v in sorted(PORT_PINS.items()))
    en = ", ".join(str(v[2]) for _, v in sorted(PORT_PINS.items()))
    return (f"#define PORT_DEFAULT_PINS {{{defaults}}}\n#define PINS_RX  {{{', '.join(map(str, rx))}}}\n#define PINS_TX  {{{', '.join(map(str, rx))}}}\n"
            f"#define PINS_PORT_EN {{{en}}}\n")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    placed, nets = build()
    n = check(placed)
    if "--check" not in sys.argv:
        preview(placed, os.path.join(HERE, "preview.png"))
        print("bom lines:", write_bom(placed, os.path.join(HERE, "bom.csv")))
        print(f"wrote {OUT} ({len(nets)} nets)")
        print(firmware_pins())
    sys.exit(1 if n else 0)
