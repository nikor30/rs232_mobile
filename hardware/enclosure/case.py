#!/usr/bin/env python3
"""Case for the RS232 console, revision 0.5 - real solid CAD (CadQuery / OpenCascade).

    <venv>/bin/python case.py            # STEP + STL of every part, assembly STEP, checks
    <venv>/bin/python case.py --no-vendor   # skip the checks against the vendor STEP
    <venv>/bin/python case.py --hat      # the same case around the two-port prototype board (hardware/hat), into out_hat/

Needs CadQuery (pip install cadquery).  All numbers of the Waveshare module come from the vendor's
own 3D model (carrier/generate.py: WS), the board outline and the part positions from the same
generator, so the case always fits the board that was generated.

Device coordinate system (right handed):
    X = KiCad x of the carrier, Y = BOARD_H - KiCad y (RJ45 edge at Y = 0, USB-C end at Y ~ 110),
    Z = up, Z = 0 on the top face of the carrier as it sits in the case.

Built for a PLA printer without support material.  The rules it follows (learned from the first print):
  * nothing starts in mid-air: no face flatter than 45 degrees, no bridge longer than about 5 mm
  * nothing slender stands upright under bending: no pins, posts are 5 mm or thicker
  * what has to flex lies flat on the bed and flexes in its own plane
  * no break-away supports inside the part

Three printed parts, nothing loose:
  body   top skin, side walls and corner pieces.  The skin is as thick as the display glass and has an
         opening the size of the glass, so the glass ends up flush with the top.  Both ends are open
         down to the rim: nothing in the body has to bridge.
  frame  lies under the skin and is what the module is clamped in: the glass rests on it, two spring
         jaws press the module's board against the opposite side.  The two keys (RST, BOOT) are part
         of it: a paddle each in the opening of the USB-C end, on a leaf spring that stands on the bed
         and bends in the plane of the layers.  No tunnels, no loose keys.  Printed flat, seat down.
  base   a plate inside the walls with snap arms that flex in the plane of the plate.  It closes the
         two open ends (a sill under the jacks, a tongue under the USB-C port), presses the board
         against the frame's legs and the body's posts, and carries the battery.
Assembly: module into the frame from the glass side (it clicks between the jaws), frame into the
body lying face down, wires, board, battery, base.  No screws, no glue.
"""
import math
import os
import re
import sys

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "carrier"))
import generate as G   # noqa: E402

WS = G.WS
VARIANT = "hat" if "--hat" in sys.argv else "carrier"
if VARIANT == "hat":
    sys.path.insert(0, os.path.join(HERE, "..", "hat"))
    import board as HB   # noqa: E402
OUT = os.path.join(HERE, "out_hat" if VARIANT == "hat" else "out")
VENDOR_STEP = os.path.join(HERE, "..", "vendor", "esp32-s3-touch-lcd-2_20241108.stp")

# ----------------------------------------------------------------------------- parameters
P = dict(
    wall=2.4,            # side walls and USB-C end
    wall_rj=1.6,         # port end: thin, so the jacks sit close behind the outer face
    gap=0.5,             # carrier to side walls
    gap_rj=0.3,          # carrier edge to the port wall
    r_out=6.0,           # outer vertical corner radius
    r_top=1.2,           # 45 deg chamfer of the top edge (the top lies on the bed: a chamfer prints clean, a radius does not)
    plug_gap=0.3,        # the module sits this much above "fully plugged": the glass always seats first
    top_t=3.0,           # top plate where there is no display
    glass_clr=0.25,      # glass to the edge of its opening (lens tolerance +-0.05, FDM)
    glass_recess=0.1,    # the glass lies this far below the top face: the skin is then 1.2 thick, six layers of 0.2 mm
    bot_clr=2.6,         # under the carrier: through-hole pins 2 mm, relays 1.75 mm
    bat_t=8.5,           # battery bay height under the carrier (four-port case)
    base_t=2.0, base_inset=0.6, base_clr=0.25,
    post_d=5.0, post_root=1.5, dome_h=1.2,
    # snap arms (in-plane cantilevers on the base)
    arm_l=18.0, arm_t=1.6, arm_h=5.0, arm_slot=1.6,     # long and slim for PLA: 0.65 % strain when snapping in
    bump=0.9, bump_l=4.0, bump_land=0.6, lead_deg=30.0, preload=0.2, recess_d=1.0,
    # the frame under the display
    frame_t=6.0,         # thickness: covers the module's board and the spacer above it
    frame_over=3.0,      # how far it reaches beyond the glass at the antenna end
    frame_clr=0.2,       # frame to the walls and to the step in the top plate
    stack_clr=0.10,      # module board to the frame's fixed long side (the jaws press it there)
    stop_clr=0.15,       # module board to the flat stop at the USB-C end
    crush_room=0.35,     # room at the antenna end, with two ribs in it that the board's edge shaves to fit
    crush_h=0.45,
    jaw_gap=0.6,         # free room between the module board and a jaw
    jaw_bite=0.4,        # a jaw's pad stands this far into the board's place: the jaw is bent by that much
    jaw_l=19.0, jaw_t=2.6, jaw_slot=0.8, jaw_pad=4.0, jaw_root=6.0,
    # USB-C end: one opening in the end wall for the port and the two keys, closed below by the tongue of the base
    io_w=28.4,
    cut_clr=0.2,         # sill and tongue of the base to the edges of their openings
    usb_w=13.4, usb_h=7.5,               # room for the plug's overmould (USB-IF maximum 12.35 x 6.5)
    # the two keys: a paddle each beside the port, on a leaf spring that is part of the frame
    key_leaf_t=0.84,                     # leaf: two lines of a 0.4 mm nozzle
    key_leaf_l=14.0,                     # its bending length, where the frame is wide enough
    key_slot=1.0,                        # room behind the leaf
    key_clr=0.4,                         # paddle to the edges of the opening, to the plug's room and to the tongue
    key_side=0.7,                        # paddle block to the fixed end of the frame: it swings a little towards it
    key_land=0.4,                        # the wall above the opening keeps this much of its lower edge, the rest is bevelled
    key_under=dict(BOOT=0.2, RST=1.0),   # paddle face below the outer face: RST lies deeper, a finger finds BOOT
    key_gap=0.2,                         # nose to switch plunger at rest
    key_travel=0.3,                      # what the switch needs to click (assumed, not from a data sheet)
    key_stroke=0.8,                      # nose travel at which the leaf meets its stop
    key_nose_w=3.0, key_nose_h=1.7,
    key_ledge=0.6,                       # the only flat overhang of the frame: under the nose, a line and a half wide
    rj_clr=0.3,
    hold_gap=0.3,        # two-port case: two walls of the frame end this far above the battery and keep it down
)
# Printing material: name, modulus (MPa), strain a snap arm may see when it is pushed in.  PLA is stiff and
# brittle; PETG would be ("PETG", 2000.0, 0.02).
MATERIAL = ("PLA", 3300.0, 0.010)

if VARIANT == "hat":
    # Two-port prototype board: 50 x 50 mm with square corners.  It lies at the bottom.  The module is wired
    # to it with jumper wires whose plugs hang 14 mm down from its pins, so the module sits high above the
    # board.  The battery (40 x 30 x 7.8 mm) lies flat on the base behind the board, under the module's
    # USB-C half: it is too wide to lie between the plugs.  That sets how far the module sits towards the
    # USB-C end, and with it the length of the case.
    P.update(gap=1.0, gap_rj=1.0, r_out=5.0,      # room for the board's square corners inside the rounded cavity
             bot_clr=HB.PIN_BELOW + 0.5,          # the board's solder tails just clear the base
             bat_t=0.0)                           # no battery bay under the board
    W, H, BOARD_R = HB.W, HB.H, 0.0
    WS_XC = HB.WS_XC
    CELL = HB.BATTERY                             # across, along, thickness
    CELL_Y1 = H + 2.0                             # behind the board: 0.5 mm of air, a stop rib, then the cell
    CELL_POCKET = 0.8                             # the cell lies this deep in the base plate: room for the wires above it
    _cy2 = CELL_Y1 + CELL[1] + 0.3 + 0.9          # inner face of the USB-C end wall
    WS_YC = H - WS["glass"][1] + P["glass_clr"] - _cy2

    def ws_xy(xv, yv):
        return WS_XC - xv, WS_YC + yv
    # vendor z -> device z: header body, plug, room for the wire to turn, and 1 mm above the bare board
    MODULE_Z = -WS["hdr_plastic_z"][0] + HB.PLUG_LEN + HB.WIRE_BEND + 1.0
    JACKS, JACK_W, JACK_H = HB.JACK_X, HB.JACK_W, HB.JACK_H
    JACK_FRONT = H - HB.JACK_FRONT                # the jack fronts sit this far inside the board edge
    POST_XY, HOLE_D = HB.HOLES, HB.HOLE_D
    ARM_STARTS = [("L", 12.0), ("L", 58.0), ("R", 12.0), ("R", 58.0)]
    PRY_Y = 41.0
    LABEL = (3.4, 30.0)
else:
    W, H, BOARD_R = G.BOARD_W, G.BOARD_H, G.BOARD_R
    WS_XC, ws_xy = G.WS_XC, G.ws_xy
    MODULE_Z = G.WS_Z0 + P["plug_gap"]
    JACKS, JACK_W, JACK_H, JACK_FRONT = G.CH_CENTERS, G.V["RJ45_W"], G.V["RJ45_H"], 0.0
    POST_XY, HOLE_D = G.POSTS, None
    ARM_STARTS = [("L", 22.0), ("L", 80.0), ("R", 22.0), ("R", 80.0)]
    PRY_Y = 55.0
    CELL = (40.0, 30.0, 7.8)                      # the same cell, in the bay under the carrier
    CELL_Y1, CELL_POCKET = None, 0.0
    LABEL = (4.5, 30.0)


def dev(xk, yk):
    """KiCad board coordinates -> device coordinates."""
    return xk, H - yk


def ws_dev(xv, yv):
    """Vendor module coordinates -> device X, Y."""
    return dev(*ws_xy(xv, yv))


ZM = MODULE_Z                                # vendor z -> device z
Z_GLASS = WS["glass_z"][0] + ZM              # glass underside = seat on the frame = underside of the top skin
Z_GLASS_TOP = WS["glass_z"][1] + ZM          # glass front
Z_TOP = Z_GLASS_TOP + P["glass_recess"]      # top face of the body
Z_PLATE = Z_TOP - P["top_t"]
Z_CARR_BOT = -1.6
Z_BAY = Z_CARR_BOT - P["bot_clr"]
Z_BASE_TOP = Z_BAY - P["bat_t"]
Z_BASE_BOT = Z_BASE_TOP - P["base_t"]
Z_BOT = Z_BASE_BOT - P["base_inset"]

# cavity and outer box
GX1, GY1 = ws_dev(WS["glass"][2], WS["glass"][3])      # glass rectangle in device coordinates
GX2, GY2 = ws_dev(WS["glass"][0], WS["glass"][1])
PBX1, PBY1 = ws_dev(WS["pcb"][2], WS["pcb"][3])        # the module's board
PBX2, PBY2 = ws_dev(WS["pcb"][0], WS["pcb"][1])
CX1, CX2 = -P["gap"], W + P["gap"]
CY1 = -P["gap_rj"]
CY2 = GY2 + P["glass_clr"]                             # the edge of the glass opening is the inner face of the USB-C end
OX1, OX2 = CX1 - P["wall"], CX2 + P["wall"]
OY1, OY2 = CY1 - P["wall_rj"], CY2 + P["wall"]
R_IN = P["r_out"] - P["wall"]

# the frame
Z_FT, Z_FB = Z_GLASS, Z_GLASS - P["frame_t"]
FX1, FX2 = CX1 + P["frame_clr"], CX2 - P["frame_clr"]
FY1 = GY1 - P["glass_clr"] - P["frame_over"]
FY2 = CY2 - P["frame_clr"]
JAW_X = PBX2 + P["jaw_gap"]                            # face of the jaws at rest; the fixed stop is on the other long side

# USB-C end
USB_X, USB_Z = WS_XC, WS["usbc_axis_z"] + ZM           # axis of the receptacle
USB_RIM_Y = ws_dev(0, WS["usbc_front_y"])[1]           # front edge of the receptacle shell
USB_BACK_Y = ws_dev(0, WS["usbc_back_y"])[1]           # where a plug's shell stops
KEY_Z = WS["button_act_z"] + ZM                        # axis of the switch plungers
KEYS = {name: ws_dev(xv, yv) for name, (xv, yv) in WS["buttons"].items()}   # x of the plunger, y of its tip
KEY_TIP_Y = max(y for _, y in KEYS.values())           # the two tips differ by 0.025 mm: one key fits both
KEY_STOP_Y = PBY2 + P["stop_clr"]                      # inner face of the frame's USB-C end: the board's stop
R_FRAME = R_IN - P["frame_clr"]                        # corner radius of the frame
IO_X1, IO_X2 = USB_X - P["io_w"] / 2, USB_X + P["io_w"] / 2
Z_IO_TOP = USB_Z + P["usb_h"] / 2 + P["key_clr"]       # upper edge of the opening: just above the plug's room
Z_TONGUE = USB_Z - P["usb_h"] / 2                      # top of the base's tongue = lower edge of the plug's room
Z_NOSE = KEY_Z + WS["button_act"][1] / 2               # top of a key's nose = top of the plunger, 0.4 under the module's board

# port end: one slot for all jacks, open down to the rim, closed below by the sill of the base
SLOT_X1, SLOT_X2 = JACKS[0] - JACK_W / 2 - P["rj_clr"], JACKS[-1] + JACK_W / 2 + P["rj_clr"]
Z_SLOT_TOP = JACK_H + P["rj_clr"] + 0.1
Z_SILL = -P["rj_clr"]

POSTS = [dev(x, y) for x, y in POST_XY]
BODY_POSTS = [p for p in POSTS if p[1] < FY1 - 4.0]    # where the top plate is thick: posts of the body press on the board
LEG_POSTS = [p for p in POSTS if p[1] >= FY1 - 4.0]    # under the frame: its legs do
ARMS = ARM_STARTS                                      # side, start along the wall

if VARIANT == "hat":
    CELL_X = (WS_XC - CELL[0] / 2, WS_XC + CELL[0] / 2)
    CELL_Z = (Z_BASE_TOP - CELL_POCKET, Z_BASE_TOP - CELL_POCKET + CELL[2])
    Z_TOWER = CELL_Z[1] + P["hold_gap"]                # two walls of the frame reach down to the cell and keep it in its tray
    # The frame's legs stand on the rear corners of the board (free of parts there).  On the jaw side there
    # is only the outer rail to grow from: that leg is 3 mm thick, so it is made a wall 12 mm long.
    _xj = JAW_X + P["jaw_t"] + P["jaw_slot"] + 0.4
    LEGS = [(0.2, H - 6.0, 5.0, H - 0.3), (_xj, H - 12.0, W - 0.1, H - 0.3)]
    # Hold-downs for the battery: an angle at each corner of the USB-C end, under the fixed part of the
    # frame's end (the keys are in between).  (x1, y1, x2, y2) of the two legs of each angle.
    _ya, _yb = KEY_STOP_Y + 0.3, FY2 - P["key_leaf_t"] - P["key_slot"] - 0.3
    _xa = USB_X - P["io_w"] / 2 + P["key_clr"] - P["key_side"] - 0.3
    _xb = USB_X + P["io_w"] / 2 - P["key_clr"] + P["key_side"] + 0.3
    HOLDS = [(0.1, _ya, _xa, _yb), (0.1, _ya - 7.0, 3.1, _yb), (_xb, _ya, W - 0.1, _yb), (_xj, _ya - 7.0, W - 0.1, _yb)]
else:
    Z_TOWER = Z_FB
    LEGS = []
    HOLDS = []


# ----------------------------------------------------------------------------- helpers
def box(x1, y1, x2, y2, z1, z2):
    return cq.Workplane("XY").box(x2 - x1, y2 - y1, z2 - z1, centered=False).translate((x1, y1, z1))


def rbox(x1, y1, x2, y2, z1, z2, r):
    return (cq.Workplane("XY").workplane(offset=z1).center((x1 + x2) / 2, (y1 + y2) / 2)
            .rect(x2 - x1, y2 - y1).extrude(z2 - z1).edges("|Z").fillet(r))


def cyl(x, y, z1, z2, d):
    return cq.Workplane("XY").workplane(offset=z1).center(x, y).circle(d / 2).extrude(z2 - z1)


def prism_y(profile, y1, y2):
    """Extrude an (x, z) polygon along Y."""
    return cq.Workplane("XZ").workplane(offset=-y2).polyline(profile).close().extrude(y2 - y1)


def prism_x(profile, x1, x2):
    """Extrude a (y, z) polygon along X."""
    return cq.Workplane("YZ").workplane(offset=x1).polyline(profile).close().extrude(x2 - x1)


def vol(shape):
    try:
        return shape.val().Volume()
    except Exception:
        return 0.0


# ----------------------------------------------------------------------------- snap geometry
def bump_profile(z0):
    """(r, z) outline of the catch, r = outwards from the arm face: 45 deg retaining face below,
    a short land, a long lead-in above (the base moves +z into the body)."""
    b = P["bump"]
    lead = b / math.tan(math.radians(P["lead_deg"]))
    return [(0, z0), (b, z0 + b), (b, z0 + b + P["bump_land"]), (0, z0 + b + P["bump_land"] + lead)]


def recess_profile(z0):
    """(r, z) outline of the pocket in the wall, r = into the wall from its inner face.  Its 45 deg
    face sits `preload` higher than the catch needs, so the arm stays slightly bent and pulls the
    base home (takes up the tolerances of the stack)."""
    d = P["recess_d"]
    zr = z0 + P["base_clr"] + P["preload"]
    top = z0 + P["bump"] + P["bump_land"] + P["bump"] / math.tan(math.radians(P["lead_deg"])) + 0.5
    return [(0, zr), (d, zr + d), (d, top), (0, top)]


Z_BUMP = Z_BASE_BOT + 0.8


def arm_frames():
    """For every arm: origin, direction along the wall (u), outward normal (n)."""
    out = []
    e = P["base_clr"]
    for side, s in ARMS:
        if side == "L":
            out.append(((CX1 + e, s), (0, 1), (-1, 0)))
        elif side == "R":
            out.append(((CX2 - e, s), (0, 1), (1, 0)))
        else:
            out.append(((s, CY2 - e), (1, 0), (0, 1)))
    return out


def oriented_box(o, u, n, a1, a2, r1, r2, z1, z2):
    """Box given along-wall range [a1, a2] and outward range [r1, r2] (negative = into the part)."""
    xs = [o[0] + u[0] * a + n[0] * r for a in (a1, a2) for r in (r1, r2)]
    ys = [o[1] + u[1] * a + n[1] * r for a in (a1, a2) for r in (r1, r2)]
    return box(min(xs), min(ys), max(xs), max(ys), z1, z2)


def oriented_prism(o, u, n, profile, a1, a2):
    """Extrude an (r, z) profile along the wall direction."""
    if u == (0, 1):
        pts = [(o[0] + n[0] * r, z) for r, z in profile]
        return prism_y(pts, o[1] + a1, o[1] + a2)
    pts = [(o[1] + n[1] * r, z) for r, z in profile]
    return prism_x(pts, o[0] + a1, o[0] + a2)


# ----------------------------------------------------------------------------- body
def chamfered_opening(x1, x2, z1, z2, y_face, inward, c=0.8, top=True):
    """A wedge that breaks the outer edge of an opening in an end wall (sides and, if wanted, upper edge, 45 deg)."""
    zt = z2 + (c if top else 0.0)
    zc0, h0 = (z1 - 6 + zt) / 2, zt - (z1 - 6)
    zc1, h1 = (z1 - 6 + z2) / 2, z2 - (z1 - 6)
    wp = cq.Workplane("XZ").workplane(offset=-y_face).center((x1 + x2) / 2, zc0).rect(x2 - x1 + 2 * c, h0)
    return wp.workplane(offset=c * inward).center(0, zc1 - zc0).rect(x2 - x1, h1).loft()


def make_body():
    body = rbox(OX1, OY1, OX2, OY2, Z_BOT, Z_TOP, P["r_out"])
    body = body.faces(">Z").edges().chamfer(P["r_top"])
    body = body.faces("<Z").edges().chamfer(0.4)
    body = body.cut(rbox(CX1, CY1, CX2, CY2, Z_BOT - 1, Z_PLATE, R_IN))
    # Under the display the top is only a skin as thick as the glass.  The frame lies against it from below,
    # the glass sits in an opening of its own size: flush with the top, and nothing here hangs over anything.
    c, g = P["frame_clr"], P["glass_clr"]
    body = body.cut(rbox(CX1, FY1 - c, CX2, CY2, Z_PLATE - 1, Z_GLASS, R_IN))
    body = body.cut(rbox(GX1 - g, GY1 - g, GX2 + g, GY2 + g, Z_GLASS - 1, Z_TOP + 1, WS["glass_r"] + g))
    # a small bevel round the opening on the top face: the first layer on the bed squeezes out, the glass must still fit
    bev = 0.3
    flare = rbox(GX1 - g - bev, GY1 - g - bev, GX2 + g + bev, GY2 + g + bev, Z_TOP - bev, Z_TOP + 1, WS["glass_r"] + g + bev)
    body = body.cut(flare.faces("<Z").edges().chamfer(bev - 0.001))
    # the inner edge of the opening is 0.3 wider for the first 0.4 mm: the glass finds its way in
    body = body.cut(rbox(GX1 - g - 0.3, GY1 - g - 0.3, GX2 + g + 0.3, GY2 + g, Z_GLASS - 1, Z_GLASS + 0.4, WS["glass_r"] + g + 0.3))
    # USB-C end: one opening for the port and the two keys, open down to the rim (the base's tongue closes it)
    body = body.cut(box(IO_X1, CY2 - 0.5, IO_X2, OY2 + 1, Z_BOT - 1, Z_IO_TOP))
    body = body.cut(chamfered_opening(IO_X1, IO_X2, Z_BOT, Z_IO_TOP, OY2, 1, top=False))
    # The wall above the opening is bevelled on its inside.  The paddles of the frame's keys grow outwards
    # under 45 degrees just below that bevel: so they print without support and still end in the wall's plane.
    yl = OY2 - P["key_land"]
    body = body.cut(prism_x([(CY2 - 0.01, Z_IO_TOP - 0.01), (yl, Z_IO_TOP - 0.01), (CY2 - 0.01, Z_IO_TOP + yl - CY2)], IO_X1, IO_X2))
    # port end: one slot for all jacks, open down to the rim (the base's sill closes it)
    body = body.cut(box(SLOT_X1, OY1 - 1, SLOT_X2, CY1 + 0.5, Z_BOT - 1, Z_SLOT_TOP))
    body = body.cut(chamfered_opening(SLOT_X1, SLOT_X2, Z_BOT, Z_SLOT_TOP, OY1, -1, 0.7))
    # posts that press on the board where the top plate is thick; no pins - a 2.4 mm pin printed upright breaks
    d, r = P["post_d"], P["post_root"]
    for px, py in BODY_POSTS:
        body = body.union(cyl(px, py, 0.0, Z_PLATE + 0.1, d))
        body = body.union(cq.Workplane("XY").workplane(offset=Z_PLATE + 0.01).center(px, py).circle(d / 2 + r)
                          .workplane(offset=-3.0).circle(d / 2).loft())
    # guides that centre a board with square corners between the walls (only where the gap is wide enough for a rib)
    if P["gap"] >= 0.8:
        t = P["gap"] - 0.25
        for x1, x2 in ((CX1 - 0.01, CX1 + t), (CX2 - t, CX2 + 0.01)):
            body = body.union(box(x1, 9.0, x2, 11.0, Z_CARR_BOT - 1.0, Z_PLATE + 0.1))
        ty = P["gap_rj"] - 0.25
        for x1, x2 in ((CX1 + 3.0, SLOT_X1 - 1.5), (SLOT_X2 + 1.5, CX2 - 3.0)):
            body = body.union(box(x1, CY1 - 0.01, x2, CY1 + ty, Z_CARR_BOT - 1.0, Z_PLATE + 0.1))
    # port numbers, debossed
    for i, xc in enumerate(JACKS):
        t = (cq.Workplane("XY").workplane(offset=Z_TOP - 0.4).center(xc, 9.0)
             .text(str(i + 1), 5.0, 1.0, font="Liberation Sans", halign="center", valign="center"))
        body = body.cut(t)
    # snap pockets in the walls + lead-in chamfer at the bottom edge
    for o, u, n in arm_frames():
        wall_o = (o[0] + n[0] * P["base_clr"], o[1] + n[1] * P["base_clr"])
        a1 = 0.5 - 1.0
        a2 = 0.5 + P["bump_l"] + 1.0
        body = body.cut(oriented_prism(wall_o, u, n, [(-0.01, z) if r == 0 else (r, z) for r, z in recess_profile(Z_BUMP)], a1, a2))
        body = body.cut(oriented_prism(wall_o, u, n, [(-0.01, Z_BOT - 0.01), (0.9, Z_BOT - 0.01), (-0.01, Z_BOT + 0.9)], a1 - 1.5, a2 + 1.5))
    # two pry notches in the bottom edge of the long sides
    for x1, x2 in ((OX1 - 1, CX1 + 0.01), (CX2 - 0.01, OX2 + 1)):
        body = body.cut(box(x1, PRY_Y - 6.0, x2, PRY_Y + 6.0, Z_BOT - 1, Z_BASE_BOT + 1.0))
    return body


# ----------------------------------------------------------------------------- frame
def make_frame():
    """What the module is clamped in.  Printed flat with the seat (its top here) on the bed, so the face the
    glass rests on is the smooth one and the jaws flex in the plane of the layers.
    Returns the fixed part and the two keys (each as leaf and head); together they are one printed part."""
    fr = rbox(FX1, FY1, FX2, FY2, Z_FB, Z_FT, R_IN - P["frame_clr"])
    fr = fr.faces(">Z").edges().chamfer(0.5)            # this edge lies on the bed: the bevel takes the first layer's squeeze
    s = P["stack_clr"]
    # the module's board goes through: a fixed stop on one long side and at both ends, the jaws on the other side
    fr = fr.cut(box(PBX1 - s, PBY1 - P["crush_room"], JAW_X, KEY_STOP_Y, Z_FB - 1, Z_FT + 1))
    # Crush ribs at the antenna end: printed parts come out a little off, and the board must not rattle
    # lengthwise (the keys reach for its switches).  The ribs stand 0.1 into the board's place; its edge
    # shaves them to size when the module is pressed in.  Full height, so they print from the bed up.
    ya = PBY1 - P["crush_room"]
    for xr in (PBX1 + 8.0, PBX2 - 8.0):
        fr = fr.union(cq.Workplane("XY").workplane(offset=Z_FB).polyline([(xr - 0.6, ya - 0.01), (xr + 0.6, ya - 0.01), (xr, ya + P["crush_h"])])
                      .close().extrude(Z_FT - Z_FB))
    # two jaws: cantilevers from a root block in the middle towards both ends, a pad at each tip
    t, sl, L, root, pad = P["jaw_t"], P["jaw_slot"], P["jaw_l"], P["jaw_root"], P["jaw_pad"]
    ym, xo, xp, zc = (PBY1 + PBY2) / 2, JAW_X, PBX2 - P["jaw_bite"], ZM - 0.1
    for sg in (-1, 1):
        tip = ym + sg * (root / 2 + L)
        ya, yb = sorted((ym + sg * root / 2, tip))
        fr = fr.cut(box(xo + t, ya, xo + t + sl, yb, Z_FB - 1, Z_FT + 1))            # slot behind the jaw
        y1, y2 = sorted((tip, tip + sg * 1.0))
        fr = fr.cut(box(xo - 0.01, y1, xo + t + sl, y2, Z_FB - 1, Z_FT + 1))         # its free end
        # the pad stands on the level of the board and the spacer above it, not of the LCD; its upper side is
        # a ramp, so the board pushes the jaw aside when the module is pressed in
        p1, p2 = sorted((tip, tip - sg * pad))
        fr = fr.union(prism_y([(xo + 0.01, Z_FB), (xp, Z_FB), (xp, zc), (xo + 0.01, zc + (xo - xp))], p1, p2))
    # USB-C end.  Between the two keys the frame is open: the receptacle, the touch flex tail and the plug
    # are there.  What stays fixed is a strip at each corner: the board's stop, and the root of a key's leaf.
    t, sl = P["key_leaf_t"], P["key_slot"]
    ks = {name: key_geometry(name) for name in KEYS}
    xf = sorted(k["x_fix"] for k in ks.values())
    fr = fr.cut(box(xf[0], KEY_STOP_Y - 1, xf[1], FY2 + 1, Z_FB - 1, Z_FT + 1))
    keys = {}
    for name, k in ks.items():
        sg = k["sg"]
        xr = k["x_root"]                                     # the slot behind the leaf ends in a half circle: no sharp corner at the root
        x1, x2 = sorted((xr, k["x_fix"]))
        fr = fr.cut(box(x1, FY2 - t - sl, x2, FY2 + 1, Z_FB - 1, Z_FT + 1))
        fr = fr.cut(cyl(xr, FY2 - t - sl / 2, Z_FB - 1, Z_FT + 1, sl))
        # the stop: a bump on the fixed strip that the leaf's end meets when the nose has travelled key_stroke.
        # Its edge on the bed is bevelled, so the first layer cannot weld it to the leaf.
        x1, x2 = sorted((k["x_fix"], k["x_fix"] + sg * 1.6))
        bump = box(x1, FY2 - t - sl - 0.01, x2, FY2 - t - k["stop_gap"], Z_FB, Z_FT)
        fr = fr.union(bump.faces(">Z").edges(">Y").chamfer(0.3))
        keys[name] = make_key(name)
    # two-port case: angles that keep the battery in its tray
    for x1, y1, x2, y2 in HOLDS:
        fr = fr.union(box(x1, y1, x2, y2, Z_TOWER, Z_FB + 0.01))
    # legs that stand on the board: the base presses the board against them and so the frame against the skin
    for x1, y1, x2, y2 in LEGS:
        fr = fr.union(prism_x([(y1, 0.0), (y2, 0.0), (y2, Z_FB + 0.01), (y1 - 4.0, Z_FB + 0.01)], x1, x2))
    d, r = P["post_d"], P["post_root"]
    for px, py in ([] if LEGS else LEG_POSTS):
        fr = fr.union(cyl(px, py, 0.0, Z_FB + 0.01, d))
        fr = fr.union(cq.Workplane("XY").workplane(offset=Z_FB + 0.01).center(px, py).circle(d / 2 + r)
                      .workplane(offset=-3.0).circle(d / 2).loft())
    return fr, keys


# ----------------------------------------------------------------------------- base
def battery_rect():
    """Four-port case: the space the corner stops on the base leave for the cell (x1, y1, x2, y2)."""
    by2 = CY2 - 6.0
    return WS_XC - CELL[0] / 2 - 0.5, by2 - CELL[1] - 1.0, WS_XC + CELL[0] / 2 + 0.5, by2


def cell_box():
    """The battery as it lies in the case: x1, y1, x2, y2, z1, z2."""
    if VARIANT == "hat":
        return CELL_X[0], CELL_Y1, CELL_X[1], CELL_Y1 + CELL[1], CELL_Z[0], CELL_Z[1]
    x1, y1, x2, y2 = battery_rect()
    return x1 + 0.5, y1 + 0.5, x2 - 0.5, y2 - 0.5, Z_BASE_TOP + 0.1, Z_BASE_TOP + 0.1 + CELL[2]


def make_base():
    e = P["base_clr"]
    x1, y1, x2, y2 = CX1 + e, CY1 + e, CX2 - e, CY2 - e
    base = rbox(x1, y1, x2, y2, Z_BASE_BOT, Z_BASE_TOP, R_IN - e)
    arm_top = Z_BASE_BOT + P["arm_h"]
    for o, u, n in arm_frames():
        L, t, s = P["arm_l"], P["arm_t"], P["arm_slot"]
        # clear the arm from the plate (slot behind it and past its free end), keep the root
        base = base.cut(oriented_box(o, u, n, -1.0, L, -(t + s), 0.5, Z_BASE_BOT - 1, arm_top + 1))
        base = base.union(oriented_box(o, u, n, L - 0.01, L + 5.0, -(t + s), 0.0, Z_BASE_BOT, arm_top))        # root block
        base = base.union(oriented_box(o, u, n, 0.0, L, -t, 0.0, Z_BASE_BOT, arm_top))                         # the arm
        base = base.union(oriented_prism(o, u, n, [(-0.01, z) if r == 0 else (r, z) for r, z in bump_profile(Z_BUMP)], 0.5, 0.5 + P["bump_l"]))
    # posts under the board, one below each of its mounting holes; a low dome in the hole centres the board
    d, r = P["post_d"], P["post_root"]
    for px, py in POSTS:
        base = base.union(cyl(px, py, Z_BASE_TOP - 0.01, Z_CARR_BOT, d))
        base = base.union(cq.Workplane("XY").workplane(offset=Z_BASE_TOP - 0.01).center(px, py).circle(d / 2 + r)
                          .workplane(offset=min(3.0, Z_CARR_BOT - Z_BASE_TOP)).circle(d / 2).loft())
        if HOLE_D:
            base = base.union(cyl(px, py, Z_CARR_BOT - 0.01, Z_CARR_BOT + P["dome_h"], HOLE_D - 0.4).faces(">Z").chamfer(0.5))
    c = P["cut_clr"]
    # sill: closes the jack slot below the jacks.  Above the plate it keeps clear of the board's edge.
    y_in = CY1 + min(e, P["gap_rj"] - 0.25)
    base = base.union(box(SLOT_X1 + c, OY1, SLOT_X2 - c, y_in, Z_BASE_BOT, Z_SILL))
    base = base.union(box(SLOT_X1 + c, OY1, SLOT_X2 - c, y1 + 0.5, Z_BASE_BOT, Z_BASE_TOP))
    # battery
    if VARIANT == "hat":
        cx1, cy1, cx2, cy2, cz1, cz2 = cell_box()
        base = base.cut(box(cx1 - 0.3, cy1 - 0.3, cx2 + 0.3, cy2 + 0.3, cz1, Z_BASE_TOP + 1))                  # a shallow tray in the plate
        for xa, xb in ((cx1 - 1.5, cx1 - 0.3), (cx2 + 0.3, cx2 + 1.5)):                                       # low rails at its sides
            base = base.union(box(xa, cy1, xb, cy2, Z_BASE_TOP - 0.01, Z_BASE_TOP + 3.0))
        # stop rib behind the board, with a lip: the cell is tucked under it; the frame's towers keep its other edge down
        zt, yr = cz2 + 0.15, cy1 - 0.3
        base = base.union(box(WS_XC - 14.0, H + 0.5, WS_XC + 14.0, yr, Z_BASE_TOP - 0.01, zt + 1.8))
        base = base.union(prism_x([(yr - 0.01, zt), (yr + 1.0, zt + 1.0), (yr + 1.0, zt + 1.8), (yr - 0.01, zt + 1.8)], WS_XC - 14.0, WS_XC + 14.0))
    else:
        bx1, by1, bx2, by2 = battery_rect()                                                                    # corner stops in the bay under the carrier
        for cx, sx in ((bx1, 1), (bx2, -1)):
            for cy, sy in ((by1, 1), (by2, -1)):
                base = base.union(box(min(cx, cx - sx * 1.4), min(cy, cy + sy * 9.0), max(cx, cx - sx * 1.4), max(cy, cy + sy * 9.0), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
                base = base.union(box(min(cx, cx + sx * 9.0), min(cy, cy - sy * 1.4), max(cx, cx + sx * 9.0), max(cy, cy - sy * 1.4), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
    # tongue: closes the USB-C opening below the port and the keys
    base = base.union(box(IO_X1 + c, CY2 + 0.05, IO_X2 - c, OY2, Z_BASE_BOT, Z_TONGUE))
    base = base.union(box(IO_X1 + c, y2 - 0.5, IO_X2 - c, OY2, Z_BASE_BOT, Z_BASE_TOP))
    # where a fingernail goes in at the pry notches: a 45 deg bevel (a step would start in mid-air on the bed)
    for xe, sx in ((x1, 1), (x2, -1)):
        base = base.cut(prism_y([(xe - sx * 0.01, Z_BASE_BOT - 0.01), (xe + sx * 1.2, Z_BASE_BOT - 0.01), (xe - sx * 0.01, Z_BASE_BOT + 1.2)], PRY_Y - 5.0, PRY_Y + 5.0))
    label = (cq.Workplane("XY").workplane(offset=Z_BASE_BOT + 0.4).center(W / 2, LABEL[1])
             .text("RS232 CONSOLE", LABEL[0], -1.0, font="Liberation Sans", halign="center", valign="center"))
    base = base.cut(label.mirror("YZ", (W / 2, 0, 0)))
    return base


# ----------------------------------------------------------------------------- keys
def key_geometry(name):
    """Numbers of one key.  The head (a block under the glass rim with the paddle on its outside and a nose
    towards the switch) hangs on a leaf that runs along the frame's end from a root near the corner.  Leaf
    and head stand on the bed over their full height; the leaf bends in the plane of the layers.
    Beam: bending length a, then the stiff head; the switch is b beyond the leaf's end."""
    kx, _ = KEYS[name]
    sg = 1 if kx > USB_X else -1                                   # which side of the port
    x_in = USB_X + sg * (P["usb_w"] / 2 + P["key_clr"])            # head's edge next to the plug
    x_out = USB_X + sg * (P["io_w"] / 2 - P["key_clr"])            # head's edge at the side of the opening: the leaf ends here
    x_fix = x_out + sg * P["key_side"]                             # where the fixed strip of the frame's end stops
    x_lim = FX2 - R_FRAME if sg > 0 else FX1 + R_FRAME             # the slot behind the leaf cannot reach nearer to the corner
    a = min(P["key_leaf_l"], abs(x_lim - x_out) - P["key_slot"] / 2)   # bending length: from the round end of the slot to the head
    b = abs(kx - x_out)
    d = a ** 3 / 3 + a * a * b + a * b * b                         # nose travel = force / (E I) * d
    swing = (a * a / 2 + a * b) / d                                # the head turns by this many rad per mm of nose travel
    u = a - P["key_side"]                                          # the spot on the leaf that meets the stop first
    stop_gap = P["key_stroke"] * ((a + b) * u * u / 2 - u ** 3 / 6) / d
    y_f = OY2 - P["key_under"][name]                               # the face a finger touches
    z0 = Z_IO_TOP - 0.5 - P["key_land"] + P["key_under"][name]     # its upper edge: 0.5 below the bevel of the wall above
    return dict(name=name, kx=kx, sg=sg, x_in=x_in, x_out=x_out, x_fix=x_fix, x_root=x_out + sg * a, a=a, b=b, d=d, swing=swing,
                stop_gap=stop_gap, y_f=y_f, z0=z0, y_in=KEY_STOP_Y + P["key_stroke"], z_bot=Z_TONGUE + P["key_clr"],
                pivot=(kx + sg / swing, FY2 - P["key_leaf_t"] / 2))


def make_key(name):
    """One key at rest: (leaf, head)."""
    k = key_geometry(name)
    t = P["key_leaf_t"]
    xa, xb = sorted((k["x_out"], k["x_in"]))
    x1, x2 = sorted((k["x_root"] + k["sg"] * 0.01, k["x_out"] - k["sg"] * 0.01))
    leaf = box(x1, FY2 - t, x2, FY2, Z_FB, Z_FT)
    # block: from the bed (the seat's level) down to the nose.  Its inner face keeps clear of the module's board
    # also when the key is pressed home.
    # The head turns a little when pressed (about a point on the leaf), so its end next to the plug travels
    # farther than the nose: the inner face is slanted by that much and lies parallel to the board's edge at the stop.
    z_nb = Z_NOSE - P["key_nose_h"]
    px = k["pivot"][0]
    yi = lambda x: KEY_STOP_Y + P["key_stroke"] * k["swing"] * abs(x - px)
    head = cq.Workplane("XY").workplane(offset=z_nb).polyline([(xa, yi(xa)), (xb, yi(xb)), (xb, FY2), (xa, FY2)]).close().extrude(Z_FT - z_nb)
    head = head.faces(">Z").edges().chamfer(0.3)
    # paddle: grows out of the block under 45 degrees (below the bevel of the wall above the opening) to the
    # face a finger touches, then straight down to just above the tongue of the base
    y_f, z0, zb = k["y_f"], k["z0"], k["z_bot"]
    head = head.union(prism_x([(FY2 - 0.01, zb), (y_f - 0.4, zb), (y_f, zb + 0.4), (y_f, z0), (FY2 - 0.01, z0 + y_f - FY2 + 0.01)], xa, xb))
    # nose: reaches under the module's board to the switch plunger.  Printed upside down its upper side is an
    # overhang: a ledge of key_ledge at the block, the rest under 45 degrees.
    y_t = KEY_TIP_Y + P["key_gap"]
    c = max(k["y_in"] - y_t - P["key_ledge"], 0.0)
    nw = P["key_nose_w"] / 2
    yb = max(yi(k["kx"] - nw), yi(k["kx"] + nw)) + 0.01
    head = head.union(prism_x([(y_t, z_nb), (y_t, Z_NOSE - c), (y_t + c, Z_NOSE), (yb, Z_NOSE), (yb, z_nb)], k["kx"] - nw, k["kx"] + nw))
    return leaf, head


def key_pressed(name, head, travel):
    """The head of a key when its nose has gone in by 'travel': it turns about a point on the leaf."""
    k = key_geometry(name)
    px, py = k["pivot"]
    return head.rotate((px, py, 0), (px, py, 1), math.degrees(k["sg"] * k["swing"] * travel))


def make_plug():
    """A USB-C plug fully mated: the shell as a tube, the overmould as the largest block the USB-IF allows."""
    U = G.USBC_PLUG
    y0, y1 = USB_BACK_Y, USB_BACK_Y + U["shell_l"]
    def stadium(w, h, ya, yb):
        return cq.Workplane("XZ").workplane(offset=-yb).center(USB_X, USB_Z).slot2D(w, h).extrude(yb - ya)
    shell = stadium(U["shell_w"], U["shell_h"], y0, y1).cut(stadium(U["shell_w"] - 0.6, U["shell_h"] - 0.6, y0 - 1, y1 - 0.5))
    over = (cq.Workplane("XZ").workplane(offset=-(y1 + 24.0)).center(USB_X, USB_Z).rect(U["over_w"], U["over_h"]).extrude(24.0)
            .edges("|Y").fillet(0.4))
    return shell, over


# ----------------------------------------------------------------------------- what goes inside
def make_module():
    """The Waveshare module from the vendor's numbers (envelope model, a handful of solids)."""
    def vbox(b, z):
        (x1, y1), (x2, y2) = ws_dev(b[2], b[3]), ws_dev(b[0], b[1])
        return box(x1, y1, x2, y2, z[0] + ZM, z[1] + ZM)
    parts = {}
    parts["glass"] = rbox(GX1, GY1, GX2, GY2, Z_GLASS, Z_GLASS_TOP, WS["glass_r"])
    parts["tp_sensor"] = vbox(WS["tp_sensor"], WS["tp_sensor_z"])
    parts["lcm"] = vbox(WS["lcm"], WS["lcm_z"])
    parts["pmma"] = vbox(WS["pmma"], WS["pmma_z"])
    parts["pcb"] = vbox(WS["pcb"], WS["pcb_z"])
    hdr = None
    for sx in (-1, 1):
        x = sx * WS["hdr_x"]
        plastic = vbox((x - 1.27, WS["hdr_y1"] - 13 * 2.54 - 1.27, x + 1.27, WS["hdr_y1"] + 1.27), WS["hdr_plastic_z"])
        hdr = plastic if hdr is None else hdr.union(plastic)
        for k in range(14):
            px, py = ws_dev(x, WS["hdr_y1"] - 2.54 * k)
            hdr = hdr.union(box(px - 0.32, py - 0.32, px + 0.32, py + 0.32, WS["hdr_pin_tip_z"] + ZM, WS["pcb_z"][1] + ZM + 1.4))
    parts["headers"] = hdr
    so = None
    for xv, yv in WS["standoffs"]:
        px, py = ws_dev(xv, yv)
        c = cyl(px, py, WS["standoff_z"][0] + ZM, WS["standoff_z"][1] + ZM, WS["standoff_d"]).cut(cyl(px, py, WS["standoff_z"][0] + ZM - 1, WS["standoff_z"][1] + ZM + 1, 1.6))
        so = c if so is None else so.union(c)
    parts["standoffs"] = so
    # USB-C receptacle: shell (a tube), insulator with the tongue, four legs
    def stadium(w, h, ya, yb):
        return cq.Workplane("XZ").workplane(offset=-yb).center(USB_X, USB_Z).slot2D(w, h).extrude(yb - ya)
    y_rear = ws_dev(0, WS["usbc"][3])[1]
    (sw, sh), (ow, oh) = WS["usbc_shell"], WS["usbc_open"]
    tw, tt, ty = WS["usbc_tongue"]
    usb = stadium(sw, sh, y_rear, USB_RIM_Y).cut(stadium(ow, oh, USB_BACK_Y, USB_RIM_Y + 1))
    y_tip = ws_dev(0, ty)[1]
    tongue = box(USB_X - tw / 2, USB_BACK_Y - 0.01, USB_X + tw / 2, y_tip, USB_Z - tt / 2, USB_Z + tt / 2).edges("|Z and >Y").chamfer(0.3)
    usb = usb.union(tongue)
    z_leg = WS["usbc_z"][1] + ZM
    for sx in (-1, 1):
        x_in, x_out = USB_X + sx * (ow / 2), USB_X + sx * WS["usbc"][2]
        usb = usb.union(box(min(x_in, x_out), y_rear, max(x_in, x_out), y_rear + 2.9, USB_Z - 0.3, z_leg))
        x_a, x_b = USB_X + sx * (sw / 2 - 1.0), USB_X + sx * sw / 2          # front tabs, on top of the shell
        usb = usb.union(box(min(x_a, x_b), USB_RIM_Y - 3.6, max(x_a, x_b), USB_RIM_Y - 2.4, USB_Z + sh / 2 - 0.3, z_leg))
    parts["usbc"] = usb
    # RST / BOOT: body and plunger
    aw, ah, ap = WS["button_act"]
    bt = None
    for name, (xv, yv) in WS["buttons"].items():
        kx, ky = ws_dev(xv, yv)
        sw_body = box(kx - WS["button_w"] / 2, ky - ap - WS["button_d"], kx + WS["button_w"] / 2, ky - ap, WS["button_z"][0] + ZM, WS["button_z"][1] + ZM)
        plunger = box(kx - aw / 2, ky - ap - 0.01, kx + aw / 2, ky, KEY_Z - ah / 2, KEY_Z + ah / 2).edges("|Y").fillet(0.2)
        b = sw_body.union(plunger)
        bt = b if bt is None else bt.union(b)
    parts["buttons"] = bt
    parts["bat_header"] = vbox(WS["bat"], WS["bat_z"])
    parts["tf_slot"] = vbox(WS["tf"], WS["tf_z"])
    parts["camera_fpc"] = vbox(WS["cam"], WS["cam_z"])
    return parts


def make_hat():
    """The two-port board with its parts, from its fabrication files (hardware/hat)."""
    pcb = box(0, 0, W, H, Z_CARR_BOT, 0)
    for xk, yk in HB.HOLES:
        pcb = pcb.cut(cyl(*dev(xk, yk), Z_CARR_BOT - 1, 1, HB.HOLE_D))
    top, bottom = None, None
    for _, x1, y1, x2, y2 in HB.JACKS:
        (ax, ay), (bx, by) = dev(x1, y2), dev(x2, y1)
        xc = (ax + bx) / 2
        jack = box(ax, ay, bx, by, 0, JACK_H).cut(box(xc - 5.85, ay - 0.1, xc + 5.85, ay + 14.0, 2.6, 10.8))    # plug opening
        top = jack if top is None else top.union(jack)
    for _, x1, y1, x2, y2, h in HB.boxes():
        (ax, ay), (bx, by) = dev(x1, y2), dev(x2, y1)
        top = top.union(box(ax, ay, bx, by, 0, h))
    for d, length in ((0.76, HB.PIN_BELOW), (3.2, 3.0)):                 # solder tails and board locks under the board
        for xk, yk in (HB.PTH if d < 1 else HB.NPTH).get(d, []):
            pin = cyl(*dev(xk, yk), Z_CARR_BOT - length, Z_CARR_BOT, 0.5 if d < 1 else 3.0)
            bottom = pin if bottom is None else bottom.union(pin)
    return {"pcb": pcb, "parts_top": top, "parts_bottom": bottom}


def make_plugs():
    """Two-port variant: the plugs of the jumper wires on the module's pins, and under each the room its wire needs to turn."""
    plugs, turn = None, None
    hw = HB.PLUG_W / 2
    z2 = WS["hdr_plastic_z"][0] + ZM
    for name, n in HB.MODULE_PINS.items():
        xv = -WS["hdr_x"] if n <= 14 else WS["hdr_x"]
        yv = WS["hdr_y1"] - 2.54 * ((n - 1) if n <= 14 else (28 - n))
        px, py = ws_dev(xv, yv)
        a = box(px - hw, py - hw, px + hw, py + hw, z2 - HB.PLUG_LEN, z2)
        b = cyl(px, py, z2 - HB.PLUG_LEN - HB.WIRE_BEND, z2 - HB.PLUG_LEN, 1.4)
        plugs = a if plugs is None else plugs.union(a)
        turn = b if turn is None else turn.union(b)
    return plugs, turn



def make_carrier():
    """Carrier board with its parts as boxes (fab outline x height), from the board generator."""
    if VARIANT == "hat":
        return make_hat()
    placed, _ = G.build(write=False)
    pcb = rbox(0, 0, W, H, Z_CARR_BOT, 0, G.BOARD_R)
    top, bottom = None, None
    for p, _, _ in placed:
        fp = G.FPS[p.fp]
        if p.fp.startswith("MountingHole"):
            x, y = dev(p.x, p.y)
            pcb = pcb.cut(cyl(x, y, Z_CARR_BOT - 1, 1, fp.pads[0].drill))
            continue
        if fp.height <= 0.1:
            continue
        b = G.part_box(p, "fab")
        (x1, y1), (x2, y2) = dev(b[0], b[3]), dev(b[2], b[1])
        if p.side == "B":
            s = box(x1, y1, x2, y2, Z_CARR_BOT - fp.height, Z_CARR_BOT)
            bottom = s if bottom is None else bottom.union(s)
        else:
            s = box(x1, y1, x2, y2, 0, fp.height)
            top = s if top is None else top.union(s)
    return {"pcb": pcb, "parts_top": top, "parts_bottom": bottom}


def load_vendor():
    shp = cq.importers.importStep(VENDOR_STEP)
    cx, cy = ws_dev(0, 0)
    return shp.rotate((0, 0, 0), (0, 0, 1), 180).translate((cx, cy, ZM))


# ----------------------------------------------------------------------------- checks
def checks(body, frame, fixed, heads, base, module, carrier, vendor):
    ok = True

    def report(name, value, limit=0.01, unit="mm3"):
        nonlocal ok
        good = value <= limit
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {name}: {value:.3f} {unit}")

    def must(name, cond, text):
        nonlocal ok
        ok &= bool(cond)
        print(f"  {'ok  ' if cond else 'FAIL'} {name}: {text}")

    mod_all = None
    for s in module.values():
        mod_all = s if mod_all is None else mod_all.union(s)
    car_all = carrier["pcb"].union(carrier["parts_top"]).union(carrier["parts_bottom"])
    cx1, cy1, cx2, cy2, cz1, cz2 = cell_box()
    cell = box(cx1, cy1, cx2, cy2, cz1, cz2)
    # the two jaws stand into the board's place by their bite; that overlap and no other is allowed
    bite = 2 * P["jaw_pad"] * P["jaw_bite"] * (ZM - 0.1 - Z_FB) * 1.1 + 0.3      # and what the two crush ribs give
    n_arms = len(ARMS)
    preload_volume = n_arms * P["bump_l"] * P["preload"] * P["bump"] * 1.3
    print("assembled:")
    report("body x module", vol(body.intersect(mod_all)))
    report("body x frame", vol(body.intersect(frame)))
    report("body x board", vol(body.intersect(car_all)))
    report("frame x module (only the bite of the two jaws)", vol(frame.intersect(mod_all)), bite)
    report("frame x board", vol(frame.intersect(car_all)))
    report("frame x base", vol(frame.intersect(base)))
    report("base x board", vol(base.intersect(car_all)))
    report(f"base x body (only the preload of the {n_arms} catches)", vol(base.intersect(body)), preload_volume)
    report("base x module", vol(base.intersect(mod_all)))
    for name, part in (("base", base), ("frame", frame), ("board", car_all), ("module", mod_all), ("body", body)):
        report(f"battery x {name}", vol(cell.intersect(part)))
    if VARIANT == "hat":
        report("module x board (the module hangs free above it)", vol(mod_all.intersect(car_all)))
        pins = module["headers"].val().BoundingBox().zmin
        print(f"  pin tips {pins:.2f} mm above the board and {pins - cz2:.2f} mm above the battery; tallest part under the module {max(b[5] for b in HB.boxes()):.2f} mm")
        plugs, turn = make_plugs()
        for name, part in (("board", car_all), ("frame", frame), ("base", base), ("body", body), ("battery", cell)):
            report(f"plugs of the jumper wires x {name}", vol(plugs.intersect(part)))
        for name, part in (("board", car_all), ("frame", frame), ("base", base), ("body", body)):
            report(f"room for the wires to turn x {name}", vol(turn.intersect(part)))
        over = vol(turn.intersect(cell))
        print(f"  info three wires turn above the battery's edge: {plugs.val().BoundingBox().zmin - cz2:.1f} mm between plug and cell"
              + (f" (the {HB.WIRE_BEND:.0f} mm turn would reach {over:.1f} mm3 into it: these wires bend sideways)" if over > 0.01 else ""))
    else:
        report("module x carrier (pins in sockets excluded)", vol(mod_all.cut(module["headers"]).intersect(car_all)))
    if vendor is not None:
        report("body x module (vendor STEP)", vol(body.intersect(vendor)))
        report("frame x module (vendor STEP, only the bite of the jaws)", vol(frame.intersect(vendor)), bite)
        report("base x module (vendor STEP)", vol(base.intersect(vendor)))
        bb = vendor.val().BoundingBox()
        e = mod_all.val().BoundingBox()
        d = max(abs(bb.xmin - e.xmin), abs(bb.xmax - e.xmax), abs(bb.ymin - e.ymin), abs(bb.ymax - e.ymax), abs(bb.zmin - e.zmin), abs(bb.zmax - e.zmax))
        report("envelope model vs vendor STEP, largest bounding-box difference", d, 0.02, "mm")
    print("what holds what (each pair must touch: moved 0.1 mm towards each other they overlap):")
    lift = (0, 0, 0.1)
    drop = (0, 0, -0.1)
    must("glass on the frame's seat", vol(module["glass"].translate(drop).intersect(frame)) > 1.0, "rests all round")
    must("frame under the top skin", vol(frame.translate(lift).intersect(body)) > 1.0, "lies against it")
    must("frame's legs on the board", vol(frame.translate(drop).intersect(carrier["pcb"])) > 0.3, "stand on it")
    must("body's posts on the board", vol(body.translate(drop).intersect(carrier["pcb"])) > 0.3, "press on it")
    must("base's posts under the board", vol(base.translate(lift).intersect(carrier["pcb"])) > 0.5, "carry it")
    stops = vol(frame.intersect(module["pcb"].translate((0, P["stop_clr"] + 0.05, 0)))), vol(frame.intersect(module["pcb"]))
    must("module's board between the stop at the USB-C end and the crush ribs", stops[0] > 0.2 and stops[1] > 0.01,
         f"{P['stop_clr']:.2f} mm to the stop, ribs {P['crush_h'] - P['crush_room']:.2f} mm into the board's place")
    side = vol(frame.intersect(module["pcb"].translate((-P["stack_clr"] - 0.05, 0, 0))))
    must("module's board against the fixed side", side > 0.5, "the jaws press it there")
    if VARIANT == "hat":
        must("battery under the frame's hold-downs", vol(frame.intersect(cell.translate((0, 0, P["hold_gap"] + 0.1)))) > 0.5 and vol(frame.intersect(cell)) < 0.01,
             f"{Z_TOWER - cz2:.2f} mm above it")
        must("battery under the lip of the stop rib", vol(base.intersect(cell.translate((0, 0, 0.8)))) > 0.5, "it cannot lift more than 0.5 mm there")
    print("assembly paths:")
    worst = max(vol(frame.intersect(mod_all.translate((0, 0, k)))) for k in (0.3, 0.6, 1, 1.5, 2, 3, 4, 6, 8, 12, 16))
    report("module pressed into the frame from the glass side (only the jaws give way)", worst, bite)
    inner = frame.union(mod_all)
    worst = max(vol(body.intersect(inner.translate((0, 0, -k)))) for k in (0.3, 0.6, 1, 2, 4, 8, 12, 16, 22, 30))
    report("frame with module into the body from below (the paddles slide up the opening)", worst)
    worst = max(vol(body.union(frame).intersect(car_all.translate((0, 0, -k)))) for k in (0.5, 1, 2, 4, 8, 12, 16, 22, 30))
    report("board onto the posts and legs", worst)
    # the base on its way in: only the catches may touch the body, and by no more than the arms can give
    ks = (0.2, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 10.0, 16.0, 24.0)
    vals = [(k, vol(body.intersect(base.translate((0, 0, -k))))) for k in ks]
    worst = max(v for _, v in vals)
    catch_volume = n_arms * P["bump_l"] * (P["bump"] - P["base_clr"]) ** 2 * 3
    print("  " + ", ".join(f"{k:.1f} mm: {v:.2f}" for k, v in vals[:9]))
    report("base pushed in: overlap with the body stays within the catches", worst, catch_volume)
    rest = frame.union(mod_all).union(car_all)
    loaded = base.union(cell)
    worst = max(vol(rest.intersect(loaded.translate((0, 0, -k)))) for k in ks)
    report("base with the battery on it x frame, module, board on the way in", worst)
    print("keys (RST, BOOT): a paddle on a leaf spring each, part of the frame")
    mat, e_mod, strain_max = MATERIAL
    t, hl = P["key_leaf_t"], P["frame_t"]
    s_act, s_stop = P["key_gap"] + P["key_travel"], P["key_stroke"]
    bare = mod_all.cut(module["buttons"])
    for name, head in heads.items():
        k = key_geometry(name)
        for other, part in (("fixed part of the frame", fixed), ("body", body), ("base", base), ("module", mod_all)):
            report(f"{name} at rest x {other}", vol(head.intersect(part)))
        for sv in (s_act, s_stop):
            pr = key_pressed(name, head, sv)
            worst = max(vol(pr.intersect(part)) for part in (fixed, body, base, bare))
            report(f"{name} pressed {sv:.2f} mm x frame, body, base, module without its switches", worst)
        hit = vol(key_pressed(name, head, P["key_gap"] + 0.06).intersect(module["buttons"]))
        must(f"{name}: the nose meets the switch plunger after {P['key_gap']:.2f} mm", hit > 0.005, f"overlap {hit:.3f} mm3 at {P['key_gap'] + 0.06:.2f} mm")
        if vendor is not None:
            report(f"{name} at rest x module (vendor STEP)", vol(head.intersect(vendor)))
            hitv = vol(key_pressed(name, head, P["key_gap"] + 0.06).intersect(vendor))
            must(f"{name}: the nose meets the plunger of the vendor model", hitv > 0.005, f"overlap {hitv:.3f} mm3")
        a, b, d = k["a"], k["b"], k["d"]
        rate = e_mod * hl * t ** 3 / 12 / d                         # N per mm of nose travel
        eps = lambda sv: t * (a + b) * sv / (2 * d)
        print(f"  {name}: leaf {a:.1f} x {t:.2f} x {hl:.1f}, switch {b:.1f} mm beyond its end; {rate:.2f} N/mm at the nose")
        print(f"  {name}: click after {s_act:.2f} mm: {rate * s_act:.1f} N for the leaf (plus the switch), strain {eps(s_act) * 100:.2f} %; "
              f"at the stop ({s_stop:.2f} mm, {k['stop_gap']:.2f} mm gap at the leaf) {eps(s_stop) * 100:.2f} % (limit {strain_max * 100:.1f} %)")
        print(f"  {name}: paddle {abs(k['x_in'] - k['x_out']):.1f} x {k['z0'] - k['z_bot']:.1f} mm, {P['key_under'][name]:.1f} below the outer face, turns {math.degrees(k['swing'] * s_act):.1f} deg; "
              f"a nose printed 0.3 mm too far presses the plunger with {rate * 0.3:.2f} N (a switch clicks at about 1.6 N)")
        ok &= eps(s_stop) < strain_max and eps(s_act) < 0.6 * strain_max
    print("USB-C plug (shell 8.25 x 2.40 x 6.65, overmould 12.35 x 6.5, the USB-IF maximum):")
    shell, over = make_plug()
    plug = shell.union(over)
    for name, part in (("body", body), ("frame", frame), ("base", base), ("module", mod_all)):
        report(f"plug fully mated x {name}", vol(part.intersect(plug)))
    both = None
    for name, head in heads.items():
        pr = key_pressed(name, head, P["key_stroke"])
        both = pr if both is None else both.union(pr)
    report("plug fully mated x both keys pressed home", vol(both.intersect(plug)))
    worst = max(vol(body.union(frame).union(base).intersect(plug.translate((0, k, 0)))) for k in (0.5, 1, 2, 3, 4, 5, 7))
    report("plug on its way in x body, frame, base", worst)
    if vendor is not None:
        print(f"  info plug shell x vendor receptacle (contact springs and latches reach into the plug): {vol(shell.intersect(vendor)):.3f} mm3")
        report("plug overmould x module (vendor STEP)", vol(over.intersect(vendor)))
    U = G.USBC_PLUG
    over_y = USB_BACK_Y + U["shell_l"]
    print(f"  receptacle rim {OY2 - USB_RIM_Y:.2f} mm behind the outer face; mated, the overmould ends {OY2 - over_y:.2f} mm below the outer face")
    print(f"  room around the largest overmould: {(P['usb_w'] - U['over_w']) / 2 + P['key_clr']:.2f} mm to each paddle, "
          f"{(P['usb_h'] - U['over_h']) / 2 + P['key_clr']:.2f} mm above, {(P['usb_h'] - U['over_h']) / 2:.2f} mm below")
    print("glass seat:")
    under = box(GX1, GY1, GX2, GY2, Z_GLASS - 0.05, Z_GLASS)
    seat = vol(frame.intersect(under)) / 0.05
    print(f"  seat area under the glass: {seat:.0f} mm2 of {37.1 * 58.8 - (4 - math.pi) * 2.6 ** 2:.0f} mm2 glass")
    ok &= seat > 300
    print(f"snap arms of the base ({mat}, E ~ {e_mod:.0f} MPa):")
    y = P["bump"] - P["base_clr"]
    L, t, b = P["arm_l"] - 0.5 - P["bump_l"] / 2, P["arm_t"], P["arm_h"]
    strain = 1.5 * t * y / L ** 2
    force = e_mod * b * t ** 3 * y / (4 * L ** 3)
    mu = 0.3
    fin = force * (mu + math.tan(math.radians(P["lead_deg"]))) / (1 - mu * math.tan(math.radians(P["lead_deg"])))
    fout = force * (mu + 1.0) / (1 - mu)
    print(f"  deflection {y:.2f} mm, strain {strain * 100:.2f} % (limit {strain_max * 100:.1f} %), force per arm {force:.1f} N")
    print(f"  push-in about {n_arms * fin:.0f} N, pull-out about {n_arms * fout:.0f} N for {n_arms} arms")
    print(f"  held bent by the preload: {1.5 * t * P['preload'] / L ** 2 * 100:.2f} % strain")
    ok &= strain < strain_max
    print("jaws of the frame:")
    yj, Lj, tj, bj = P["jaw_bite"], P["jaw_l"] - P["jaw_pad"] / 2, P["jaw_t"], P["frame_t"]
    sj = 1.5 * tj * yj / Lj ** 2
    fj = e_mod * bj * tj ** 3 * yj / (4 * Lj ** 3)
    print(f"  each bent {yj:.2f} mm: {fj:.1f} N, strain {sj * 100:.2f} % (limit {strain_max * 100:.1f} %); a bite between 0.2 and 0.6 mm gives {fj * 0.2 / yj:.1f} to {fj * 0.6 / yj:.1f} N")
    print(f"  together {2 * fj:.0f} N against the fixed side; with friction 0.3 to 0.5 it takes {2 * 2 * fj * 0.3:.0f} to {2 * 2 * fj * 0.5:.0f} N to push the module out")
    ok &= sj < strain_max
    return ok


# ----------------------------------------------------------------------------- output
def main():
    os.makedirs(OUT, exist_ok=True)
    body, base = make_body(), make_base()
    fixed, key_parts = make_frame()
    frame, keys = fixed, None                    # the frame as it is printed: fixed part and both keys in one piece
    for leaf, head in key_parts.values():
        frame = frame.union(leaf).union(head)
        keys = leaf.union(head) if keys is None else keys.union(leaf).union(head)
    heads = {name: head for name, (_, head) in key_parts.items()}
    module, carrier = make_module(), make_carrier()
    for name, part in (("body ", body), ("frame", frame), ("base ", base)):
        b = part.val().BoundingBox()
        print(f"{name} {b.xlen:.1f} x {b.ylen:.1f} x {b.zlen:.1f} mm, {vol(part) / 1000:.1f} cm3")
    bb = body.val().BoundingBox()
    vendor = None
    if "--no-vendor" not in sys.argv and os.path.exists(VENDOR_STEP):
        vendor = load_vendor()
    ok = checks(body, frame, fixed, heads, base, module, carrier, vendor)

    tag = "rs232_2port" if VARIANT == "hat" else "rs232_4port"

    def step(shape, fname):
        """A single part as STEP, with a part name of its own: CAD systems that keep one part number per
        session (CATIA) refuse two files that both carry the exporter's default name."""
        path = os.path.join(OUT, fname)
        cq.exporters.export(shape, path)
        name = f"{tag}_{os.path.splitext(fname)[0]}"
        text = open(path).read()
        text, n = re.subn(r"PRODUCT\('Open CASCADE STEP translator [^']*',\s*'Open CASCADE STEP translator [^']*'", f"PRODUCT('{name}','{name}'", text)
        assert n == 1, (fname, n)
        open(path, "w").write(text)
    for name, part in (("body", body), ("frame", frame), ("base", base)):
        step(part, f"{name}.step")
        cq.exporters.export(part, os.path.join(OUT, f"{name}.stl"), tolerance=0.02, angularTolerance=0.15)
    # print-ready files: every part as it goes on the bed
    pd = os.path.join(OUT, "print")
    os.makedirs(pd, exist_ok=True)
    for old in ("cradle.step", "cradle.stl", os.path.join("print", "cradle_print.stl"), "_rest.stl", "_rest.inc",
                "key.step", "key.stl", os.path.join("print", "key_print_x2.stl"), "p_key_print_x2.inc"):
        if os.path.exists(os.path.join(OUT, old)):
            os.remove(os.path.join(OUT, old))

    def flipped(shape, z_face):
        """Turned over, the face at z_face on the bed."""
        return shape.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, 0, z_face))
    cq.exporters.export(flipped(body, Z_TOP), os.path.join(pd, "body_print.stl"), tolerance=0.02, angularTolerance=0.15)
    cq.exporters.export(flipped(frame, Z_FT), os.path.join(pd, "frame_print.stl"), tolerance=0.02, angularTolerance=0.15)
    cq.exporters.export(base.translate((0, 0, -Z_BASE_BOT)), os.path.join(pd, "base_print.stl"), tolerance=0.02, angularTolerance=0.15)
    colours = {"glass": (0.04, 0.04, 0.05), "tp_sensor": (0.2, 0.2, 0.22), "lcm": (0.55, 0.55, 0.57), "pmma": (0.85, 0.9, 0.95),
               "pcb": (0.05, 0.15, 0.45), "headers": (0.85, 0.75, 0.2), "standoffs": (0.8, 0.65, 0.3), "usbc": (0.75, 0.75, 0.78),
               "buttons": (0.9, 0.9, 0.9), "bat_header": (0.95, 0.95, 0.9), "tf_slot": (0.7, 0.72, 0.75), "camera_fpc": (0.2, 0.2, 0.2)}
    mod = cq.Assembly(name="waveshare_esp32_s3_touch_lcd_2")
    for name, s in module.items():
        mod.add(s, name=name, color=cq.Color(*colours[name]))
    mod.save(os.path.join(OUT, "waveshare_module_envelope.step"))
    car = cq.Assembly(name="rs232_console_hat" if VARIANT == "hat" else "rs232_carrier")
    car.add(carrier["pcb"], name="pcb", color=cq.Color(0.1, 0.4, 0.2))
    car.add(carrier["parts_top"], name="parts_top", color=cq.Color(0.15, 0.15, 0.15))
    car.add(carrier["parts_bottom"], name="parts_bottom", color=cq.Color(0.15, 0.15, 0.15))
    car.save(os.path.join(OUT, "carrier_envelope.step"))
    cx1, cy1, cx2, cy2, cz1, cz2 = cell_box()
    cell = box(cx1, cy1, cx2, cy2, cz1, cz2)

    def assembly(name, the_module, module_name):
        asm = cq.Assembly(name=name)
        asm.add(body, name="body", color=cq.Color(0.9, 0.9, 0.88))
        asm.add(frame, name="frame", color=cq.Color(0.35, 0.36, 0.38))
        asm.add(base, name="base", color=cq.Color(0.6, 0.6, 0.6))
        asm.add(cell, name="battery", color=cq.Color(0.25, 0.3, 0.5))
        asm.add(the_module, name=module_name)
        asm.add(car, name="carrier")
        return asm
    assembly("rs232_console", mod, "module").save(os.path.join(OUT, "rs232_console_assembly.step"))
    if vendor is not None:
        # the same assembly with the vendor's own model of the module (every part of it), for CAD work
        assembly("rs232_console_vendor_module", vendor, "waveshare_esp32_s3_touch_lcd_2_vendor").save(os.path.join(OUT, "rs232_console_assembly_vendor_module.step"))
    # meshes for the renderer
    shell, over = make_plug()
    extra = [("keys", keys), ("plug_shell", shell), ("plug_over", over), ("frame", fixed), ("cell", cell)] + ([("plugs", make_plugs()[0])] if VARIANT == "hat" else [])
    for name, s in [("pcb_ws" if k == "pcb" else k, v) for k, v in module.items()] + list(carrier.items()) + extra:
        cq.exporters.export(s, os.path.join(OUT, f"_{name}.stl"), tolerance=0.02 if name in ("usbc", "buttons", "keys") else 0.03, angularTolerance=0.2)
    import json
    va = ws_dev(WS["glass"][0] + 3.06 + 30.99 / 2, WS["glass"][3] - 7.51 - 41.20 / 2)     # centre of the lit view area
    hat = None
    if VARIANT == "hat":
        hat = {"W": W, "H": H, "holes": [list(dev(*h)) for h in HB.HOLES], "hole_d": HB.HOLE_D,
               "jacks": [[dev(j[1], j[4])[0], dev(j[1], j[4])[1], dev(j[3], j[2])[0], dev(j[3], j[2])[1]] for j in HB.JACKS],
               "parts": [[b[0], dev(b[1], b[4])[0], dev(b[1], b[4])[1], dev(b[3], b[2])[0], dev(b[3], b[2])[1], b[5]] for b in HB.boxes()],
               "j3": [list(dev(*q)) for q in HB.J3], "pin_below": HB.PIN_BELOW, "plug": [HB.PLUG_LEN, HB.PLUG_W, HB.WIRE_BEND],
               "plugs": {name: list(ws_dev(-WS["hdr_x"] if n <= 14 else WS["hdr_x"], WS["hdr_y1"] - 2.54 * ((n - 1) if n <= 14 else (28 - n)))) for name, n in HB.MODULE_PINS.items()},
               "hdr_plastic_z": WS["hdr_plastic_z"][0] + ZM, "pin_tip_z": WS["hdr_pin_tip_z"] + ZM,
               "hdr_x": [ws_dev(WS["hdr_x"], 0)[0], ws_dev(-WS["hdr_x"], 0)[0]],
               "hdr_y": [ws_dev(0, WS["hdr_y1"])[1], ws_dev(0, WS["hdr_y1"] - 13 * 2.54)[1]],
               "rib": [WS_XC - 14.0, H + 0.5, WS_XC + 14.0, cy1 - 0.3, cz2 + 0.15 + 1.8], "z_tower": Z_TOWER}
    json.dump({"variant": VARIANT, "hat": hat, "W": W, "H": H, "jacks": list(JACKS), "jack_w": JACK_W, "jack_h": JACK_H, "jack_front": JACK_FRONT, "ws_xc": WS_XC,
               "cell": list(CELL), "cell_box": [cx1, cy1, cx2, cy2, cz1, cz2],
               "oy": [OY1, OY2], "ox": [OX1, OX2], "cx": [CX1, CX2], "cy": [CY1, CY2], "posts": [list(p) for p in POSTS], "body_posts": [list(p) for p in BODY_POSTS],
               "legs": [list(leg) for leg in LEGS] or [[p[0] - 2.5, p[1] - 2.5, p[0] + 2.5, p[1] + 2.5] for p in LEG_POSTS],
               "z_top": Z_TOP, "z_glass_top": Z_GLASS_TOP, "z_base_top": Z_BASE_TOP, "cy2": CY2, "view_area_centre": va, "outer": [bb.xlen, bb.ylen, bb.zlen],
               "P": P, "z": {"bot": Z_BOT, "base_bot": Z_BASE_BOT, "base_top": Z_BASE_TOP, "bay": Z_BAY, "plate": Z_PLATE, "glass": Z_GLASS,
                             "top": Z_TOP, "bump": Z_BUMP, "frame_bot": Z_FB, "tower": Z_TOWER, "nose": Z_NOSE, "io_top": Z_IO_TOP, "tongue": Z_TONGUE,
                             "slot_top": Z_SLOT_TOP, "sill": Z_SILL, "carr_bot": Z_CARR_BOT},
               "bump_profile": bump_profile(Z_BUMP), "recess_profile": recess_profile(Z_BUMP),
               "glass": [GX1, GY1, GX2, GY2], "module_pcb": [PBX1, PBY1, PBX2, PBY2], "frame": [FX1, FY1, FX2, FY2], "jaw_x": JAW_X,
               "jaw_y": [(PBY1 + PBY2) / 2 - P["jaw_root"] / 2 - P["jaw_l"], (PBY1 + PBY2) / 2 + P["jaw_root"] / 2 + P["jaw_l"]],
               "io_x": [IO_X1, IO_X2], "slot_x": [SLOT_X1, SLOT_X2], "holds": [list(h) for h in HOLDS], "zm": ZM,
               "usb": {"x": USB_X, "z": USB_Z, "rim_y": USB_RIM_Y, "back_y": USB_BACK_Y, "oy2": OY2, "cy2": CY2, "fy2": FY2,
                       "plug": G.USBC_PLUG, "shell": WS["usbc_shell"], "open": WS["usbc_open"], "tongue": WS["usbc_tongue"][:2],
                       "tongue_tip_y": ws_dev(0, WS["usbc_tongue"][2])[1], "rear_y": ws_dev(0, WS["usbc"][3])[1]},
               "key": {"z": KEY_Z, "x": {n: v[0] for n, v in KEYS.items()}, "tip_y": KEY_TIP_Y, "stop_y": KEY_STOP_Y,
                       "act": WS["button_act"], "body_d": WS["button_d"], "body_z": [WS["button_z"][0] + ZM, WS["button_z"][1] + ZM],
                       "pcb_edge_y": PBY2, "pcb_z": [WS["pcb_z"][0] + ZM, WS["pcb_z"][1] + ZM], "glass_y": GY2,
                       "geo": {n: {kk: vv for kk, vv in key_geometry(n).items()} for n in KEYS}},
               "ws": {k: v for k, v in WS.items() if k.endswith("_z") or k in ("pcb", "lcm", "pmma", "tp_sensor", "glass")}},
              open(os.path.join(OUT, "geometry.json"), "w"), indent=1)
    with open(os.path.join(OUT, "dimensions.txt"), "w") as fh:
        fh.write(f"outer {bb.xlen:.2f} x {bb.ylen:.2f} x {bb.zlen:.2f} mm\n"
                 f"z: bottom {Z_BOT:.2f}, base {Z_BASE_BOT:.2f}..{Z_BASE_TOP:.2f}, board {Z_CARR_BOT:.2f}..0, frame {Z_FB:.2f}..{Z_FT:.2f}, top plate from {Z_PLATE:.2f}, skin from {Z_GLASS:.2f}, top {Z_TOP:.2f}\n"
                 f"glass opening x {GX1 - P['glass_clr']:.2f}..{GX2 + P['glass_clr']:.2f}, y {GY1 - P['glass_clr']:.2f}..{GY2 + P['glass_clr']:.2f}, skin {Z_TOP - Z_GLASS:.2f} thick\n"
                 f"frame x {FX1:.2f}..{FX2:.2f}, y {FY1:.2f}..{FY2:.2f}; module board x {PBX1:.2f}..{PBX2:.2f}, y {PBY1:.2f}..{PBY2:.2f}; jaws at x {JAW_X:.2f}, pads {P['jaw_bite']:.2f} into the board's place\n"
                 f"USB-C: axis x {USB_X:.2f} z {USB_Z:.2f}, receptacle rim y {USB_RIM_Y:.2f}, plug stop y {USB_BACK_Y:.2f}, passage {P['usb_w']:.1f} x {P['usb_h']:.1f}, outer face y {OY2:.2f}\n"
                 f"opening in the USB-C end x {IO_X1:.2f}..{IO_X2:.2f} up to z {Z_IO_TOP:.2f}, tongue of the base up to z {Z_TONGUE:.2f}\n"
                 f"jack slot x {SLOT_X1:.2f}..{SLOT_X2:.2f} up to z {Z_SLOT_TOP:.2f}, sill of the base up to z {Z_SILL:.2f}\n"
                 f"keys (part of the frame): plunger axis z {KEY_Z:.2f}, x {', '.join(f'{n} {v[0]:.2f}' for n, v in KEYS.items())}; "
                 + "; ".join(f"{n}: paddle x {min(g['x_in'], g['x_out']):.2f}..{max(g['x_in'], g['x_out']):.2f}, z {g['z_bot']:.2f}..{g['z0']:.2f}, face y {g['y_f']:.2f}, leaf {g['a']:.1f} x {P['key_leaf_t']:.2f}"
                             for n, g in ((n, key_geometry(n)) for n in KEYS)) + f"; nose {P['key_gap']:.1f} before the plunger, stop after {P['key_stroke']:.1f}\n"
                 f"battery {CELL[0]:.1f} x {CELL[1]:.1f} x {CELL[2]:.1f}: x {cx1:.2f}..{cx2:.2f}, y {cy1:.2f}..{cy2:.2f}, z {cz1:.2f}..{cz2:.2f}\n")
    print("wrote", OUT, "- all checks passed" if ok else "- CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
