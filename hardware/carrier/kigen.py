#!/usr/bin/env python3
"""Minimal KiCad 7 file writer (schematic + PCB) used by generate.py.

Everything is written as plain s-expressions.  Symbols and footprints are
embedded in the generated files, so the project opens without any external
library.  Coordinates: schematic in mm (y down, KiCad grid 1.27 mm), PCB in mm
(y down).  Symbol graphics are defined in library coordinates (y up) as KiCad
expects.
"""
import math
import uuid
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

NS = uuid.UUID("6f9e1c2a-7b1d-4a53-9c3e-2a7d8e5f1b00")


def U(name: str) -> str:
    """Deterministic UUID so re-running the generator keeps identities."""
    return str(uuid.uuid5(NS, name))


def f(x) -> str:
    if isinstance(x, float):
        s = f"{x:.4f}".rstrip("0").rstrip(".")
        return "0" if s in ("-0", "") else s
    return str(x)


# --------------------------------------------------------------------------- symbols
@dataclass
class SPin:
    number: str
    name: str
    side: str            # 'L' 'R' 'T' 'B'
    etype: str = "passive"   # input output bidirectional passive power_in power_out open_collector no_connect
    slot: int = 0        # position index along the side (0 = top / left)


@dataclass
class SymbolDef:
    name: str
    pins: List[SPin]
    kind: str = "box"    # box | res | cap | led | diode | pwr | flag | jumper3 | hole | npn
    width: float = 10.16  # box width (library units, mm)
    ref_prefix: str = "U"
    pin_len: float = 2.54
    power: bool = False
    extra_props: Dict[str, str] = field(default_factory=dict)

    # geometry helpers --------------------------------------------------------
    def box_height(self) -> float:
        nl = len([p for p in self.pins if p.side == "L"])
        nr = len([p for p in self.pins if p.side == "R"])
        n = max(nl, nr, 1)
        return (n + 1) * 2.54

    def pin_pos(self, p: SPin) -> Tuple[float, float, int]:
        """Library coordinates of the pin connection point and its angle."""
        if self.kind in ("res", "cap", "led", "diode"):
            # horizontal two-pin device, pin 1 left, pin 2 right
            if p.number == "1":
                return (-3.81, 0.0, 0)
            return (3.81, 0.0, 180)
        if self.kind == "jumper3":
            return {"1": (-5.08, 0.0, 0), "2": (0.0, -2.54, 90), "3": (5.08, 0.0, 180)}[p.number]
        if self.kind in ("pwr", "flag"):
            return (0.0, 0.0, 270 if self.name in ("GND",) else 90)
        if self.kind == "npn":
            return {"1": (-5.08, 0.0, 0), "2": (0.0, -5.08, 90), "3": (0.0, 5.08, 270)}[p.number]
        # box
        h = self.box_height()
        hw = self.width / 2
        top = h / 2 - 2.54
        if p.side == "L":
            return (-(hw + self.pin_len), top - p.slot * 2.54, 0)
        if p.side == "R":
            return ((hw + self.pin_len), top - p.slot * 2.54, 180)
        if p.side == "T":
            return (-hw + 2.54 + p.slot * 2.54, h / 2 + self.pin_len, 270)
        return (-hw + 2.54 + p.slot * 2.54, -(h / 2 + self.pin_len), 90)

    # s-expression ------------------------------------------------------------
    def sexp(self, lib: str) -> str:
        n = f"{lib}:{self.name}"
        o = [f'  (symbol "{n}"']
        if self.power:
            o.append("    (power)")
        if self.kind in ("res", "cap", "led", "diode", "jumper3", "pwr", "flag", "hole"):
            o.append("    (pin_numbers hide)")
        o.append("    (pin_names (offset 0.508))" if self.kind == "box" else "    (pin_names (offset 0))")
        o.append("    (in_bom yes) (on_board yes)")
        hide_ref = " hide" if self.power else ""
        h = self.box_height() if self.kind == "box" else 5.08
        o.append(f'    (property "Reference" "{self.ref_prefix}" (at 0 {f(h/2+1.27)} 0) (effects (font (size 1.27 1.27)){hide_ref}))')
        o.append(f'    (property "Value" "{self.name}" (at 0 {f(-(h/2+1.27))} 0) (effects (font (size 1.27 1.27))))')
        o.append('    (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
        o.append('    (property "Datasheet" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
        for k, v in self.extra_props.items():
            o.append(f'    (property "{k}" "{v}" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))')
        # graphics
        g = [f'    (symbol "{self.name}_0_1"']
        st = "(stroke (width 0.254) (type default))"
        if self.kind == "box":
            hw = self.width / 2
            g.append(f"      (rectangle (start {f(-hw)} {f(h/2)}) (end {f(hw)} {f(-h/2)}) {st} (fill (type background)))")
        elif self.kind == "res":
            g.append(f"      (rectangle (start -2.54 1.016) (end 2.54 -1.016) {st} (fill (type none)))")
        elif self.kind == "cap":
            g.append(f"      (polyline (pts (xy -0.508 1.905) (xy -0.508 -1.905)) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy 0.508 1.905) (xy 0.508 -1.905)) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy -3.81 0) (xy -0.508 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy 0.508 0) (xy 3.81 0)) (stroke (width 0) (type default)) (fill (type none)))")
        elif self.kind in ("led", "diode"):
            g.append(f"      (polyline (pts (xy -1.27 1.27) (xy -1.27 -1.27) (xy 1.27 0) (xy -1.27 1.27)) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy 1.27 1.27) (xy 1.27 -1.27)) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy -3.81 0) (xy -1.27 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy 1.27 0) (xy 3.81 0)) (stroke (width 0) (type default)) (fill (type none)))")
            if self.kind == "led":
                g.append(f"      (polyline (pts (xy -0.5 2.0) (xy 0.5 3.0)) {st} (fill (type none)))")
                g.append(f"      (polyline (pts (xy 0.5 2.0) (xy 1.5 3.0)) {st} (fill (type none)))")
        elif self.kind == "jumper3":
            g.append(f"      (arc (start -3.81 0) (mid -2.54 1.27) (end -1.27 0) {st} (fill (type none)))")
            g.append(f"      (arc (start 1.27 0) (mid 2.54 1.27) (end 3.81 0) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy -5.08 0) (xy -3.81 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy 3.81 0) (xy 5.08 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy -1.27 0) (xy 1.27 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy 0 0) (xy 0 -2.54)) (stroke (width 0) (type default)) (fill (type none)))")
        elif self.kind == "npn":
            g.append(f"      (polyline (pts (xy -5.08 0) (xy -1.27 0)) (stroke (width 0) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy -1.27 2.54) (xy -1.27 -2.54)) (stroke (width 0.508) (type default)) (fill (type none)))")
            g.append(f"      (polyline (pts (xy -1.27 1.27) (xy 0 2.54) (xy 0 5.08)) {st} (fill (type none)))")
            g.append(f"      (polyline (pts (xy -1.27 -1.27) (xy 0 -2.54) (xy 0 -5.08)) {st} (fill (type none)))")
            g.append(f"      (circle (center 0 0) (radius 2.8) {st} (fill (type none)))")
        elif self.kind == "pwr":
            if self.name == "GND":
                g.append(f"      (polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27)) {st} (fill (type none)))")
            else:
                g.append(f"      (polyline (pts (xy -0.762 1.27) (xy 0 2.54)) {st} (fill (type none)))")
                g.append(f"      (polyline (pts (xy 0 0) (xy 0 2.54)) {st} (fill (type none)))")
                g.append(f"      (polyline (pts (xy 0 2.54) (xy 0.762 1.27)) {st} (fill (type none)))")
        elif self.kind == "flag":
            g.append(f"      (polyline (pts (xy 0 0) (xy 0 1.27) (xy -1.016 1.905) (xy 0 2.54) (xy 1.016 1.905) (xy 0 1.27)) {st} (fill (type none)))")
        elif self.kind == "hole":
            g.append(f"      (circle (center 0 0) (radius 1.27) {st} (fill (type none)))")
        g.append("    )")
        o += g
        # pins
        pp = [f'    (symbol "{self.name}_1_1"']
        for p in self.pins:
            x, y, a = self.pin_pos(p)
            ln = 0 if self.kind in ("pwr", "flag") else (1.27 if self.kind in ("res", "cap", "led", "diode", "jumper3", "npn") else self.pin_len)
            nm = p.name if self.kind == "box" else "~"
            if self.kind in ("pwr", "flag"):
                nm = self.name if self.kind == "pwr" else "pwr"
            pp.append(f'      (pin {p.etype} line (at {f(x)} {f(y)} {a}) (length {f(ln)})'
                      f' (name "{nm}" (effects (font (size 1.27 1.27)))) (number "{p.number}" (effects (font (size 1.27 1.27)))))')
        pp.append("    )")
        o += pp
        o.append("  )")
        return "\n".join(o)


# --------------------------------------------------------------------------- footprints
@dataclass
class Pad:
    number: str
    x: float
    y: float
    w: float
    h: float
    kind: str = "smd"    # smd | tht | npth
    shape: str = "roundrect"   # roundrect | rect | circle | oval
    drill: float = 0.0
    rot: float = 0.0


@dataclass
class FootprintDef:
    name: str
    pads: List[Pad]
    courtyard: Tuple[float, float, float, float]   # x1 y1 x2 y2
    fab: Optional[Tuple[float, float, float, float]] = None
    silk_lines: List[Tuple[float, float, float, float]] = field(default_factory=list)
    fab_lines: List[Tuple[float, float, float, float]] = field(default_factory=list)
    attr: str = "smd"    # smd | through_hole
    polys: List[Tuple[str, List[Tuple[float, float]]]] = field(default_factory=list)   # (layer, pts)
    net_tie: str = ""
    height: float = 1.0  # component height, for the enclosure check
    note: str = ""

    def sexp(self, lib: str, ref: str, value: str, x: float, y: float, rot: float,
             nets: Dict[str, Tuple[int, str]], path: str, uid: str, dnp: bool = False,
             props: Optional[Dict[str, str]] = None) -> str:
        o = [f'  (footprint "{lib}:{self.name}" (layer "F.Cu")', f"    (tstamp {uid})",
             f"    (at {f(x)} {f(y)} {f(rot)})"]
        if self.note:
            o.append(f'    (descr "{self.note}")')
        o.append(f'    (property "Sheetname" "")')
        for k, v in (props or {}).items():
            o.append(f'    (property "{k}" "{v}")')
        o.append(f'    (path "{path}")')
        o.append(f"    (attr {self.attr}{' dnp' if dnp else ''})")
        if self.net_tie:
            o.append(f'    (net_tie_pad_groups "{self.net_tie}")')
        cy = self.courtyard
        ry = cy[3] + 1.0
        ref_layer = "F.Fab" if (len(self.pads) <= 3 and self.attr == "smd") or not self.pads or all(p.kind == "npth" for p in self.pads) else "F.SilkS"
        o.append(f'    (fp_text reference "{ref}" (at 0 {f(cy[1]-0.7)} {f(rot)}) (layer "{ref_layer}") (tstamp {U(uid+"ref")})'
                 f" (effects (font (size 0.6 0.6) (thickness 0.1))))")
        o.append(f'    (fp_text value "{value}" (at 0 {f(ry)} {f(rot)}) (layer "F.Fab") (tstamp {U(uid+"val")})'
                 f" (effects (font (size 0.8 0.8) (thickness 0.12))))")
        o.append(f"    (fp_rect (start {f(cy[0])} {f(cy[1])}) (end {f(cy[2])} {f(cy[3])}) (stroke (width 0.05) (type default)) (fill none) (layer \"F.CrtYd\") (tstamp {U(uid+'cy')}))")
        if self.fab:
            fb = self.fab
            o.append(f"    (fp_rect (start {f(fb[0])} {f(fb[1])}) (end {f(fb[2])} {f(fb[3])}) (stroke (width 0.1) (type default)) (fill none) (layer \"F.Fab\") (tstamp {U(uid+'fab')}))")
        for i, (x1, y1, x2, y2) in enumerate(self.silk_lines):
            o.append(f"    (fp_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.12) (type solid)) (layer \"F.SilkS\") (tstamp {U(uid+'sl'+str(i))}))")
        for i, (x1, y1, x2, y2) in enumerate(self.fab_lines):
            o.append(f"    (fp_line (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.1) (type solid)) (layer \"F.Fab\") (tstamp {U(uid+'fl'+str(i))}))")
        for i, (layer, pts) in enumerate(self.polys):
            p = " ".join(f"(xy {f(a)} {f(b)})" for a, b in pts)
            o.append(f"    (fp_poly (pts {p}) (stroke (width 0) (type solid)) (fill solid) (layer \"{layer}\") (tstamp {U(uid+'po'+str(i))}))")
        for p in self.pads:
            prot = f(rot + p.rot)
            if p.kind == "npth":
                o.append(f'    (pad "" np_thru_hole circle (at {f(p.x)} {f(p.y)} {prot}) (size {f(p.w)} {f(p.h)}) (drill {f(p.drill)}) (layers "*.Cu" "*.Mask") (tstamp {U(uid+"pad"+p.number+str(p.x)+str(p.y))}))')
                continue
            net = nets.get(p.number)
            nets_s = f' (net {net[0]} "{net[1]}")' if net else ""
            if p.kind == "tht":
                shape = p.shape
                o.append(f'    (pad "{p.number}" thru_hole {shape} (at {f(p.x)} {f(p.y)} {prot}) (size {f(p.w)} {f(p.h)}) (drill {f(p.drill)}) (layers "*.Cu" "*.Mask"){nets_s} (remove_unused_layers) (tstamp {U(uid+"pad"+p.number)}))')
            else:
                rr = " (roundrect_rratio 0.25)" if p.shape == "roundrect" else ""
                o.append(f'    (pad "{p.number}" smd {p.shape} (at {f(p.x)} {f(p.y)} {prot}) (size {f(p.w)} {f(p.h)}) (layers "F.Cu" "F.Paste" "F.Mask"){rr}{nets_s} (tstamp {U(uid+"pad"+p.number)}))')
        o.append("  )")
        return "\n".join(o)

    def courtyard_world(self, x: float, y: float, rot: float) -> Tuple[float, float, float, float]:
        x1, y1, x2, y2 = self.courtyard
        pts = [(x1, y1), (x2, y1), (x2, y2), (x1, y2)]
        r = math.radians(rot)
        c, s = math.cos(r), math.sin(r)
        # KiCad rotates counter-clockwise for positive angles in a y-down system:
        # world = (x + px*c + py*s, y - px*s + py*c)
        w = [(x + px * c + py * s, y - px * s + py * c) for px, py in pts]
        xs = [p[0] for p in w]
        ys = [p[1] for p in w]
        return (min(xs), min(ys), max(xs), max(ys))


def rect_lines(x1, y1, x2, y2):
    return [(x1, y1, x2, y1), (x2, y1, x2, y2), (x2, y2, x1, y2), (x1, y2, x1, y1)]


# standard 2-layer stack
LAYERS = """  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)
    (50 "User.1" user)
    (51 "User.2" user)
  )"""

SETUP = """  (setup
    (pad_to_mask_clearance 0)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (disableapertmacros false)
      (usegerberextensions false)
      (usegerberattributes true)
      (usegerberadvancedattributes true)
      (creategerberjobfile true)
      (dashed_line_dash_ratio 12.000000)
      (dashed_line_gap_ratio 3.000000)
      (svgprecision 4)
      (plotframeref false)
      (viasonmask false)
      (mode 1)
      (useauxorigin false)
      (hpglpennumber 1)
      (hpglpenspeed 20)
      (hpglpendiameter 15.000000)
      (dxfpolygonmode false)
      (dxfimperialunits true)
      (dxfusepcbnewfont true)
      (psnegative false)
      (psa4output false)
      (plotreference true)
      (plotvalue true)
      (plotinvisibletext false)
      (sketchpadsonfab false)
      (subtractmaskfromsilk false)
      (outputformat 1)
      (mirror false)
      (drillshape 1)
      (scaleselection 1)
      (outputdirectory "")
    )
  )"""
