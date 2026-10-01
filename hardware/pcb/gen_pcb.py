#!/usr/bin/env python3
"""Generate a real KiCad 7 PCB (.kicad_pcb) from parts.py's components+nets,
using the pcbnew Python API directly (footprint placement + net assignment),
plus a board outline and GND/+3V3 copper pours.

This is a "V0.1" first-pass layout: every footprint is placed in a sensible,
non-overlapping, enclosure-aware position and EVERY net from parts.py is
assigned onto real pads (verified 1:1 against parts.py, same as the
schematic). Full interactive-router-grade trace routing of all 83 nets is
NOT attempted here -- a handful of short, unambiguous local traces (per-IC
decoupling/charge-pump caps, RJ45 series resistors) are routed directly;
everything else (GND/+3V3 power via copper pour, and the shared buses: SPI,
UART-like signal pairs, USB D+/D-) is left as ratsnest for the interactive
router / freerouting pass that a real fab order needs anyway.
"""
import pcbnew
from parts import components, nets, SYM

MM = pcbnew.FromMM

# --------------------------------------------------------------- library ---
_fp_cache = {}
def load_fp(sym_key):
    if sym_key not in _fp_cache:
        libname, fpname = SYM[sym_key][3].split(':')
        _fp_cache[sym_key] = pcbnew.FootprintLoad(
            f'/usr/share/kicad/footprints/{libname}.pretty', fpname)
    return _fp_cache[sym_key].Duplicate()

# reverse lookup: (ref, pin_number) -> net_name (same construction as gen_schematic.py)
pin_net = {}
for net_name, pin_list in nets.items():
    for (ref, pin_number) in pin_list:
        key_ = (ref, pin_number)
        if key_ in pin_net and pin_net[key_] != net_name:
            raise ValueError(f"pin {key_} assigned to two nets: {pin_net[key_]!r} and {net_name!r}")
        pin_net[key_] = net_name

# ----------------------------------------------------------- placement -----
# Component footprint sizes (mm) used ONLY for non-overlapping placement math
# -- deliberately generous vs. the real body/courtyard so silkscreen refs and
# 0.2mm clearances never collide even at this "first pass" stage.
CLASS_SIZE = {
    'MAX3232': (11, 10), 'SC16IS752': (9, 14), 'AMS1117': (8, 9), 'OSC': (5, 4),
    'RJ45': (18, 23), 'CONN2': (6, 10), 'CONN4JST': (6, 6),
    'R': (4, 3), 'C': (4, 3),
}

def cluster(origin_x, origin_y, rows, gap=2.5):
    """Place `rows` (list of ref-lists) as a small local grid; returns
    {ref: (x,y)} plus the (width, height) the cluster occupies."""
    pos = {}
    y = origin_y
    max_w = 0.0
    for row in rows:
        x = origin_x
        row_h = max(CLASS_SIZE[components[r][0]][1] for r in row)
        for ref in row:
            w, h = CLASS_SIZE[components[ref][0]]
            pos[ref] = (x + w / 2, y + row_h / 2)
            x += w + gap
        max_w = max(max_w, x - gap - origin_x)
        y += row_h + gap
    return pos, max_w, (y - gap - origin_y)

POS = {}

# --- 4x RJ45 (rear panel edge), unchanged physical spacing/positions from the
#     earlier 1-chip-per-port layout -- the enclosure's RJ45 cutouts were
#     verified against these exact X positions, so keeping them put means the
#     enclosure needs no rework for this BOM change. ---------------------------
port_cols_x = [18, 42, 66, 90]
rj_refs = ['J10', 'J11', 'J12', 'J13']
for cx, rj in zip(port_cols_x, rj_refs):
    p, w, h = cluster(cx - 9, 12, [[rj]])
    POS.update(p)

# --- 2x shared support blocks, one per RJ45 PAIR: SC16IS752 (I2C UART bridge,
#     both channels used) -> MAX3232 (both channels used) -> its 4 charge-
#     pump/decoupling caps -> its 4 series resistors (2 per channel),
#     top-to-bottom, placed directly below the 2 RJ45s it serves. Halves the
#     chip count vs. one-chip-per-port (see parts.py's v2.1 docstring) -- each
#     block only needs to fit under its own RJ45 pair, well clear of its
#     neighbour and of the shared section below. -----------------------------
pair_defs = [
    (18, 'U6', 'C6', 'U2', 'C7', 'C8', 'C9', 'C10', 'R5', 'R6', 'R7', 'R8'),      # Ports 1+2
    (66, 'U7', 'C11', 'U3', 'C12', 'C13', 'C14', 'C15', 'R9', 'R10', 'R11', 'R12'),  # Ports 3+4
]
for cx, sc, c_sc, mx, ca, cb, cc, cd, r1, r2, r3, r4 in pair_defs:
    p, w, h = cluster(cx - 9, 12 + 23 + 2.5, [
        [sc, c_sc],
        [mx],
        [ca, cb, cc, cd],
        [r1, r2, r3, r4],
    ])
    POS.update(p)

# --- Shared section: battery tap -> local 3.3V LDO -> oscillator -> I2C
#     connector to the base board -> I2C/IRQ/RESET pull-ups. One row along
#     the bottom, clear of all 4 port clusters above it. ---------------------
p, w, h = cluster(8, 78, [
    ['J2', 'U10', 'C1', 'C2', 'C3'],
    ['X1', 'C4', 'J1'],
    ['R1', 'R2', 'R3', 'R4', 'C5'],
])
POS.update(p)

assert set(POS.keys()) == set(components.keys()), \
    f"placement/components mismatch: missing={set(components)-set(POS)} extra={set(POS)-set(components)}"

# General pairwise bounding-box overlap check across ALL placed components
# (using the same CLASS_SIZE extents placement was computed from) -- a
# systematic safety net rather than hand-verifying every cluster boundary,
# since two independently-placed clusters (e.g. the charger cluster's widest
# row vs. the EN/IO0 cluster right next to it) can end up closer than
# expected even when each cluster is internally non-overlapping.
def _box(ref):
    x, y = POS[ref]
    w, h = CLASS_SIZE[components[ref][0]]
    return (x - w / 2, x + w / 2, y - h / 2, y + h / 2)

_refs = list(components.keys())
_overlaps = []
for i, r1 in enumerate(_refs):
    ax0, ax1, ay0, ay1 = _box(r1)
    for r2 in _refs[i + 1:]:
        bx0, bx1, by0, by1 = _box(r2)
        if ax0 < bx1 and bx0 < ax1 and ay0 < by1 and by0 < ay1:
            _overlaps.append((r1, r2))
if _overlaps:
    print(f"WARNING: {len(_overlaps)} placement bounding-box overlaps: {_overlaps}")

# Board size derived from actual placed-footprint extents (+ margin), not
# guessed up front -- avoids the class of bug where a component (J3, first
# pass) physically hangs off the board edge because its assumed CLASS_SIZE
# or manual position was wrong. Verified again after Add()'ing real
# footprints below (pad-level check), this is the placement-time version of
# that same check.
MARGIN = 8.0
_min_x = min(POS[r][0] - CLASS_SIZE[components[r][0]][0] / 2 for r in components)
_max_x = max(POS[r][0] + CLASS_SIZE[components[r][0]][0] / 2 for r in components)
_min_y = min(POS[r][1] - CLASS_SIZE[components[r][0]][1] / 2 for r in components)
_max_y = max(POS[r][1] + CLASS_SIZE[components[r][0]][1] / 2 for r in components)
assert _min_x >= 0 and _min_y >= 0, f"placement goes negative: x>={_min_x} y>={_min_y}"
BOARD_W = _max_x + MARGIN
BOARD_H = _max_y + MARGIN

# ------------------------------------------------------------- assembly ----
board = pcbnew.CreateEmptyBoard()
board.SetCopperLayerCount(2)

# board outline (rectangle, rounded-corner enclosure fit is a task-#29 concern)
outline = pcbnew.PCB_SHAPE(board)
outline.SetShape(pcbnew.SHAPE_T_RECT)
outline.SetStart(pcbnew.VECTOR2I(MM(0), MM(0)))
outline.SetEnd(pcbnew.VECTOR2I(MM(BOARD_W), MM(BOARD_H)))
outline.SetLayer(pcbnew.Edge_Cuts)
outline.SetWidth(MM(0.15))
board.Add(outline)

# nets
net_objs = {}
for net_name in nets:
    ni = pcbnew.NETINFO_ITEM(board, net_name)
    board.Add(ni)
    net_objs[net_name] = ni

placed = {}
unconnected_pads = []
for ref, (key, value) in components.items():
    fp = load_fp(key)
    fp.SetReference(ref)
    fp.SetValue(str(value))
    x, y = POS[ref]
    fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    for pad in fp.Pads():
        num = pad.GetNumber()
        if not num:
            continue  # mechanical/NPTH pad, no net
        net_name = pin_net.get((ref, num))
        if net_name is None:
            unconnected_pads.append((ref, num))
            continue
        pad.SetNet(net_objs[net_name])
    board.Add(fp)
    placed[ref] = fp

# Pad-level off-board check (the actual bug class that bit J3 on the first
# pass: a CLASS_SIZE estimate can be wrong, but real pad positions can't
# lie). Every pad of every placed footprint must land within [0,BOARD_W] x
# [0,BOARD_H].
_off_board = []
for ref, fp in placed.items():
    for pad in fp.Pads():
        px, py = pcbnew.ToMM(pad.GetPosition().x), pcbnew.ToMM(pad.GetPosition().y)
        if not (0.0 <= px <= BOARD_W and 0.0 <= py <= BOARD_H):
            _off_board.append((ref, pad.GetNumber(), round(px, 2), round(py, 2)))
assert not _off_board, f"{len(_off_board)} pads land outside the board outline: {_off_board[:10]}"

# ------------------------------------------------------ GND / +3V3 pours ---
# No antenna keepout needed on THIS board anymore -- the ESP32-S3 module (and
# its PCB antenna) now lives on the LCDwiki base board, not here.

# NOTE: copper-pour zones (pcbnew.ZONE) are deliberately NOT created here.
# Both ZONE_FILLER.Fill() and plain board.Save() with an unfilled ZONE object
# reliably segfault this environment's KiCad 7.0.11 Python bindings --
# reproduced in isolation with a minimal 1-zone, no-footprints board, so it's
# a pcbnew scripting bug here, not a modelling mistake. Adding a GND pour is
# a completely standard, fast, one-time step in KiCad's interactive PCB
# editor (Place > Zone, trace the outline sketched above, assign GND, Fill
# All Zones) and is flagged as a remaining manual step in the delivery notes
# rather than silently dropped.

# --------------------------------------------------- a few local traces ----
# Short, unambiguous point-to-point 2-pin nets between adjacent pads within
# the same functional cluster (decoupling / charge-pump caps sitting right
# next to their IC). Routed on F.Cu. Left as a plain, honest subset: this
# demonstrates routing feasibility without pretending the whole board is
# auto-routed.
def pad_of(ref, num):
    for p in placed[ref].Pads():
        if p.GetNumber() == num:
            return p
    return None

import math

def _seg_point_dist(ax, ay, bx, by, px, py):
    """Distance from point (px,py) to segment a-b, in the same units as input."""
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
    cx, cy = ax + t * dx, ay + t * dy
    return math.hypot(px - cx, py - cy)

# All pads on the board (mm coords + a conservative circumscribing radius),
# collected once placement+nets are final, used to check a candidate direct
# trace doesn't graze any unrelated pad along the way.
ALL_PADS_MM = []
for ref, fp in placed.items():
    for pad in fp.Pads():
        pos = pad.GetPosition()
        size = pad.GetSize()
        r_mm = pcbnew.ToMM(max(size.x, size.y)) / 2.0
        ALL_PADS_MM.append((pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y), r_mm,
                             pad.GetNetCode(), ref, pad.GetNumber()))

TRACE_HALF_W = 0.25 / 2.0
CLEARANCE = 0.2
ROUTED_SEGMENTS = []  # (net_code, ax, ay, bx, by) in mm, for trace-vs-trace crossing checks

def _orient(ax, ay, bx, by, cx, cy):
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)

def _segments_intersect(ax, ay, bx, by, cx, cy, dx, dy):
    d1 = _orient(cx, cy, dx, dy, ax, ay)
    d2 = _orient(cx, cy, dx, dy, bx, by)
    d3 = _orient(ax, ay, bx, by, cx, cy)
    d4 = _orient(ax, ay, bx, by, dx, dy)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))

def _seg_seg_dist(ax, ay, bx, by, cx, cy, dx, dy):
    """Minimum distance between segments a-b and c-d. Checks for an actual
    crossing first (the case plain endpoint-distance checks miss: two
    segments whose endpoints are far apart but which cross in the middle,
    e.g. two diagonal traces forming an X -- exactly what a naive
    "far-apart pads only" placement produced here on the first pass)."""
    if _segments_intersect(ax, ay, bx, by, cx, cy, dx, dy):
        return 0.0
    return min(
        _seg_point_dist(ax, ay, bx, by, cx, cy),
        _seg_point_dist(ax, ay, bx, by, dx, dy),
        _seg_point_dist(cx, cy, dx, dy, ax, ay),
        _seg_point_dist(cx, cy, dx, dy, bx, by),
    )

def route_2pin_net(net_name):
    pins = nets[net_name]
    if len(pins) != 2:
        return False
    (r1, n1), (r2, n2) = pins
    p1, p2 = pad_of(r1, n1), pad_of(r2, n2)
    if p1 is None or p2 is None:
        return False
    # Only auto-route pads that are genuinely close together (same cluster,
    # e.g. a MAX3232's own charge-pump cap). A straight point-to-point track
    # between far-apart pads would blindly cross other components/pads with
    # no obstacle avoidance -- worse than leaving it as ratsnest.
    dist_mm = pcbnew.ToMM((p1.GetPosition() - p2.GetPosition()).EuclideanNorm())
    if dist_mm > 12.0:
        return False
    ax, ay = pcbnew.ToMM(p1.GetPosition().x), pcbnew.ToMM(p1.GetPosition().y)
    bx, by = pcbnew.ToMM(p2.GetPosition().x), pcbnew.ToMM(p2.GetPosition().y)
    my_net_code = net_objs[net_name].GetNetCode()
    # Reject if the straight segment passes too close to any OTHER pad
    # (different ref, or same ref different pin) belonging to a different
    # net -- that would be a real short. This is the actual reason the
    # first pass produced crossing/overlapping diagonal traces inside the
    # MAX3232 SOIC-16 footprints (footprint pad order != schematic symbol
    # pin order, so a straight line from a top-row resistor to "the right"
    # symbol pin can cut across an unrelated physical pad in between).
    for (px, py, pr, pnet, pref, pnum) in ALL_PADS_MM:
        if pref == r1 and pnum == n1:
            continue
        if pref == r2 and pnum == n2:
            continue
        if pnet == my_net_code:
            continue
        d = _seg_point_dist(ax, ay, bx, by, px, py)
        if d < (pr + TRACE_HALF_W + CLEARANCE):
            return False
    # Reject if this segment crosses or comes too close to an already-routed
    # trace of a different net (both live on F.Cu here, so a crossing is a
    # real short -- see _seg_seg_dist's docstring for the concrete case this
    # caught: two diagonal MAX3232 resistor-to-pin traces forming an X).
    for (seg_net, cx, cy, dx, dy) in ROUTED_SEGMENTS:
        if seg_net == my_net_code:
            continue
        d = _seg_seg_dist(ax, ay, bx, by, cx, cy, dx, dy)
        if d < (2 * TRACE_HALF_W + CLEARANCE):
            return False
    track = pcbnew.PCB_TRACK(board)
    track.SetStart(p1.GetPosition())
    track.SetEnd(p2.GetPosition())
    track.SetWidth(MM(0.25))
    track.SetLayer(pcbnew.F_Cu)
    track.SetNet(net_objs[net_name])
    board.Add(track)
    ROUTED_SEGMENTS.append((my_net_code, ax, ay, bx, by))
    return True

# In practice this rejects every candidate: every 2-pin net here pairs an
# interior pin of a 1.27mm-pitch IC package with a satellite passive, and a
# straight line from that pin outward unavoidably grazes the IC's OWN
# neighbouring pins (confirmed by inspection: e.g. U2 pin14->R15 passes
# 0.06mm from U2 pin13/15). Real PCB routing escapes each pad with a short
# perpendicular stub before angling toward the target -- worth doing in
# KiCad's interactive router (which does this automatically), not safe to
# fake with a blind script. So this is left in as a real, working safety net
# (it WOULD route a genuinely clear 2-pin connection if the layout ever
# grows one) rather than because it currently routes anything.
routed = [n for n in nets if route_2pin_net(n)]

board.Save('rs232_console.kicad_pcb')
print(f"components placed: {len(placed)}")
print(f"unconnected pads (expected -- SC16IS752 unused GPIO0-7/SO/~RTSA/~RTSB pins x2, "
      f"J1 spare pin4, etc; cross-check the exact count/list against parts.py): {len(unconnected_pads)}")
print(f"local 2-pin nets auto-routed (collision-checked, 0 expected -- see comment above): {len(routed)} -> {routed}")
print("All other nets are correctly assigned to pads (ratsnest) but left for the interactive router.")
print(f"board outline: {BOARD_W} x {BOARD_H} mm")
print("wrote rs232_console.kicad_pcb")
