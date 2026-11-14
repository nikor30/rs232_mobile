#!/usr/bin/env python3
"""Generate the KiCad 7 project of the RS232 isolation carrier board.

    python3 generate.py            # writes kicad/rs232_carrier.* and preview.png
    python3 generate.py --check    # only run the placement checks

The carrier takes the Waveshare ESP32-S3-Touch-LCD-2 on two 1x14 sockets and
adds four galvanically isolated RS232 ports (TLP2361 optocouplers, MAX3232E,
one B0503S isolated DC-DC per port), a VBAT->5V boost converter and a
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
REV = "0.4"
DATE = "2026-11-14"

# ============================================================================
# Waveshare ESP32-S3-Touch-LCD-2: geometry taken from the vendor's own 3D model
# (esp32-s3-touch-lcd-2_20241108.stp from the Waveshare wiki, "2D/3D file") and
# cross-checked with the dimension drawing.  Vendor coordinate system:
#   x across the width, y along the length (USB-C at -y, antenna at +y),
#   z up towards the display front; glass front z = +3.6, PCB z = -3.6 .. -2.0.
# Seen from the DISPLAY side with the antenna end up, +x is to the right; the
# header with pins 1-14 (3V3 .. VBAT) is at x = -15.24, pins 15-28 at +15.24.
# ============================================================================
WS = {
    "pcb": (-17.509, -25.809, 17.509, 22.409), "pcb_z": (-3.6, -2.0),          # 35.02 x 48.22 x 1.6
    "pmma": (-17.5, -25.6, 17.5, 22.4), "pmma_z": (-2.0, 0.0),                  # acrylic spacer under the LCD
    "lcm": (-17.3, -25.8, 17.3, 22.2), "lcm_z": (0.0, 2.1),                     # LCD module
    "tp_sensor": (-17.09, -25.6, 17.09, 21.95), "tp_sensor_z": (2.1, 2.5),      # touch sensor film under the glass
    "tp_fpc_y": -26.0,                                                          # touch flex tail, 0.19 mm beyond the PCB edge
    "glass": (-18.55, -30.89, 18.55, 27.91), "glass_z": (2.5, 3.6), "glass_r": 2.6,   # 37.10 x 58.80 x 1.10
    "hdr_x": 15.24,                 # pin rows at x = -15.24 (pins 1-14) and x = +15.24 (pins 15-28)
    "hdr_y1": 14.81,                # pin 1 and pin 28 (antenna end); 13 more pins at -2.54 down to y = -18.21
    "hdr_plastic_z": (-6.1, -3.6), "hdr_pin_tip_z": -11.9,
    "standoffs": [(-14.5, -22.8), (14.5, -22.8), (-14.5, 19.4), (14.5, 19.4)],  # SMTSO-M2-2.5ET, 29.0 x 42.2
    "standoff_d": 3.5, "standoff_z": (-6.1, -2.1),
    # USB-C receptacle (16 pin, sunk into the board edge).  Standard interface: shell 8.94 x 3.16 outside,
    # opening 8.34 x 2.56, the insulator face a plug's shell stops at lies 4.78 behind the rim.
    "usbc": (-4.79, -26.993, 4.79, -19.462), "usbc_z": (-6.85, -2.69),          # with legs; rim 1.18 beyond the PCB edge
    "usbc_axis_z": -5.27, "usbc_shell": (8.94, 3.16), "usbc_open": (8.34, 2.56),
    "usbc_front_y": -26.993, "usbc_back_y": -22.213,
    "usbc_tongue": (6.69, 0.62, -26.663),                                       # width, thickness, y of the tip
    # RST / BOOT: side-push tact switches TS24CA.  Body 4.55 x 2.30 x 1.80, plunger 2.00 x 1.00,
    # 1.20 proud of the body, pushed towards +y.  (x centre, y of the plunger tip)
    "buttons": {"RST": (9.474, -25.527), "BOOT": (-9.449, -25.502)},
    "button_w": 4.55, "button_d": 2.30, "button_z": (-5.4, -3.6),
    "button_act": (2.0, 1.0, 1.2), "button_act_z": -4.5,
    "bat": (-12.92, -7.90, -5.27, -2.70), "bat_z": (-7.0, -3.6),                # MX1.25 2P battery header (pin 1 = VBAT)
    "tf": (-6.05, 1.5, 5.35, 7.0), "tf_z": (-6.3, -3.65),                       # micro-SD socket, card enters from -y
    "cam": (-8.6, -15.32, 8.6, -9.63), "cam_z": (-5.6, -3.6),                   # 24-pin camera FPC connector
}
# USB Type-C plug as the USB-IF compliance document dimensions it: metal shell 8.25 x 2.40, 6.65 long
# in front of the overmould; the overmould may be up to 12.35 wide and 6.5 high.
USBC_PLUG = {"shell_w": 8.25, "shell_h": 2.40, "shell_l": 6.65, "over_w": 12.35, "over_h": 6.5}
SOCKET_H = 8.5                       # 2.54 mm socket strip on the carrier
WS_Z0 = SOCKET_H - WS["hdr_plastic_z"][0]   # vendor z -> height above the carrier top when fully plugged (14.6)

# ============================================================================
# Numbers to verify before ordering (from datasheets / from memory).
# ============================================================================
VERIFY = {
    "WS_W": WS["pcb"][2] - WS["pcb"][0], "WS_L": WS["pcb"][3] - WS["pcb"][1],
    "WS_STACK": WS_Z0 + WS["pcb_z"][0],          # carrier top -> module PCB underside: 11.0
    # RJ45 jack (generic 8P8C, side entry, pegs).  Replace with the KiCad library
    # footprint of the exact part before ordering.
    "RJ45_W": 15.9, "RJ45_D": 21.3, "RJ45_H": 13.5,
    "RJ45_ROW2_DY": -2.54,    # even pins row relative to the odd pins row
    "RJ45_PEG": (5.715, -6.35, 3.25),  # +-x, y of the two pegs, diameter
    "RJ45_FRONT": -8.9,       # front face relative to the odd pins row
    "RJ45_PROTRUDE": 0.0,     # front face flush with the PCB edge; the case wall is thin there
    # B0503S-1WR3 SIP-4: pins 1,2 (in) and 3,4 (out), 2.54 pitch, 7.62 gap between 2 and 3
    "SIP4_PINS": (0.0, 2.54, 10.16, 12.7), "SIP4_BODY": (19.6, 6.0, 10.2),
    # TLP2361 SO6: rows 5.9 mm apart (pads centred +-2.95), pitch 1.27
    "SO6_ROW": 2.95,
    # CPC2330N SOIC-8: LED pins 1-4 on one side, relay pins 5-8 on the other
    "SO8_ROW": 2.75,
}
V = VERIFY

# ---------------------------------------------------------------------------- board geometry
BOARD_W, BOARD_H = 78.0, 110.0
BOARD_R = 3.0                           # corner radius of the board outline (the case has rounded inner corners)
CH_PITCH = 19.0
CH_X0 = 0.9 + CH_PITCH / 2              # centre of channel 1
CH_CENTERS = [CH_X0 + i * CH_PITCH for i in range(4)]
WS_XC = BOARD_W / 2                     # vendor x = 0 sits on the carrier centre line
WS_Y0 = 1.0                             # USB-C edge of the module PCB on the carrier
WS_YC = WS_Y0 - WS["pcb"][1]            # carrier y of vendor y = 0
WS_X0 = WS_XC + WS["pcb"][0]            # module PCB outline on the carrier
BARRIER = (61.0, 66.0)                  # y band without copper between ESP side and isolated side
B0 = BARRIER[0]


def ws_xy(xv: float, yv: float) -> Tuple[float, float]:
    """Vendor coordinates -> carrier (KiCad) coordinates.  The module sits display up with the
    USB-C end at the top edge of the carrier (KiCad -y): a 180 degree turn seen from above, so
    carrier x = centre - vendor x and carrier y (down) = centre + vendor y.  No mirror."""
    return WS_XC - xv, WS_YC + yv


# GPIO assignment on the Waveshare headers (header pin -> GPIO), schematic P2 / P1
HDR_A = {1: "3V3", 2: "GND", 3: "IO43", 4: "IO44", 5: "IO47", 6: "IO48", 7: "IO15", 8: "IO13",
         9: "IO11", 10: "IO12", 11: "IO14", 12: "IO9", 13: "GND", 14: "VBAT"}
HDR_B = {28: "IO2", 27: "IO4", 26: "IO6", 25: "IO16", 24: "IO17", 23: "IO18", 22: "IO21", 21: "IO8",
         20: "IO7", 19: "IO10", 18: "IO20", 17: "IO19", 16: "GND", 15: "5V"}
PORT_PINS = {   # port -> (rx gpio, tx gpio, enable gpio, cross-over gpio)
    1: (44, 21, 16, 2),
    2: (15, 13, 17, 4),
    3: (11, 12, 18, 6),
    4: (14, 9, 10, 7),
}
GPIO_NET = {}   # gpio -> net name on the carrier
for p, (rx, tx, en, sw) in PORT_PINS.items():
    GPIO_NET[rx] = f"ESP_RX{p}"
    GPIO_NET[tx] = f"ESP_TX{p}"
    GPIO_NET[en] = f"EN{p}"
    GPIO_NET[sw] = f"SWAP{p}"
GPIO_NET.update({47: "SCL", 48: "SDA", 8: "IO8", 43: "TXD0"})


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
    box("DCDC_SIP4", [("2", "+Vin", "L", "power_in", 0), ("1", "-Vin", "L", "power_in", 1),
                      ("4", "+Vout", "R", "power_out", 0), ("3", "-Vout", "R", "power_out", 1)], prefix="PS",
        extra_props={"Note": "1 W isolated DC-DC 5 V -> 3.3 V, SIP-4, 1.5 kV (B0503S-1WR3)"}),
    box("PhotoMOS_1A1B", [("1", "A1", "L", "input", 0), ("2", "K1", "L", "input", 1), ("3", "A2", "L", "input", 2), ("4", "K2", "L", "input", 3),
                          ("8", "NO_a", "R", "passive", 0), ("7", "NO_b", "R", "passive", 1), ("6", "NC_a", "R", "passive", 2), ("5", "NC_b", "R", "passive", 3)],
        width=12.7, extra_props={"Note": "CPC2330N: LED 1-2 drives the normally open pole 7-8, LED 3-4 the normally closed pole 5-6. VERIFY the pinout"}),
    box("TVS_2ch", [("1", "IO1", "L", "passive", 0), ("2", "IO2", "L", "passive", 1), ("3", "COM", "R", "passive", 0)], prefix="D",
        extra_props={"Note": "PESD15VL2BT: two bidirectional 15 V lines to common"}),
    box("RJ45", [(str(i), f"P{i}", "L", "passive", i - 1) for i in range(1, 9)], prefix="J"),
    box("MT3608", [("5", "IN", "L", "power_in", 0), ("4", "EN", "L", "input", 1), ("3", "FB", "L", "input", 2),
                   ("1", "SW", "R", "passive", 0), ("6", "NC", "R", "no_connect", 1), ("2", "GND", "R", "power_in", 2)]),
    box("Q_NMOS", [("1", "G", "L", "input", 0), ("3", "D", "R", "passive", 0), ("2", "S", "R", "passive", 1)], prefix="Q"),
    box("Q_PMOS", [("1", "G", "L", "input", 0), ("2", "S", "R", "passive", 0), ("3", "D", "R", "passive", 1)], prefix="Q"),
    box("PinSocket_1x14", [(str(i), f"{i}", "L", "passive", i - 1) for i in range(1, 15)], prefix="J"),
    box("PinSocket_1x14_B", [(str(28 - i), f"{28 - i}", "L", "passive", i) for i in range(14)], prefix="J"),
    box("Conn_1x7", [(str(i), f"{i}", "L", "passive", i - 1) for i in range(1, 8)], prefix="J"),
    box("Conn_1x2", [("1", "BAT+", "L", "passive", 0), ("2", "BAT-", "L", "passive", 1)], prefix="J"),
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
two_pad("R_0805", 0.9125, 1.025, 1.45, (1.68, 0.98), (2.0, 1.25))
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
                                 note="Mornsun B0503S-1WR3 (5 V in, 3.3 V out) or pin compatible")

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

# SOIC-8 narrow (CPC2330N): pins 1-4 at -x (top->bottom), 5-8 at +x (bottom->top)
r8 = V["SO8_ROW"]
FPS["SOIC-8"] = FootprintDef("SOIC-8", [Pad(str(i + 1), -r8, -1.905 + 1.27 * i, 1.5, 0.6) for i in range(4)]
                             + [Pad(str(8 - i), r8, -1.905 + 1.27 * i, 1.5, 0.6) for i in range(4)],
                             (-3.75, -2.75, 3.75, 2.75), fab=(-1.95, -2.45, 1.95, 2.45),
                             silk_lines=[(-1.95, -2.6, 1.95, -2.6), (-1.95, 2.6, 1.95, 2.6), (-3.6, -2.6, -3.6, -2.0)], height=1.75,
                             note="CPC2330N / LBA110S-compatible SOIC-8, 1.27 mm pitch")
# Molex PicoBlade-compatible 1.25 mm header, 2 pins, vertical through-hole (the battery plug of the Waveshare kits)
FPS["MX125_2_Vertical"] = FootprintDef("MX125_2_Vertical", [Pad("1", 0, 0, 0.9, 1.3, "tht", "rect", 0.5), Pad("2", 1.25, 0, 0.9, 1.3, "tht", "oval", 0.5)],
                                       (-1.9, -2.0, 3.15, 2.0), fab=(-1.5, -1.6, 2.75, 1.6), silk_lines=rect_lines(-1.6, -1.7, 2.85, 1.7),
                                       attr="through_hole", height=4.2, note="Molex 53047-0210 or compatible, mounted on the bottom side")

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
        P("R01", "R", "R_0603", "330", {"1": "+3V3", "2": "TXA"}, -6.0, B0 - 5.7, 90, 25.4, 38.1, props={"Note": "TX LED, ~5 mA at 3.3 V"}),
        P("U01", "TLP2361", "SO6_Opto", "TLP2361", {"1": "TXA", "3": "ESP_TX", "6": "VISO", "5": "T1IN", "4": "ISOGND"}, -5.0, B0 + 2.5, 0, 50.8, 38.1),
        P("U02", "TLP2361", "SO6_Opto", "TLP2361", {"1": "RXA", "3": "BUF_OUT", "6": "+3V3", "5": "RX_O", "4": "GND"}, 0.0, B0 + 2.5, 180, 50.8, 63.5),
        P("R03", "R", "R_0603", "100", {"1": "RX_O", "2": "ESP_RX"}, 1.5, B0 - 5.7, 90, 25.4, 63.5),
        P("C07", "C", "C_0603", "100n", {"1": "+3V3", "2": "GND"}, -2.3, B0 - 5.7, 90, 25.4, 76.2),
        # isolated supply: 5 V in, 3.3 V out.  Unregulated, so it rises at light load - 3.3 V nominal keeps
        # every part on the island inside its supply range (a 5 V type would exceed 5.5 V unloaded).
        P("PS01", "DCDC_SIP4", "SIP-4_DCDC", "B0503S-1WR3", {"2": "ISO_IN", "1": "GND", "4": "VISO", "3": "ISOGND"}, 7.2, B0 - 3.0, 270, 50.8, 96.52),
        P("C10", "C", "C_0805", "2.2u", {"1": "ISO_IN", "2": "GND"}, 7.2, B0 - 8.4, 180, 25.4, 96.52),
        P("C09", "C", "C_0603", "4.7u", {"1": "VISO", "2": "ISOGND"}, 6.2, B0 + 15.2, 0, 78.74, 104.14),
        P("C05", "C", "C_0603", "100n", {"1": "VISO", "2": "ISOGND"}, 3.3, B0 + 8.6, 90, 96.52, 91.44),
        # isolated side
        P("U03", "MAX3232E", "SOIC-16", "MAX3232E", {"1": "C1P", "3": "C1N", "4": "C2P", "5": "C2N", "2": "VP", "6": "VN",
                                                      "11": "T1IN", "10": "VISO", "16": "VISO", "15": "ISOGND", "14": "T1OUT",
                                                      "13": "R1IN", "12": "R1OUT", "7": "NC", "8": "NC", "9": "NC"}, -2.4, B0 + 17.0, 0, 134.62, 45.72),
        P("C01", "C", "C_0603", "100n", {"1": "C1P", "2": "C1N"}, -7.6, B0 + 13.2, 270, 96.52, 40.64),
        P("C02", "C", "C_0603", "470n", {"1": "C2P", "2": "C2N"}, -7.6, B0 + 16.4, 270, 96.52, 53.34, props={"Note": "0.47 uF covers 3.0-5.5 V (datasheet table)"}),
        P("C03", "C", "C_0603", "470n", {"1": "VP", "2": "ISOGND"}, -3.9, B0 + 9.6, 0, 96.52, 66.04),
        P("C04", "C", "C_0603", "470n", {"1": "VN", "2": "ISOGND"}, -4.3, B0 + 23.4, 0, 96.52, 78.74),
        P("U04", "74LVC1G17", "SOT-23-5", "74LVC1G17", {"2": "R1OUT", "1": "NC", "5": "VISO", "4": "BUF_OUT", "3": "ISOGND"}, 5.9, B0 + 19.5, 0, 134.62, 88.9),
        P("C08", "C", "C_0603", "100n", {"1": "VISO", "2": "ISOGND"}, 5.5, B0 + 22.5, 0, 96.52, 104.14),
        P("R04", "R", "R_0603", "1k", {"1": "VISO", "2": "LEDA"}, 4.9, B0 + 24.4, 0, 134.62, 106.68),
        P("D01", "LED", "LED_0603", "LED green", {"2": "LEDA", "1": "ISOGND"}, 5.4, B0 + 26.3, 0, 154.94, 106.68),
        P("R02", "R", "R_0603", "330", {"1": "VISO", "2": "RXA"}, 0.4, B0 + 23.4, 0, 78.74, 71.12, props={"Note": "RX LED, ~5 mA at 3.3 V"}),
        # software cross-over: two 1-Form-A + 1-Form-B PhotoMOS relays on the bottom side, across the barrier.
        # LEDs off (default, also when the port is unpowered): pin 3 = TX out, pin 6 = RX in (Cisco console pinout,
        # rollover cable).  LEDs on: pins 3 and 6 swapped (straight patch cable).  The LEDs take their current from
        # the switched port supply, so a disabled port never draws LED current.
        P("U05", "PhotoMOS_1A1B", "SOIC-8", "CPC2330N", {"1": "SWA1", "2": "SWA2", "3": "SWA2", "4": "SWK",
                                                          "6": "RJ6", "5": "R1IN", "7": "RJ6", "8": "T1OUT"}, -5.3, B0 + 2.5, 90, 167.64, 43.18, side="B",
          props={"Note": "line 6: closed pole 5-6 = RX (default), open pole 7-8 = TX"}),
        P("U06", "PhotoMOS_1A1B", "SOIC-8", "CPC2330N", {"1": "SWB1", "2": "SWB2", "3": "SWB2", "4": "SWK",
                                                          "6": "RJ3", "5": "T1OUT", "7": "RJ3", "8": "R1IN"}, 0.3, B0 + 2.5, 90, 167.64, 68.58, side="B",
          props={"Note": "line 3: closed pole 5-6 = TX (default), open pole 7-8 = RX"}),
        # LED series resistors, bottom side, placed as bridges: R05 is fed from the west, R06 from the DC-DC input pin
        # in the east, and the common cathode line of the second relay passes between the pads of R06 - so the
        # whole LED circuit is planar on the bottom layer and needs a single via (to Q01 on top).
        P("R05", "R", "R_0805", "470", {"1": "ISO_IN", "2": "SWA1"}, -7.2, B0 - 4.0, 180, 25.4, 109.22, props={"Note": "2 LEDs in series, ~5.5 mA"}, side="B"),
        P("R06", "R", "R_0805", "470", {"1": "ISO_IN", "2": "SWB1"}, 2.2, B0 - 4.0, 0, 25.4, 121.92, side="B"),
        P("Q01", "Q_NMOS", "SOT-23", "BSS138", {"1": "SWAP", "3": "SWK", "2": "GND"}, -0.6, 51.0, 270, 50.8, 115.57),
        P("R07", "R", "R_0603", "100k", {"1": "SWAP", "2": "GND"}, 3.2, 50.0, 0, 25.4, 134.62, props={"Note": "default: not crossed"}),
        P("D02", "TVS_2ch", "SOT-23", "PESD15VL2BT", {"1": "RJ3", "2": "RJ6", "3": "ISOGND"}, 0.0, BOARD_H - 12.5, 270, 195.58, 88.9, side="B"),
        P("J01", "RJ45", "RJ45_Generic", "RJ45", {"1": "NC", "2": "NC", "3": "RJ3", "4": "ISOGND", "5": "ISOGND", "6": "RJ6", "7": "NC", "8": "NC"},
          0.0, 0.0, 180, 195.58, 48.26),
    ]


CH_LOCAL_NETS = {"TXA", "RXA", "T1IN", "R1OUT", "R1IN", "T1OUT", "BUF_OUT", "RX_O", "RJ3", "RJ6", "VISO", "ISOGND",
                 "C1P", "C1N", "C2P", "C2N", "VP", "VN", "LEDA", "SWA1", "SWA2", "SWB1", "SWB2", "SWK"}
CH_PORTS = {"ESP_TX": "input", "ESP_RX": "output", "ISO_IN": "input", "SWAP": "input"}
CH_ESP_LOCALS = {"TXA", "RX_O", "SWA1", "SWA2", "SWB1", "SWB2", "SWK"}   # channel-local nets on the ESP side of the barrier
CH_ISO_LOCALS = CH_LOCAL_NETS - CH_ESP_LOCALS
POSTS = [(3.5, 45.0), (74.5, 45.0), (19.9, 85.0), (57.9, 85.0)]   # where the case posts clamp the board (top and bottom)
POST_R = 2.6                                        # component-free radius; plastic posts: copper may run underneath
HOLE_KEEPOUT_R = {"M2": 2.6}                        # no copper under the metal screw heads of the module standoffs
GLOBAL_POWER = {"GND", "+3V3", "VBAT", "+5V"}


def root_parts() -> List[Part]:
    P = Part
    hx = WS["hdr_x"]
    j1 = ws_xy(-hx, WS["hdr_y1"])     # pins 1-14: seen from above with the USB-C end up this row is on the RIGHT
    j2 = ws_xy(+hx, WS["hdr_y1"])     # pins 15-28 on the LEFT
    parts = [
        P("J1", "PinSocket_1x14", "PinSocket_1x14", "WS header 1-14", {str(k): hdr_net(v) for k, v in HDR_A.items()},
          j1[0], j1[1], 90, 30.48, 45.72, props={"Note": "Waveshare P2 (3V3 .. VBAT), pin 1 at the antenna end"}),
        P("J2", "PinSocket_1x14_B", "PinSocket_1x14_B", "WS header 15-28", {str(k): hdr_net(v) for k, v in HDR_B.items()},
          j2[0], j2[1], 90, 30.48, 101.6, props={"Note": "Waveshare P1 (GPIO2 .. 5V), pin 28 at the antenna end"}),
        P("J3", "Conn_1x7", "PinHeader_1x7", "EXT", {"1": "+3V3", "2": "GND", "3": "SDA", "4": "SCL", "5": "IO8", "6": "TXD0", "7": "VBUS"},
          58.76, 2.0, 0, 30.48, 157.48, props={"Note": "I2C, spare GPIO8, boot log TX (GPIO43), USB 5 V"}),
        # boost converter VBAT -> +5V, next to the VBAT pin (top right)
        P("U5", "MT3608", "SOT-23-6", "MT3608", {"5": "VBAT", "4": "PWR_EN", "3": "FB", "1": "SW", "6": "NC", "2": "GND"}, 67.5, 8.6, 180, 96.52, 45.72),
        P("L1", "L", "L_5x5", "22u", {"1": "VBAT", "2": "SW"}, 62.5, 14.5, 0, 96.52, 27.94),
        P("D5", "D", "D_SMA", "SS14", {"2": "SW", "1": "+5V"}, 70.3, 14.5, 180, 121.92, 27.94),
        P("C51", "C", "C_0805", "10u", {"1": "VBAT", "2": "GND"}, 61.0, 8.6, 90, 71.12, 27.94),
        P("C52", "C", "C_0805", "22u", {"1": "+5V", "2": "GND"}, 76.0, 14.5, 90, 147.32, 27.94),
        P("C53", "C", "C_0805", "22u", {"1": "+5V", "2": "GND"}, 71.5, 18.6, 0, 147.32, 38.1),
        P("C54", "C", "C_0603", "100n", {"1": "VBAT", "2": "GND"}, 63.5, 8.6, 90, 71.12, 38.1),
        P("R51", "R", "R_0603", "270k", {"1": "+5V", "2": "FB"}, 75.0, 5.6, 0, 121.92, 60.96, props={"Note": "Vout = 0.6 V * (1 + R51/R52) = 5.1 V; 12 uA from the cell when off"}),
        P("R52", "R", "R_0603", "36k", {"1": "FB", "2": "GND"}, 71.5, 5.6, 0, 121.92, 71.12),
        P("R53", "R", "R_0603", "100k", {"1": "PWR_EN", "2": "GND"}, 65.0, 5.6, 0, 71.12, 60.96),
        # battery: either connector (bottom side), through a P-FET that blocks a reversed plug
        P("J4", "Conn_1x2", "JST_PH_2_Vertical", "BAT JST-PH", {"1": "BATP", "2": "GND"}, 66.0, 30.0, 0, 30.48, 175.26,
          props={"Note": "battery, bottom side. CHECK POLARITY with a meter before the first plug-in"}, side="B"),
        P("J5", "Conn_1x2", "MX125_2_Vertical", "BAT MX1.25", {"1": "BATP", "2": "GND"}, 66.0, 38.0, 0, 55.88, 175.26,
          props={"Note": "same battery net, 1.25 mm plug as on the Waveshare module (its J4: pin 1 = VBAT)"}, side="B"),
        P("Q5", "Q_PMOS", "SOT-23", "AO3401A", {"1": "GND", "3": "BATP", "2": "VBAT"}, 72.5, 25.0, 0, 81.28, 175.26,
          props={"Note": "reverse-polarity protection: conducts both ways when the cell is the right way round"}),
    ]
    rows = [5.0, 14.5, 24.0, 33.5]
    for n in range(1, 5):
        y = rows[n - 1]
        sy = 109.22 + 17.78 * (n - 1)
        parts += [
            P(f"R6{n}", "R", "R_0603", "100k", {"1": f"EN{n}", "2": "GND"}, 9.8, y - 0.6, 0, 96.52, sy, props={"Note": "port off until the firmware enables it"}),
            P(f"Q{n}1", "Q_NMOS", "SOT-23", "BSS138", {"1": f"EN{n}", "3": f"PG{n}", "2": "GND"}, 5.0, y, 0, 121.92, sy),
            P(f"R7{n}", "R", "R_0603", "100k", {"1": "+5V", "2": f"PG{n}"}, 9.8, y + 2.6, 0, 147.32, sy),
            P(f"Q{n}2", "Q_PMOS", "SOT-23", "AO3401A", {"1": f"PG{n}", "2": "+5V", "3": f"ISO_IN{n}"}, 5.0, y + 4.6, 180, 172.72, sy),
            P(f"D5{n}", "D", "D_SOD-123", "1N4148W", {"2": f"EN{n}", "1": "PWR_EN"}, 15.0, y + 1.0, 0, 71.12, 76.2 + 10.16 * (n - 1)),
        ]
    # holes: 4 x M2 under the module's threaded standoffs (optional screws from below)
    for i, (hx_, hy_) in enumerate(ws_holes()):
        parts.append(P(f"H{i + 1}", "MountingHole", "MountingHole_2.2", "M2 Waveshare", {}, hx_, hy_, 0, 30.48 + 12.7 * i, 185.42))
    return parts


def ws_holes():
    return [ws_xy(x, y) for x, y in WS["standoffs"]]


def pad_xy(p: Part, pad) -> Tuple[float, float]:
    """Board position of a pad (bottom-side parts are mirrored)."""
    return FPS[p.fp].local_world(p.x, p.y, p.rot, pad.x, pad.y, p.side)


def part_box(p: Part, which: str = "courtyard") -> Tuple[float, float, float, float]:
    fp = FPS[p.fp]
    return fp.box_world(p.x, p.y, p.rot, getattr(fp, which) or fp.courtyard, p.side)


def hole_keepouts():
    """(x, y, r) of the copper keep-outs under the metal spacers."""
    return [(x, y, HOLE_KEEPOUT_R["M2"]) for x, y in ws_holes()]


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
    R = BOARD_R
    for i, (x1, y1, x2, y2) in enumerate([(R, 0, W - R, 0), (W, R, W, H - R), (W - R, H, R, H), (0, H - R, 0, R)]):
        o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (tstamp {U("edge" + str(i))}))')
    k = R * (1 - math.sqrt(0.5))
    for i, (sx, sy, mx, my, ex, ey) in enumerate([(0, R, k, k, R, 0), (W - R, 0, W - k, k, W, R), (W, H - R, W - k, H - k, W - R, H), (R, H, k, H - k, 0, H - R)]):
        o.append(f'  (gr_arc (start {f(sx)} {f(sy)}) (mid {f(mx)} {f(my)}) (end {f(ex)} {f(ey)}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (tstamp {U("edgearc" + str(i))}))')
    # Waveshare outline, glass and USB-C on User.Comments (from the vendor model)
    def cm_rect(box, tag, style="default", width=0.1):
        (ax, ay), (bx, by) = ws_xy(box[0], box[1]), ws_xy(box[2], box[3])
        for i, (x1, y1, x2, y2) in enumerate(rect_lines(min(ax, bx), min(ay, by), max(ax, bx), max(ay, by))):
            o.append(f'  (gr_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width {width}) (type {style})) (layer "Cmts.User") (tstamp {U(tag + str(i))}))')
    cm_rect(WS["pcb"], "ws", "dash", 0.15)
    cm_rect(WS["glass"], "gl", "dot")
    cm_rect(WS["usbc"], "usb")
    cm_rect(WS["bat"], "wsbat")
    wx1, wy1 = WS_X0, WS_Y0

    def text(s, x, y, layer="F.SilkS", size=1.0, rot=0, just="left"):
        words = [w for w in (("" if just == "center" else just), ("mirror" if layer.startswith("B.") else "")) if w]
        jst = f" (justify {' '.join(words)})" if words else ""
        o.append(f'  (gr_text "{s}" (at {f(x)} {f(y)} {rot}) (layer "{layer}") (tstamp {U("gt" + s + str(x) + str(y))})\n'
                 f"    (effects (font (size {size} {size}) (thickness {f(size * 0.15)})){jst})\n  )")

    text("Waveshare ESP32-S3-Touch-LCD-2, USB-C up", wx1 + 1, wy1 + 20, "Cmts.User", 1.0, 90)
    text(f"RS232 ISO CARRIER v{REV} {DATE} nikor30/rs232_mobile", 39.0, 46.5, "B.SilkS", 1.2, 0, "center")
    for i, xc in enumerate(CH_CENTERS):
        text(f"PORT {i + 1}", xc - 2.8, H - 21.5, "F.SilkS", 1.0)
        text("3=TX 6=RX 4/5=GND", xc - 5.6, H - 22.9, "F.SilkS", 0.6)
    text("BAT +", 68.3, 30.9, "B.SilkS", 0.9)
    text("BAT +", 68.3, 38.9, "B.SilkS", 0.9)
    text("check polarity", 68.3, 34.4, "B.SilkS", 0.7)
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
                     f"    (connect_pads thru_hole_only (clearance 0.2))\n    (min_thickness 0.2) (filled_areas_thickness no)\n"
                     f"    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))\n    (polygon (pts {p}))\n  )")

    b1, b2 = BARRIER
    for layer in ("B.Cu", "F.Cu"):     # pours on both layers, stitched by the ground vias and through-hole pins
        zone("GND", layer, [(0.5, 2.0), (2.0, 0.5), (W - 2.0, 0.5), (W - 0.5, 2.0), (W - 0.5, b1), (0.5, b1)], f"GND plane (ESP side) {layer}")
    zone(None, "", [(0.5, b1), (W - 0.5, b1), (W - 0.5, b2), (0.5, b2)], "isolation barrier", keepout=True)
    for i, xc in enumerate(CH_CENTERS):
        hw = CH_PITCH / 2 - 0.9
        pts = [(xc - hw, b2), (xc + hw, b2), (xc + hw, H - 0.5), (xc - hw, H - 0.5)]
        if i == 0:
            pts = [(xc - hw, b2), (xc + hw, b2), (xc + hw, H - 0.5), (xc - hw + 1.2, H - 0.5), (xc - hw, H - 1.7)]
        if i == 3:
            pts = [(xc - hw, b2), (xc + hw, b2), (xc + hw, H - 1.7), (xc + hw - 1.2, H - 0.5), (xc - hw, H - 0.5)]
        for layer in ("B.Cu", "F.Cu"):
            zone(f"/CH{i + 1}/ISOGND", layer, pts, f"ISOGND port {i + 1} {layer}")
        if i < 3:
            gx = xc + CH_PITCH / 2
            zone(None, "", [(gx - 0.9, b1), (gx + 0.9, b1), (gx + 0.9, H - 0.5), (gx - 0.9, H - 0.5)], f"gap port {i + 1}/{i + 2}", keepout=True)
    for hx, hy, hr in hole_keepouts():
        pts = [(hx + hr * math.cos(2 * math.pi * k / 24), hy + hr * math.sin(2 * math.pi * k / 24)) for k in range(24)]
        zone(None, "", pts, "metal spacer", keepout=True)
    for i, (px_, py_) in enumerate(POSTS):          # case post landing spots: no parts, copper allowed
        for layer in ("F.SilkS", "B.SilkS", "Cmts.User"):
            o.append(f'  (gr_circle (center {f(px_)} {f(py_)}) (end {f(px_ + POST_R - 0.3)} {f(py_)}) (stroke (width 0.12) (type default)) (fill none) (layer "{layer}") (tstamp {U("post" + layer + str(i))}))')
    o.append(")\n")
    with open(path, "w") as fh:
        fh.write("\n".join(o))



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
def build(write: bool = True):
    """Build schematic, placement and nets; write the KiCad files unless write is False."""
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
    ch.text("isolated side (VISO 3.3 V / ISOGND), RJ45: 3 TX out, 6 RX in, 4+5 GND; swapped by software", 96.52, 30.48, 1.5)
    ch.text("SWAP low (default) = Cisco console pinout, for a rollover cable. SWAP high = pins 3 and 6 crossed, for a straight patch cable.", 20.32, 142.0, 1.3)
    ch.text("LED off while the line idles (UART mark = LED off) -> no quiescent LED current.", 20.32, 115.0, 1.3)
    for p in tmpl:
        ch.place(p)
    for n, net in enumerate(["VISO", "ISOGND"]):
        ch.label(net, 215.9, 40.64 + 2.54 * n, 0, "local")
    if write:
        ch.write(os.path.join(OUT, "channel.kicad_sch"), "2")

    # ---- root sheet
    rparts = root_parts()
    root = Sheet("root", root_uid, rparts, set(), {}, PROJECT, [(root_path, {})], "RS232 isolation carrier - Waveshare ESP32-S3-Touch-LCD-2")
    root.text("Waveshare ESP32-S3-Touch-LCD-2 on two 1x14 sockets", 20.32, 25.4)
    root.text("Port power: VBAT -> MT3608 boost 5.1 V -> one high-side switch per port -> B0503S (channel sheet). EN high = port on.", 63.5, 20.32, 1.4)
    root.text("EN1..EN4 = GPIO16/17/18/10 (100k pull-down: off until enabled). SWAP1..4 = GPIO2/4/6/7: high = cross pins 3 and 6.", 63.5, 104.14, 1.3)
    root.text("J3: I2C (GPIO47/48), spare GPIO8, boot log TX (GPIO43), USB 5 V. J4/J5: battery, either plug, reverse protected by Q5.", 20.32, 152.4, 1.3)
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
                          {"ESP_TX": ("input", "L", f"ESP_TX{n}"), "ISO_IN": ("input", "L", f"ISO_IN{n}"), "SWAP": ("input", "L", f"SWAP{n}"),
                           "ESP_RX": ("output", "R", f"ESP_RX{n}")},
                          str(n + 1))
    if write:
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
                    full[k] = {"ESP_TX": f"ESP_TX{n}", "ESP_RX": f"ESP_RX{n}", "ISO_IN": f"ISO_IN{n}", "SWAP": f"SWAP{n}"}[v]
                elif v in CH_LOCAL_NETS:
                    full[k] = f"/CH{n}/{v}"
                else:
                    full[k] = v
            nets.update(v for v in full.values() if v != "NC")
            ip = Part(refmaps[n - 1][tp.ref], tp.sym, tp.fp, tp.value, tp.pins, xc + tp.x, tp.y, tp.rot, dnp=tp.dnp, props=tp.props, side=tp.side)
            if tp.ref == "J01":
                ip.y = BOARD_H + V["RJ45_PROTRUDE"] + V["RJ45_FRONT"]   # rot 180: front face at y + (-front)
            placed.append((ip, full, f"{root_path}/{sheet_uids[n - 1]}/{U('channel:' + tp.ref)}"))
    net_list = sorted(nets, key=lambda s: (s.startswith("/"), s))
    if not write:
        return placed, net_list
    write_pcb(os.path.join(OUT, f"{PROJECT}.kicad_pcb"), placed, net_list)
    write_project(os.path.join(OUT, f"{PROJECT}.kicad_pro"), root_uid, sheet_uids)
    with open(os.path.join(OUT, "fp-lib-table"), "w") as fh:
        fh.write("(fp_lib_table\n  (version 7)\n)\n")
    with open(os.path.join(OUT, "sym-lib-table"), "w") as fh:
        fh.write("(sym_lib_table\n  (version 7)\n)\n")
    return placed, net_list


# ---------------------------------------------------------------------------- checks + preview
def check(placed) -> int:
    """Placement checks.  Parts on the same side must not overlap; a bottom-side SMD part may sit under a
    through-hole body but not on its pins; nothing tall under the Waveshare module."""
    boxes = []
    errs = 0
    wx1, wy1 = ws_xy(WS["pcb"][2], WS["pcb"][1])
    wx2, wy2 = ws_xy(WS["pcb"][0], WS["pcb"][3])
    for p, _, _ in placed:
        fp = FPS[p.fp]
        b = part_box(p)
        boxes.append((p, b, fp))
        if b[0] < 0.3 or b[1] < 0.3 or b[2] > BOARD_W - 0.3 or b[3] > BOARD_H - 0.3:
            if not (p.fp == "RJ45_Generic" and b[3] > BOARD_H):   # the jack front may stand out
                print(f"EDGE   {p.ref:6s} {p.fp:22s} courtyard {tuple(round(v, 2) for v in b)}")
                errs += 1
        # the module's underside reaches down to 7.6 mm above the carrier (battery header): keep 0.6 mm
        under = not (b[2] < wx1 or b[0] > wx2 or b[3] < wy1 or b[1] > wy2)
        if under and p.side == "F" and fp.height > 7.0 and not p.fp.startswith("PinSocket"):
            print(f"HEIGHT {p.ref} {fp.height} mm under the Waveshare module")
            errs += 1

    def hit(a, b):
        return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]

    def pad_boxes(p, fp, grow=0.5):
        out = []
        for pad in fp.pads:
            x, y = pad_xy(p, pad)
            r = max(pad.w, pad.h, pad.drill) / 2 + grow
            out.append((x - r, y - r, x + r, y + r))
        return out
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            (pa, a, fa), (pb, b, fb) = boxes[i], boxes[j]
            tha, thb = fa.attr == "through_hole", fb.attr == "through_hole"
            if pa.side == pb.side:
                clash = hit(a, b)
            elif tha and not thb:        # SMD on the other side: only the pins of the through-hole part count
                clash = any(hit(q, b) for q in pad_boxes(pa, fa))
            elif thb and not tha:
                clash = any(hit(q, a) for q in pad_boxes(pb, fb))
            elif tha and thb:
                clash = any(hit(q, b) for q in pad_boxes(pa, fa)) or any(hit(q, a) for q in pad_boxes(pb, fb))
            else:
                clash = False            # SMD top vs SMD bottom
            if clash:
                ox = min(a[2], b[2]) - max(a[0], b[0])
                oy = min(a[3], b[3]) - max(a[1], b[1])
                print(f"OVERLAP {pa.ref:6s} x {pb.ref:6s}  {ox:.2f} x {oy:.2f} mm")
                errs += 1
    for px_, py_ in POSTS:
        pb = (px_ - POST_R, py_ - POST_R, px_ + POST_R, py_ + POST_R)
        for p, b, fp in boxes:
            dx = max(b[0] - px_, 0, px_ - b[2])
            dy = max(b[1] - py_, 0, py_ - b[3])
            if math.hypot(dx, dy) < POST_R:
                print(f"POST   {p.ref} is in the landing spot of the case post at ({px_}, {py_})")
                errs += 1
    # header rows against the vendor model: every socket pin must sit under its module pin
    want = {}
    for k in range(14):
        want[str(k + 1)] = ws_xy(-WS["hdr_x"], WS["hdr_y1"] - 2.54 * k)       # pins 1..14
        want[str(28 - k)] = ws_xy(+WS["hdr_x"], WS["hdr_y1"] - 2.54 * k)      # pins 28..15
    labels = dict(HDR_A); labels.update(HDR_B)
    for p, pin_nets, _ in placed:
        if not p.fp.startswith("PinSocket"):
            continue
        for pad in FPS[p.fp].pads:
            x, y = pad_xy(p, pad)
            wx, wy = want[pad.number]
            if abs(x - wx) > 0.01 or abs(y - wy) > 0.01 or pin_nets[pad.number] != hdr_net(labels[int(pad.number)]):
                print(f"HEADER pin {pad.number} at ({x:.2f}, {y:.2f}) net {pin_nets[pad.number]}, module pin is at ({wx:.2f}, {wy:.2f}) = {labels[int(pad.number)]}")
                errs += 1
    # the isolation barrier: bottom relays and optocouplers must straddle it with their LED side in the right domain
    for p, pin_nets, _ in placed:
        if p.fp in ("SOIC-8", "SO6_Opto"):
            for pad in FPS[p.fp].pads:
                x, y = pad_xy(p, pad)
                net = pin_nets.get(pad.number, "NC")
                iso = net.startswith("/CH") and net.split("/")[2] in CH_ISO_LOCALS
                if (iso and y < BARRIER[1]) or (not iso and net != "NC" and y > BARRIER[0]):
                    print(f"BARRIER {p.ref} pad {pad.number} ({net}) at y = {y:.2f} is on the wrong side")
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
    ax.add_patch(Rectangle((WS_X0, WS_Y0), V["WS_W"], V["WS_L"], fill=True, alpha=0.08, color="tab:blue", lw=1, ls="--"))
    ax.text(WS_X0 + 2, WS_Y0 + 20, "Waveshare\nESP32-S3-Touch-LCD-2\n(11 mm above, USB-C up)", fontsize=7, color="tab:blue")
    ax.add_patch(Rectangle((0.5, BARRIER[0]), BOARD_W - 1, BARRIER[1] - BARRIER[0], fill=True, alpha=0.15, color="red", lw=0))
    ax.text(BOARD_W / 2, (BARRIER[0] + BARRIER[1]) / 2, "isolation barrier (no copper)", ha="center", va="center", fontsize=7, color="red")
    for i, xc in enumerate(CH_CENTERS):
        ax.add_patch(Rectangle((xc - CH_PITCH / 2 + 0.5, BARRIER[1]), CH_PITCH - 1, BOARD_H - BARRIER[1] - 0.5, fill=True, alpha=0.06, color="tab:green", lw=0))
        ax.text(xc, BOARD_H - 21, f"PORT {i + 1}", ha="center", fontsize=8, color="tab:green")
    for p, _, _ in placed:
        fp = FPS[p.fp]
        b = part_box(p)
        col = "tab:orange" if fp.attr == "through_hole" else ("tab:purple" if p.side == "B" else "tab:gray")
        ax.add_patch(Rectangle((b[0], b[1]), b[2] - b[0], b[3] - b[1], fill=True, alpha=0.35, color=col, lw=0.5, ec="k"))
        for pad in fp.pads:
            px, py = pad_xy(p, pad)
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
    ("B0503S-1WR3", "SIP-4_DCDC"): ("Mornsun B0503S-1WR3", "5 V in, 3.3 V out, 1 W, 1.5 kV; NOT the 5 V-output B0505S. Verify pin spacing 2.54/7.62"),
    ("CPC2330N", "SOIC-8"): ("IXYS/Littelfuse CPC2330N", "1 Form A + 1 Form B PhotoMOS, 350 V, SOIC-8; bottom side. VERIFY the pinout before ordering"),
    ("PESD15VL2BT", "SOT-23"): ("Nexperia PESD15VL2BT", "two bidirectional 15 V TVS lines; protects the relay contacts and the MAX3232"),
    ("BSS138", "SOT-23"): ("onsemi BSS138", "logic-level N-MOSFET (Vth max 1.5 V), driven straight from a 3.3 V GPIO"),
    ("BAT JST-PH", "JST_PH_2_Vertical"): ("JST B2B-PH-K-S", "2 mm battery plug, bottom side; fit this OR the 1.25 mm header"),
    ("BAT MX1.25", "MX125_2_Vertical"): ("Molex 53047-0210", "1.25 mm battery plug as on the Waveshare module, bottom side"),
    ("RJ45", "RJ45_Generic"): ("Amphenol 54602-908LF", "8P8C tab-down, no magnetics, no LEDs; footprint must match the chosen part"),
    ("MT3608", "SOT-23-6"): ("Aerosemi MT3608", "boost VBAT -> 5.1 V; alt. TPS61023 (different footprint)"),
    ("SS14", "D_SMA"): ("Vishay SS14", "40 V Schottky, SMA"),
    ("1N4148W", "D_SOD-123"): ("Diodes Inc 1N4148W-7-F", ""),
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
            if fp in ("R_0603", "R_0805"):
                mpn, note = f"{value} {fp[2:]} 1%", ""
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
    sw = ", ".join(str(v[3]) for _, v in sorted(PORT_PINS.items()))
    return (f"#define PORT_DEFAULT_PINS {{{defaults}}}\n#define PINS_RX  {{{', '.join(map(str, rx))}}}\n#define PINS_TX  {{{', '.join(map(str, rx))}}}\n"
            f"#define PINS_PORT_EN {{{en}}}\n#define PINS_PORT_SWAP {{{sw}}}\n")


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
