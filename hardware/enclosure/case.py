#!/usr/bin/env python3
"""Case for the RS232 console, revision 0.4 - real solid CAD (CadQuery / OpenCascade).

    <venv>/bin/python case.py            # STEP + STL of every part, assembly STEP, checks
    <venv>/bin/python case.py --no-vendor   # skip the checks against the vendor STEP
    <venv>/bin/python case.py --hat      # the same case around the two-port prototype board (hardware/hat), into out_hat/

Needs CadQuery (pip install cadquery).  All numbers of the Waveshare module come from the vendor's
own 3D model (carrier/generate.py: WS), the board outline and the part positions from the same
generator, so the case always fits the board that was generated.

Device coordinate system (right handed):
    X = KiCad x of the carrier, Y = BOARD_H - KiCad y (RJ45 edge at Y = 0, USB-C end at Y ~ 110),
    Z = up, Z = 0 on the top face of the carrier as it sits in the case.

Three printed parts (four pieces):
  body  the whole shell.  The display glass lies in a pocket of the top face on a ledge, flush with
        the top - the module is dropped in from above and the touch force goes into the case.
  base  a plate inside the walls at the bottom with five snap arms that flex in the plane of the
        plate (printed flat: the layers run along the arms, not across them).
  key   two of them: sliding keys that reach the RST and BOOT switches of the module.  A key runs in
        a tunnel under the top plate, its cap shows in the recessed bay around the USB-C port.
Assembly: both keys into their tunnels from inside, module into the body from above, body face
down, carrier pressed onto the pins, battery plugged, base pushed in until it clicks.  No screws.
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
    wall_rj=1.6,         # port end: thin, so the jacks sit 1.9 mm behind the outer face
    gap=0.5,             # carrier to side walls
    gap_rj=0.3,          # carrier edge to the port wall
    r_out=6.0,           # outer vertical corner radius
    r_top=1.2,           # 45 deg chamfer of the top edge (the top lies on the bed: a chamfer prints clean, a radius does not)
    plug_gap=0.3,        # the module sits this much above "fully plugged": the glass always seats on its ledge first
    top_t=3.0,           # top plate
    glass_clr=0.25,      # glass to pocket wall (lens tolerance +-0.05, FDM)
    glass_recess=0.1,    # the glass lies this far below the top face: the pocket is then 1.2 deep, six layers of 0.2 mm
    open_clr=0.30,       # module PCB to the drop-in opening
    bot_clr=2.6,         # under the carrier: through-hole pins 2 mm, relays 1.75 mm
    bat_t=7.0,           # battery bay height
    base_t=2.0, base_inset=0.6, base_clr=0.25,
    post_d=5.0,
    # snap arms (in-plane cantilevers on the base)
    arm_l=18.0, arm_t=1.6, arm_h=5.0, arm_slot=1.6,     # long and slim for PLA: 0.65 % strain when snapping in
    bump=0.9, bump_l=4.0, bump_land=0.6, lead_deg=30.0, preload=0.2, recess_d=1.0,
    # USB-C end: a shallow bay with 45 deg walls holds the port and the two keys below the outer face
    bay_depth=1.0, bay_w=26.0, bay_h=9.6, bay_r=3.0,
    usb_w=13.4, usb_h=7.5, usb_r=2.0,    # well for the plug's overmould (USB-IF maximum 12.35 x 6.5)
    key_w=3.0, key_h=6.0,                # the visible cap
    key_proud=0.8,                       # cap above the bay floor (0.2 below the outer face)
    key_clr=0.2,                         # sliding fit in hole and tunnel
    key_gap=0.15,                        # nose to switch plunger at rest
    key_shoulder=1.0,                    # the key's body is taller than its cap: it cannot leave outwards
    key_nose_h=1.9, key_stroke=0.6,
    rj_clr=0.3, foot_d=10.4, foot_depth=0.6,
    rest_gap=0.15,       # play between the module's standoffs and what they rest on (two-port variant)
)
# Printing material: name, modulus (MPa), strain a snap arm may see when it is pushed in.  PLA is stiff and
# brittle; PETG would be ("PETG", 2000.0, 0.02).
MATERIAL = ("PLA", 3300.0, 0.010)

if VARIANT == "hat":
    # Two-port prototype board: 50 x 50 mm with square corners, held by its four mounting holes.  It lies at
    # the bottom.  The module is wired to it with jumper wires whose plugs hang 14 mm down from its pins, so
    # the module sits high above the board and rests on posts and on a cradle under its own M2 standoffs.
    # The battery lies in between, in a pocket of that cradle, between the two pin rows.
    P.update(gap=1.0, gap_rj=1.0, r_out=5.0,      # room for the board's square corners inside the rounded cavity
             bot_clr=HB.PIN_BELOW + 0.5,          # the board's solder tails just clear the base
             bat_t=0.0)                           # no battery bay under the board
    W, H, BOARD_R = HB.W, HB.H, 0.0
    WS_XC, WS_YC = HB.WS_XC, HB.ws_yc(WS)

    def ws_xy(xv, yv):
        return WS_XC - xv, WS_YC + yv
    # vendor z -> device z: header body, plug, room for the wire to turn, and 1 mm above the bare board
    MODULE_Z = -WS["hdr_plastic_z"][0] + HB.PLUG_LEN + HB.WIRE_BEND + 1.0
    JACKS, JACK_W, JACK_H = HB.JACK_X, HB.JACK_W, HB.JACK_H
    JACK_FRONT = H - HB.JACK_FRONT                # the jack fronts sit this far inside the board edge
    POST_XY, POST_PIN, POST_ROOT = HB.HOLES, HB.HOLE_D - 0.3, 1.4
    ARM_STARTS = [("L", 12.0), ("L", 52.0), ("R", 12.0), ("R", 52.0), ("U", W / 2 - 11.0)]
    PRY_Y = 41.0
    BATTERY = None                                # not on the base: in the cradle
    STANDOFF_POSTS = sorted(WS["standoffs"], key=lambda s: s[1])[:2]     # the two at the USB-C end; the other two rest on the board
    LABEL = (3.4, 43.0)
else:
    W, H, BOARD_R = G.BOARD_W, G.BOARD_H, G.BOARD_R
    WS_XC, ws_xy = G.WS_XC, G.ws_xy
    MODULE_Z = G.WS_Z0 + P["plug_gap"]
    JACKS, JACK_W, JACK_H, JACK_FRONT = G.CH_CENTERS, G.V["RJ45_W"], G.V["RJ45_H"], 0.0
    POST_XY, POST_PIN, POST_ROOT = G.POSTS, None, 2.0
    ARM_STARTS = [("L", 22.0), ("L", 80.0), ("R", 22.0), ("R", 80.0), ("U", W / 2 - P["arm_l"] / 2)]
    PRY_Y = 55.0
    BATTERY = (40.0, 60.0)
    STANDOFF_POSTS = []
    LABEL = (4.5, 30.0)


def dev(xk, yk):
    """KiCad board coordinates -> device coordinates."""
    return xk, H - yk


def ws_dev(xv, yv):
    """Vendor module coordinates -> device X, Y."""
    return dev(*ws_xy(xv, yv))


ZM = MODULE_Z                                # vendor z -> device z
Z_GLASS = WS["glass_z"][0] + ZM              # glass underside = ledge
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
CX1, CX2 = -P["gap"], W + P["gap"]
CY1 = -P["gap_rj"]
CY2 = GY2 + P["glass_clr"]                             # the pocket wall is the inner face of the USB-C end
OX1, OX2 = CX1 - P["wall"], CX2 + P["wall"]
OY1, OY2 = CY1 - P["wall_rj"], CY2 + P["wall"]
R_IN = P["r_out"] - P["wall"]

STANDOFF_Z = WS["standoff_z"][0] + ZM                  # underside of the module's M2 standoffs
POST_PIN_OUT = 2.0                                     # the pins of the body posts stick out this far under the board

# USB-C end
Y_BAY = OY2 - P["bay_depth"]                           # floor of the bay
USB_X, USB_Z = WS_XC, WS["usbc_axis_z"] + ZM           # axis of the receptacle
USB_RIM_Y = ws_dev(0, WS["usbc_front_y"])[1]           # front edge of the receptacle shell
USB_BACK_Y = ws_dev(0, WS["usbc_back_y"])[1]           # where a plug's shell stops
BAY_Z = USB_Z + 0.2
KEY_Z = WS["button_act_z"] + ZM                        # axis of the switch plungers
KEYS = {name: ws_dev(xv, yv) for name, (xv, yv) in WS["buttons"].items()}   # x of the plunger, y of its tip
KEY_TIP_Y = max(y for _, y in KEYS.values())           # the two tips differ by 0.025 mm: one key fits both
KEY_STOP_Y = ws_dev(0, WS["tp_fpc_y"])[1] + 0.45       # nothing of the body reaches further in: drop-in path of the module

POSTS = [dev(x, y) for x, y in POST_XY]
ARMS = ARM_STARTS                                      # side, start along the wall


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
def make_body():
    body = rbox(OX1, OY1, OX2, OY2, Z_BOT, Z_TOP, P["r_out"])
    body = body.faces(">Z").edges().chamfer(P["r_top"])
    body = body.faces("<Z").edges().chamfer(0.4)
    body = body.cut(rbox(CX1, CY1, CX2, CY2, Z_BOT - 1, Z_PLATE, R_IN))
    # glass pocket: flush, the ledge is what is left of the top plate under the glass rim
    g = P["glass_clr"]
    body = body.cut(rbox(GX1 - g, GY1 - g, GX2 + g, GY2 + g, Z_GLASS, Z_TOP + 1, WS["glass_r"] + g))
    # drop-in opening for everything below the glass: PCB outline, touch flex tail, USB-C shell
    c = P["open_clr"]
    ax, ay = ws_dev(WS["pcb"][2] + c, WS["pcb"][3] + c)
    bx, by = ws_dev(WS["pcb"][0] - c, WS["tp_fpc_y"] - c - 0.05)
    body = body.cut(box(ax, ay, bx, by, Z_PLATE - 1, Z_GLASS + 0.01))
    ux1, uy1 = ws_dev(WS["usbc"][2] + 0.4, WS["usbc"][3])
    ux2, uy2 = ws_dev(WS["usbc"][0] - 0.4, WS["usbc"][1] - 0.4)
    body = body.cut(box(ux1, uy1, ux2, uy2, Z_PLATE - 1, Z_GLASS + 0.01))
    # USB-C end, outside: a shallow bay with 45 deg walls around the port and the two keys
    bay = (cq.Workplane("XZ").workplane(offset=-(OY2 + 2)).center(USB_X, BAY_Z)
           .rect(P["bay_w"] + 2 * P["bay_depth"], P["bay_h"] + 2 * P["bay_depth"]).extrude(2 + P["bay_depth"])
           .edges("|Y").fillet(P["bay_r"]).faces("<Y").chamfer(P["bay_depth"] - 0.001))
    body = body.cut(bay)
    # keys: a tunnel block under the top plate for each, bored for the key's body, and a hole for its cap
    kw, kh, kc = P["key_w"] / 2, P["key_h"] / 2, P["key_clr"]
    bh = kh + P["key_shoulder"] + kc
    for kx, _ in KEYS.values():
        body = body.union(box(kx - kw - kc - 1.1, KEY_STOP_Y, kx + kw + kc + 1.1, CY2 + 0.1, KEY_Z - bh - 1.2, Z_PLATE + 0.1))
        body = body.cut(box(kx - kw - kc, KEY_STOP_Y - 1, kx + kw + kc, CY2 + 0.01, KEY_Z - bh, KEY_Z + bh))
        hole = (cq.Workplane("XZ").workplane(offset=-(OY2 + 1)).center(kx, KEY_Z).rect(2 * (kw + kc), 2 * (kh + kc))
                .extrude(OY2 + 1 - CY2 + 0.5).edges("|Y").fillet(0.5))
        body = body.cut(hole)
    # USB-C: a well through the end wall for the plug's overmould, on the axis of the receptacle
    well = (cq.Workplane("XZ").workplane(offset=-(OY2 + 1)).center(USB_X, USB_Z).rect(P["usb_w"], P["usb_h"])
            .extrude(P["wall"] + 2).edges("|Y").fillet(P["usb_r"]))
    body = body.cut(well)
    # RJ45 windows with a small chamfer outwards
    hw = JACK_W / 2 + P["rj_clr"]
    z1, z2 = -P["rj_clr"], JACK_H + P["rj_clr"] + 0.1
    for xc in JACKS:
        body = body.cut(box(xc - hw, OY1 - 1, xc + hw, CY1 + 0.5, z1, z2))
        ch = (cq.Workplane("XZ").workplane(offset=-OY1).center(xc, (z1 + z2) / 2).rect(2 * hw + 1.4, z2 - z1 + 1.4)
              .workplane(offset=-0.7).rect(2 * hw, z2 - z1).loft())
        body = body.cut(ch)
    # posts that hold the carrier down
    for px, py in POSTS:
        body = body.union(cyl(px, py, 0.0, Z_PLATE + 0.1, P["post_d"]))
        if POST_PIN:                                   # a pin through the board's mounting hole: the board finds its place on four of them
            body = body.union(cyl(px, py, Z_CARR_BOT - POST_PIN_OUT, 0.01, POST_PIN).faces("<Z").chamfer(0.5))
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


def pocket_ribs():
    """Break-away ribs for printing the body with its top face on the bed.  They stand in the glass pocket under
    the ledge at its two ends - the only faces of the body that would otherwise start in mid-air - and stop
    one layer (0.2 mm) short of it.  They are separate from the body and come out with a fingernail."""
    g, c = P["glass_clr"], P["open_clr"]
    x1, x2, y1, y2 = GX1 - g, GX2 + g, GY1 - g, GY2 + g
    _, ay = ws_dev(WS["pcb"][2] + c, WS["pcb"][3] + c)
    _, by = ws_dev(WS["pcb"][0] - c, WS["tp_fpc_y"] - c - 0.05)
    ux1, uy1 = ws_dev(WS["usbc"][2] + 0.4, WS["usbc"][3])
    ux2, uy2 = ws_dev(WS["usbc"][0] - 0.4, WS["usbc"][1] - 0.4)
    z1, z2 = Z_GLASS + 0.2, Z_TOP                  # one layer short of the ledge
    pitch = 2.5
    n = int((x2 - x1 - 4.0) // pitch)
    x0 = (x1 + x2) / 2 - n * pitch / 2
    ribs = None
    for i in range(n + 1):
        x = x0 + i * pitch
        for ya, yb in ((y1 + 0.3, ay + 0.2), (by - 0.2, y2 - 0.3)):   # 0.2 past the edge of the opening: its outline lands on them
            r = box(x - 0.25, ya, x + 0.25, yb, z1, z2)
            ribs = r if ribs is None else ribs.union(r)
    return ribs.cut(box(ux1 - 0.3, uy1, ux2 + 0.3, uy2 + 0.3, z1 - 1, z2 + 1))


# ----------------------------------------------------------------------------- base
def battery_rect():
    """The space the corner stops on the base leave for the cell (x1, y1, x2, y2)."""
    if not BATTERY:
        return None
    bx1, bx2 = WS_XC - BATTERY[0] / 2 - 0.5, WS_XC + BATTERY[0] / 2 + 0.5
    by2 = CY2 - P["base_clr"] - P["arm_t"] - P["arm_slot"] - 1.0
    if STANDOFF_POSTS:                                 # the cell ends before the posts under the module
        by2 = min(ws_dev(*s)[1] for s in STANDOFF_POSTS) - 4.5 / 2 - POST_ROOT - 0.6
    return bx1, by2 - BATTERY[1] - 1.0, bx2, by2


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
    # posts under the carrier (same places as the body posts)
    def post(px, py, z_top, d):
        nonlocal base
        base = base.union(cyl(px, py, Z_BASE_TOP - 0.01, z_top, d))
        base = base.union(cq.Workplane("XY").workplane(offset=Z_BASE_TOP - 0.01).center(px, py).circle(d / 2 + POST_ROOT)
                          .workplane(offset=3.0).circle(d / 2).loft())
    for px, py in POSTS:
        post(px, py, Z_CARR_BOT, P["post_d"])
        if POST_PIN:                                   # socket for the pin of the body post above
            base = base.cut(cyl(px, py, Z_CARR_BOT - POST_PIN_OUT - 0.6, Z_CARR_BOT + 0.01, POST_PIN + 0.5))
    for xv, yv in STANDOFF_POSTS:                      # the module rests on these with its M2 standoffs
        px, py = ws_dev(xv, yv)
        post(px, py, STANDOFF_Z - P["rest_gap"], 4.5)
    # battery fence: corner stops for a cell up to 40 x 60 mm under the module
    if STANDOFF_POSTS:                                 # the two tall posts become a portal: a web between them
        (ax, ay), (bx, by) = [ws_dev(*q) for q in STANDOFF_POSTS]
        base = base.union(box(min(ax, bx), ay - 0.8, max(ax, bx), ay + 0.8, Z_BASE_TOP - 0.01, min(12.0, STANDOFF_Z - 4.0)))
    bx1, by1, bx2, by2 = battery_rect() or (0, 0, 0, 0)
    for cx, sx in ((bx1, 1), (bx2, -1)) if BATTERY else ():
        for cy, sy in ((by1, 1), (by2, -1)):
            base = base.union(box(min(cx, cx - sx * 1.4), min(cy, cy + sy * 9.0), max(cx, cx - sx * 1.4), max(cy, cy + sy * 9.0), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
            base = base.union(box(min(cx, cx + sx * 9.0), min(cy, cy - sy * 1.4), max(cx, cx + sx * 9.0), max(cy, cy - sy * 1.4), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
    # feet recesses, finger recesses at the pry notches, label
    for fx in (x1 + 10, x2 - 10):
        for fy in (y1 + 10, y2 - 10):
            base = base.cut(cyl(fx, fy, Z_BASE_BOT - 1, Z_BASE_BOT + P["foot_depth"], P["foot_d"]))
    for xa, xb in ((x1 - 1, x1 + 1.2), (x2 - 1.2, x2 + 1)):
        base = base.cut(box(xa, PRY_Y - 5.0, xb, PRY_Y + 5.0, Z_BASE_BOT - 1, Z_BASE_BOT + 1.0))
    label = (cq.Workplane("XY").workplane(offset=Z_BASE_BOT + 0.4).center(W / 2, LABEL[1])
             .text("RS232 CONSOLE", LABEL[0], -1.0, font="Liberation Sans", halign="center", valign="center"))
    base = base.cut(label.mirror("YZ", (W / 2, 0, 0)))
    return base


# ----------------------------------------------------------------------------- keys
def key_profile():
    """(y, z) outline of a key at rest: cap, taller body (the shoulders), nose under the module's PCB."""
    kh, sh = P["key_h"] / 2, P["key_shoulder"]
    y_tip = Y_BAY + P["key_proud"]
    y_sh = CY2 - 0.1                          # the shoulders rest against the inner face of the end wall
    y_body = KEY_STOP_Y + 0.45                # the body stays inside the tunnel, also when pressed
    y_nose = KEY_TIP_Y + P["key_gap"]
    zt = KEY_Z + WS["button_act"][1] / 2      # top of the nose = top of the plunger, 0.4 under the PCB
    zb = zt - P["key_nose_h"]
    c = 0.5
    return [(y_tip, KEY_Z - kh + c), (y_tip, KEY_Z + kh - c), (y_tip - c, KEY_Z + kh), (y_sh, KEY_Z + kh),
            (y_sh, KEY_Z + kh + sh), (y_body, KEY_Z + kh + sh), (y_body, zt), (y_nose + 0.4, zt), (y_nose, zt - 0.4),
            (y_nose, zb), (y_body, zb), (y_body, KEY_Z - kh - sh), (y_sh, KEY_Z - kh - sh), (y_sh, KEY_Z - kh), (y_tip - c, KEY_Z - kh)]


def make_key(kx):
    """One key in its rest position.  A flat prism, 3 mm thick: printed lying on its side."""
    return prism_x(key_profile(), kx - P["key_w"] / 2, kx + P["key_w"] / 2)


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


def rest_dims():
    """Numbers of the cradle (two-port variant), device coordinates."""
    so = sorted(WS["standoffs"], key=lambda t: t[1])[2:]
    xs = sorted(ws_dev(x, y)[0] for x, y in so)
    yc = ws_dev(*so[0])[1]
    r = WS["standoff_d"] / 2 + 0.2
    bw, bl, bt = HB.BATTERY
    wall = 1.2
    d = {"xa": xs[0] - r, "xb": xs[1] + r, "y1": yc - REST_T / 2, "y2": yc + REST_T / 2,       # bar; the standoffs sit between xa and xb
         "z_seat": STANDOFF_Z - P["rest_gap"], "z_horn": STANDOFF_Z - P["rest_gap"] + 1.3, "wall": wall}
    # battery pocket: a tube towards the USB-C end, above the wires at J3 and below the module
    z0 = 6.6
    d["pocket"] = [WS_XC - bw / 2 - 0.5, d["y2"] + 7.2, WS_XC + bw / 2 + 0.5, d["y2"] + 7.2 + bl + 0.5, z0 + wall, z0 + wall + bt + 1.0]   # inside
    px1, py1, px2, py2, pz1, pz2 = d["pocket"]
    d["tube"] = [px1 - wall, d["y2"], px2 + wall, py2, z0, pz2 + wall]
    d["fin_y"] = dev(0, 8.5)[1]                      # the two fins stand on the board up to here: J3 and its wires stay free
    return d


def make_rest():
    """Two-port variant: the cradle.  A bar stands on the board in the free strip between the resistor row and
    the jumper row and carries the module's two standoffs at the antenna end, where no post from the base can
    reach; horns at its ends hold it between them.  From the bar a tube runs towards the USB-C end, between the
    two pin rows: the battery slides into it.  Two fins under the tube stand on the board.
    Printed with the bar's jack side on the bed; everything else grows straight up from it."""
    d = rest_dims()
    xa, xb, z_seat, z_horn = d["xa"], d["xb"], d["z_seat"], d["z_horn"]
    bar = prism_y([(xa - 2.0, 0), (xb + 2.0, 0), (xb + 2.0, z_horn), (xb, z_horn), (xb, z_seat), (xa, z_seat), (xa, z_horn), (xa - 2.0, z_horn)],
                  d["y1"], d["y2"])
    tx1, ty1, tx2, ty2, tz1, tz2 = d["tube"]
    px1, py1, px2, py2, pz1, pz2 = d["pocket"]
    tube = box(tx1, ty1 - 0.01, tx2, ty2, tz1, tz2).cut(box(px1, py1, px2, ty2 + 1, pz1, pz2))
    y_r1, y_r2 = dev(0, 21.0)[1], dev(0, 20.0)[1]      # over the resistor row the fins stay 1 mm above the board
    fin = [(ty1 - 0.01, 1.0), (y_r1, 1.0), (y_r2, 0.0), (d["fin_y"], 0.0), (d["fin_y"], tz1 + 0.01), (ty1 - 0.01, tz1 + 0.01)]
    rest = bar.union(tube)
    for x in (tx1, tx2 - d["wall"]):
        rest = rest.union(prism_x(fin, x, x + d["wall"]))
    return rest


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


REST_T = 2.6


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
def checks(body, base, module, carrier, vendor, keys, rest=None):
    ok = True

    def report(name, value, limit=0.01, unit="mm3"):
        nonlocal ok
        good = value <= limit
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} {name}: {value:.3f} {unit}")

    mod_all = None
    for s in module.values():
        mod_all = s if mod_all is None else mod_all.union(s)
    car_all = carrier["pcb"].union(carrier["parts_top"]).union(carrier["parts_bottom"])
    print("assembled:")
    report("body x module (envelope)", vol(body.intersect(mod_all)))
    report("body x carrier", vol(body.intersect(car_all)))
    report("base x carrier", vol(base.intersect(car_all)))
    # the catches are meant to bear on their pockets with a small preload: allow exactly that overlap
    preload_volume = 5 * P["bump_l"] * P["preload"] * P["bump"] * 1.3
    report("base x body (only the preload of the five catches)", vol(base.intersect(body)), preload_volume)
    if VARIANT == "hat":
        report("module x board (the module hangs free above it)", vol(mod_all.intersect(car_all)))
        pins = module["headers"].val().BoundingBox().zmin
        print(f"  pin tips {pins:.2f} mm above the board, tallest part under the module {max(b[5] for b in HB.boxes()):.2f} mm")
        report("cradle x board", vol(rest.intersect(car_all)))
        report("cradle x module", vol(rest.intersect(mod_all)))
        report("cradle x body", vol(rest.intersect(body)))
        report("base x module", vol(base.intersect(mod_all)))
        report("base x cradle", vol(base.intersect(rest)))
        plugs, turn = make_plugs()
        wiring = plugs.union(turn)
        for name, part in (("board", car_all), ("cradle", rest), ("base", base), ("body", body)):
            report(f"plugs of the jumper wires and their wires' turn x {name}", vol(wiring.intersect(part)))
        d = rest_dims()
        px1, py1, px2, py2, pz1, pz2 = d["pocket"]
        bw, bl, bt = HB.BATTERY
        cell = box(WS_XC - bw / 2, py1 + 0.2, WS_XC + bw / 2, py1 + 0.2 + bl, pz1 + 0.2, pz1 + 0.2 + bt)
        report("battery in its pocket x cradle", vol(cell.intersect(rest)))
        report("battery x plugs", vol(cell.intersect(wiring)))
        print(f"  plugs end {plugs.val().BoundingBox().zmin:.1f} mm above the board, the wires turn in the {HB.WIRE_BEND:.0f} mm below; "
              f"{px1 - d['wall'] - (ws_dev(WS['hdr_x'], 0)[0] + HB.PLUG_W / 2):.1f} mm between pocket and plugs")
        lift = P["rest_gap"] + 0.05                   # raised by a little more than the play, each support must meet a standoff
        for name, part in (("base posts", base), ("cradle", rest)):
            hit = vol(part.translate((0, 0, lift)).intersect(module["standoffs"]))
            print(f"  {'ok  ' if hit > 0.01 else 'FAIL'} {name} carry the module's standoffs: overlap {hit:.3f} mm3 when raised {lift:.2f} mm")
            ok &= hit > 0.01
    else:
        report("module x carrier (pins in sockets excluded)", vol(mod_all.cut(module["headers"]).intersect(car_all)))
    if vendor is not None:
        report("body x module (vendor STEP)", vol(body.intersect(vendor)))
        if rest is not None:
            report("cradle x module (vendor STEP)", vol(rest.intersect(vendor)))
            report("base x module (vendor STEP)", vol(base.intersect(vendor)))
            report("board x module (vendor STEP)", vol(car_all.intersect(vendor)))
        bb = vendor.val().BoundingBox()
        e = mod_all.val().BoundingBox()
        d = max(abs(bb.xmin - e.xmin), abs(bb.xmax - e.xmax), abs(bb.ymin - e.ymin), abs(bb.ymax - e.ymax), abs(bb.zmin - e.zmin), abs(bb.zmax - e.zmax))
        report("envelope model vs vendor STEP, largest bounding-box difference", d, 0.02, "mm")
    print("assembly paths:")
    worst = max(vol(body.intersect(mod_all.translate((0, 0, k)))) for k in (0.5, 1, 2, 4, 6, 8, 12, 16, 20, 25))
    report("module lifted out of the body (drop-in path)", worst)
    worst = max(vol(body.intersect(car_all.translate((0, 0, -k)))) for k in (0.5, 1, 2, 4, 8, 12, 16, 22, 30))
    report("carrier pulled out downwards", worst)
    # the base on its way in: only the five catches may touch, and by no more than the arms can give
    vals = [(k, vol(body.intersect(base.translate((0, 0, -k))))) for k in (0.2, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0)]
    worst = max(v for _, v in vals)
    catch_volume = 5 * P["bump_l"] * (P["bump"] - P["base_clr"]) ** 2 * 3
    print("  " + ", ".join(f"{k:.1f} mm: {v:.2f}" for k, v in vals))
    report("base pushed in: overlap stays within the catches", worst, catch_volume)
    print("keys (RST, BOOT):")
    stroke = P["key_stroke"]
    report("keys at rest x body", vol(body.intersect(keys)))
    report("keys at rest x module", vol(keys.intersect(mod_all)))
    pressed = keys.translate((0, -stroke, 0))
    report(f"keys pressed {stroke} mm x body (free travel)", vol(body.intersect(pressed)))
    report("keys pressed x module without its switches", vol(pressed.intersect(mod_all.cut(module["buttons"]))))
    hit = vol(keys.translate((0, -(P["key_gap"] + 0.06), 0)).intersect(module["buttons"]))
    print(f"  {'ok  ' if hit > 0.01 else 'FAIL'} the nose meets the switch plunger after {P['key_gap']:.2f} mm: overlap {hit:.3f} mm3 at {P['key_gap'] + 0.06:.2f} mm")
    ok &= hit > 0.01
    worst = max(vol(keys.intersect(mod_all.translate((0, 0, k)))) for k in (0.3, 0.6, 1, 1.5, 2, 3, 4, 6, 8, 12, 16))
    report("module dropped in past the keys", worst)
    worst = max(vol(body.intersect(keys.translate((0, -k, 0)))) for k in (1, 2, 3, 4, 5, 6, 8))
    report("keys slid into their tunnels from inside", worst)
    if vendor is not None:
        report("keys at rest x module (vendor STEP)", vol(keys.intersect(vendor)))
        hitv = vol(keys.translate((0, -(P["key_gap"] + 0.06), 0)).intersect(vendor))
        print(f"  {'ok  ' if hitv > 0.01 else 'FAIL'} the nose meets the plunger of the vendor model: overlap {hitv:.3f} mm3")
        ok &= hitv > 0.01
    print("USB-C plug (shell 8.25 x 2.40 x 6.65, overmould 12.35 x 6.5, the USB-IF maximum):")
    shell, over = make_plug()
    plug = shell.union(over)
    report("plug fully mated x body", vol(body.intersect(plug)))
    report("plug fully mated x keys", vol(keys.intersect(plug)))
    report("plug fully mated x module", vol(mod_all.intersect(plug)))
    worst = max(vol(body.intersect(plug.translate((0, k, 0)))) for k in (0.5, 1, 2, 3, 4, 5, 7))
    report("plug on its way in x body", worst)
    if vendor is not None:
        print(f"  info plug shell x vendor receptacle (contact springs and latches reach into the plug): {vol(shell.intersect(vendor)):.3f} mm3")
        report("plug overmould x module (vendor STEP)", vol(over.intersect(vendor)))
    U = G.USBC_PLUG
    over_y = USB_BACK_Y + U["shell_l"]
    print(f"  receptacle rim {OY2 - USB_RIM_Y:.2f} mm behind the outer face, {Y_BAY - USB_RIM_Y:.2f} mm behind the bay floor")
    print(f"  mated: overmould ends {over_y - USB_RIM_Y:.2f} mm in front of the rim, {Y_BAY - over_y:.2f} mm below the bay floor, {OY2 - over_y:.2f} mm below the outer face")
    print(f"  room around the largest overmould: {(P['usb_w'] - U['over_w']) / 2:.2f} mm each side, {(P['usb_h'] - U['over_h']) / 2:.2f} mm above and below")
    print("glass seat:")
    g = module["glass"]
    under = box(GX1 - 1, GY1 - 1, GX2 + 1, GY2 + 1, Z_GLASS - 0.05, Z_GLASS)
    ledge = vol(body.intersect(under).intersect(box(GX1, GY1, GX2, GY2, Z_GLASS - 1, Z_GLASS))) / 0.05
    print(f"  ledge area under the glass: {ledge:.0f} mm2 of {37.1 * 58.8 - (4 - math.pi) * 2.6 ** 2:.0f} mm2 glass")
    ok &= ledge > 300
    mat, e_mod, strain_max = MATERIAL
    print(f"snap arms ({mat}, E ~ {e_mod:.0f} MPa):")
    y = P["bump"] - P["base_clr"]
    L, t, b = P["arm_l"] - 0.5 - P["bump_l"] / 2, P["arm_t"], P["arm_h"]
    strain = 1.5 * t * y / L ** 2
    force = e_mod * b * t ** 3 * y / (4 * L ** 3)
    mu = 0.3
    fin = force * (mu + math.tan(math.radians(P["lead_deg"]))) / (1 - mu * math.tan(math.radians(P["lead_deg"])))
    fout = force * (mu + 1.0) / (1 - mu)
    print(f"  deflection {y:.2f} mm, strain {strain * 100:.2f} % (limit {strain_max * 100:.1f} %), force per arm {force:.1f} N")
    print(f"  push-in about {5 * fin:.0f} N, pull-out about {5 * fout:.0f} N for five arms")
    print(f"  held bent by the preload: {1.5 * t * P['preload'] / L ** 2 * 100:.2f} % strain")
    ok &= strain < strain_max
    ribs = pocket_ribs()
    print("printing the body with its top face on the bed:")
    report("break-away ribs in the glass pocket x body", vol(body.intersect(ribs)))
    return ok


# ----------------------------------------------------------------------------- output
def main():
    os.makedirs(OUT, exist_ok=True)
    body, base = make_body(), make_base()
    module, carrier = make_module(), make_carrier()
    key_parts = {name: make_key(kx) for name, (kx, _) in KEYS.items()}
    keys = None
    for k in key_parts.values():
        keys = k if keys is None else keys.union(k)
    bb = body.val().BoundingBox()
    print(f"body  {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm, {vol(body) / 1000:.1f} cm3")
    bb2 = base.val().BoundingBox()
    print(f"base  {bb2.xlen:.1f} x {bb2.ylen:.1f} x {bb2.zlen:.1f} mm, {vol(base) / 1000:.1f} cm3")
    vendor = None
    if "--no-vendor" not in sys.argv and os.path.exists(VENDOR_STEP):
        vendor = load_vendor()
    rest = make_rest() if VARIANT == "hat" else None
    ok = checks(body, base, module, carrier, vendor, keys, rest)

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
    step(body, "body.step")
    step(base, "base.step")
    cq.exporters.export(body, os.path.join(OUT, "body.stl"), tolerance=0.02, angularTolerance=0.15)
    cq.exporters.export(base, os.path.join(OUT, "base.stl"), tolerance=0.02, angularTolerance=0.15)
    # one key, lying on its side as it is printed (two are needed)
    k0x = KEYS["RST"][0]
    key_flat = key_parts["RST"].translate((-k0x + P["key_w"] / 2, -KEY_STOP_Y, -KEY_Z)).rotate((0, 0, 0), (0, 1, 0), -90)
    step(key_flat, "key.step")
    cq.exporters.export(key_flat, os.path.join(OUT, "key.stl"), tolerance=0.01, angularTolerance=0.1)
    kb = key_flat.val().BoundingBox()
    print(f"key   {kb.xlen:.1f} x {kb.ylen:.1f} x {kb.zlen:.1f} mm (print two, flat as exported)")
    if rest is not None:
        rb = rest.val().BoundingBox()
        rest_flat = rest.translate((-rb.xmin, -rb.ymin, -rb.zmin)).rotate((0, 0, 0), (1, 0, 0), 90).translate((0, rb.zlen, 0))
        step(rest_flat, "cradle.step")
        cq.exporters.export(rest_flat, os.path.join(OUT, "cradle.stl"), tolerance=0.01, angularTolerance=0.1)
        fb = rest_flat.val().BoundingBox()
        print(f"cradle {fb.xlen:.1f} x {fb.ylen:.1f} x {fb.zlen:.1f} mm (print one, standing as exported)")
    # print-ready files: every part as it goes on the bed
    pd = os.path.join(OUT, "print")
    os.makedirs(pd, exist_ok=True)

    def face_down(shape):
        return shape.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, 0, Z_TOP))
    body_print = cq.Workplane("XY").add(cq.Compound.makeCompound([face_down(body).val(), face_down(pocket_ribs()).val()]))
    cq.exporters.export(body_print, os.path.join(pd, "body_print.stl"), tolerance=0.02, angularTolerance=0.15)
    cq.exporters.export(base.translate((0, 0, -Z_BASE_BOT)), os.path.join(pd, "base_print.stl"), tolerance=0.02, angularTolerance=0.15)
    two_keys = cq.Workplane("XY").add(cq.Compound.makeCompound([key_flat.val(), key_flat.translate((kb.xlen + 5.0, 0, 0)).val()]))
    cq.exporters.export(two_keys, os.path.join(pd, "key_print_x2.stl"), tolerance=0.01, angularTolerance=0.1)
    if rest is not None:
        cq.exporters.export(rest_flat, os.path.join(pd, "cradle_print.stl"), tolerance=0.01, angularTolerance=0.1)
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
    asm = cq.Assembly(name="rs232_console")
    asm.add(body, name="body", color=cq.Color(0.9, 0.9, 0.88))
    asm.add(base, name="base", color=cq.Color(0.6, 0.6, 0.6))
    for name, k in key_parts.items():
        asm.add(k, name=f"key_{name}", color=cq.Color(0.35, 0.36, 0.38))
    if rest is not None:
        asm.add(rest, name="cradle", color=cq.Color(0.35, 0.36, 0.38))
    asm.add(mod, name="module")
    asm.add(car, name="carrier")
    asm.save(os.path.join(OUT, "rs232_console_assembly.step"))
    if vendor is not None:
        # the same assembly with the vendor's own model of the module (every part of it), for CAD work
        full = cq.Assembly(name="rs232_console_vendor_module")
        full.add(body, name="body", color=cq.Color(0.9, 0.9, 0.88))
        full.add(base, name="base", color=cq.Color(0.6, 0.6, 0.6))
        for name, k in key_parts.items():
            full.add(k, name=f"key_{name}", color=cq.Color(0.35, 0.36, 0.38))
        if rest is not None:
            full.add(rest, name="cradle", color=cq.Color(0.35, 0.36, 0.38))
        full.add(vendor, name="waveshare_esp32_s3_touch_lcd_2_vendor")
        full.add(car, name="carrier")
        full.save(os.path.join(OUT, "rs232_console_assembly_vendor_module.step"))
    # meshes for the renderer
    shell, over = make_plug()
    extra = [("keys", keys), ("plug_shell", shell), ("plug_over", over)] + ([("rest", rest), ("plugs", make_plugs()[0])] if rest is not None else [])
    for name, s in [("pcb_ws" if k == "pcb" else k, v) for k, v in module.items()] + list(carrier.items()) + extra:
        cq.exporters.export(s, os.path.join(OUT, f"_{name}.stl"), tolerance=0.02 if name in ("usbc", "buttons", "keys") else 0.03, angularTolerance=0.2)
    import json
    va = ws_dev(WS["glass"][0] + 3.06 + 30.99 / 2, WS["glass"][3] - 7.51 - 41.20 / 2)     # centre of the lit view area
    hat = None
    if VARIANT == "hat":
        so = sorted(WS["standoffs"], key=lambda t: t[1])
        hat = {"W": W, "H": H, "holes": [list(dev(*h)) for h in HB.HOLES], "hole_d": HB.HOLE_D,
               "jacks": [[dev(j[1], j[4])[0], dev(j[1], j[4])[1], dev(j[3], j[2])[0], dev(j[3], j[2])[1]] for j in HB.JACKS],
               "parts": [[b[0], dev(b[1], b[4])[0], dev(b[1], b[4])[1], dev(b[3], b[2])[0], dev(b[3], b[2])[1], b[5]] for b in HB.boxes()],
               "j3": [list(dev(*q)) for q in HB.J3], "pin_below": HB.PIN_BELOW,
               "module_pcb": [*ws_dev(WS["pcb"][2], WS["pcb"][3]), *ws_dev(WS["pcb"][0], WS["pcb"][1])],
               "glass": [GX1, GY1, GX2, GY2], "standoffs_base": [list(ws_dev(*q)) for q in so[:2]], "standoffs_rest": [list(ws_dev(*q)) for q in so[2:]],
               "standoff_z": STANDOFF_Z, "rest_t": REST_T, "cradle": rest_dims(), "cell": list(HB.BATTERY), "plug": [HB.PLUG_LEN, HB.PLUG_W, HB.WIRE_BEND],
               "plugs": {name: list(ws_dev(-WS["hdr_x"] if n <= 14 else WS["hdr_x"], WS["hdr_y1"] - 2.54 * ((n - 1) if n <= 14 else (28 - n)))) for name, n in HB.MODULE_PINS.items()},
               "hdr_plastic_z": WS["hdr_plastic_z"][0] + ZM, "pin_tip_z": WS["hdr_pin_tip_z"] + ZM, "cavity": [CX1, CY1, CX2, CY2], "outer": [OX1, OY1, OX2, OY2],
               "pcb_z": [WS["pcb_z"][0] + ZM, WS["pcb_z"][1] + ZM], "hdr_x": [ws_dev(WS["hdr_x"], 0)[0], ws_dev(-WS["hdr_x"], 0)[0]],
               "hdr_y": [ws_dev(0, WS["hdr_y1"])[1], ws_dev(0, WS["hdr_y1"] - 13 * 2.54)[1]]}
    json.dump({"variant": VARIANT, "hat": hat, "jacks": list(JACKS), "jack_w": JACK_W, "jack_h": JACK_H, "jack_front": JACK_FRONT, "ws_xc": WS_XC, "battery": list(BATTERY or HB.BATTERY), "battery_rect": battery_rect() and list(battery_rect()),
               "oy": [OY1, OY2], "posts": [list(p) for p in POSTS],
               "z_top": Z_TOP, "z_glass_top": Z_GLASS_TOP, "z_base_top": Z_BASE_TOP, "cy2": CY2, "view_area_centre": va, "outer": [bb.xlen, bb.ylen, bb.zlen],
               "P": P, "z": {"bot": Z_BOT, "base_bot": Z_BASE_BOT, "base_top": Z_BASE_TOP, "bay": Z_BAY, "plate": Z_PLATE, "glass": Z_GLASS,
                             "top": Z_TOP, "bump": Z_BUMP},
               "bump_profile": bump_profile(Z_BUMP), "recess_profile": recess_profile(Z_BUMP),
               "glass_x": [GX1, GX2], "open_x": [ws_dev(WS["pcb"][2] + P["open_clr"], 0)[0], ws_dev(WS["pcb"][0] - P["open_clr"], 0)[0]],
               "cx": [CX1, CX2], "ox": [OX1, OX2], "zm": ZM,
               "usb": {"x": USB_X, "z": USB_Z, "rim_y": USB_RIM_Y, "back_y": USB_BACK_Y, "bay_y": Y_BAY, "bay_z": BAY_Z, "oy2": OY2, "cy2": CY2,
                       "plug": G.USBC_PLUG, "shell": WS["usbc_shell"], "open": WS["usbc_open"], "tongue": WS["usbc_tongue"][:2],
                       "tongue_tip_y": ws_dev(0, WS["usbc_tongue"][2])[1], "rear_y": ws_dev(0, WS["usbc"][3])[1]},
               "key": {"profile": key_profile(), "z": KEY_Z, "x": {n: v[0] for n, v in KEYS.items()}, "tip_y": KEY_TIP_Y, "stop_y": KEY_STOP_Y,
                       "act": WS["button_act"], "body_d": WS["button_d"], "body_z": [WS["button_z"][0] + ZM, WS["button_z"][1] + ZM],
                       "pcb_edge_y": ws_dev(0, WS["pcb"][1])[1], "pcb_z": [WS["pcb_z"][0] + ZM, WS["pcb_z"][1] + ZM], "glass_y": GY2,
                       "tunnel_z": [KEY_Z - P["key_h"] / 2 - P["key_shoulder"] - P["key_clr"], KEY_Z + P["key_h"] / 2 + P["key_shoulder"] + P["key_clr"]]}, "ws": {k: v for k, v in WS.items() if k.endswith("_z") or k in ("pcb", "lcm", "pmma", "tp_sensor", "glass")}},
              open(os.path.join(OUT, "geometry.json"), "w"), indent=1)
    with open(os.path.join(OUT, "dimensions.txt"), "w") as fh:
        fh.write(f"outer {bb.xlen:.2f} x {bb.ylen:.2f} x {bb.zlen:.2f} mm\n"
                 f"z: bottom {Z_BOT:.2f}, base {Z_BASE_BOT:.2f}..{Z_BASE_TOP:.2f}, carrier {Z_CARR_BOT:.2f}..0, top plate {Z_PLATE:.2f}, ledge {Z_GLASS:.2f}, top {Z_TOP:.2f}\n"
                 f"glass pocket x {GX1 - P['glass_clr']:.2f}..{GX2 + P['glass_clr']:.2f}, y {GY1 - P['glass_clr']:.2f}..{GY2 + P['glass_clr']:.2f}, depth {Z_TOP - Z_GLASS:.2f}\n"
                 f"USB-C: axis x {USB_X:.2f} z {USB_Z:.2f}, receptacle rim y {USB_RIM_Y:.2f}, plug stop y {USB_BACK_Y:.2f}, well {P['usb_w']:.1f} x {P['usb_h']:.1f}, bay floor y {Y_BAY:.2f}, outer face y {OY2:.2f}\n"
                 f"keys: axis z {KEY_Z:.2f}, x {', '.join(f'{n} {v[0]:.2f}' for n, v in KEYS.items())}, cap {P['key_w']:.1f} x {P['key_h']:.1f}, {P['key_proud']:.1f} above the bay floor, stroke {P['key_stroke']:.1f}\n")
    print("wrote", OUT, "- all checks passed" if ok else "- CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
