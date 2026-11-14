#!/usr/bin/env python3
"""Case for the RS232 console, revision 0.4 - real solid CAD (CadQuery / OpenCascade).

    <venv>/bin/python case.py            # STEP + STL of every part, assembly STEP, checks
    <venv>/bin/python case.py --no-vendor   # skip the checks against the vendor STEP

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
import sys

import cadquery as cq

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "carrier"))
import generate as G   # noqa: E402

WS = G.WS
OUT = os.path.join(HERE, "out")
VENDOR_STEP = os.path.join(HERE, "..", "vendor", "esp32-s3-touch-lcd-2_20241108.stp")

# ----------------------------------------------------------------------------- parameters
P = dict(
    wall=2.4,            # side walls and USB-C end
    wall_rj=1.6,         # port end: thin, so the jacks sit 1.9 mm behind the outer face
    gap=0.5,             # carrier to side walls
    gap_rj=0.3,          # carrier edge to the port wall
    r_out=6.0,           # outer vertical corner radius
    r_top=1.5,           # radius of the top edge
    plug_gap=0.3,        # the module sits this much above "fully plugged": the glass always seats on its ledge first
    top_t=3.0,           # top plate
    glass_clr=0.25,      # glass to pocket wall (lens tolerance +-0.05, FDM)
    open_clr=0.30,       # module PCB to the drop-in opening
    bot_clr=2.6,         # under the carrier: through-hole pins 2 mm, relays 1.75 mm
    bat_t=7.0,           # battery bay height
    base_t=2.0, base_inset=0.6, base_clr=0.25,
    post_d=5.0,
    # snap arms (in-plane cantilevers on the base)
    arm_l=16.0, arm_t=2.0, arm_h=5.0, arm_slot=1.6,
    bump=0.9, bump_l=4.0, bump_land=0.6, lead_deg=30.0, preload=0.2, recess_d=1.0,
    # USB-C end: a shallow bay with 45 deg walls holds the port and the two keys below the outer face
    bay_depth=1.0, bay_w=26.0, bay_h=9.6, bay_r=3.0,
    usb_w=13.4, usb_h=7.5, usb_r=2.0,    # well for the plug's overmould (USB-IF maximum 12.35 x 6.5)
    key_w=3.0, key_h=6.0,                # the visible cap
    key_proud=0.8,                       # cap above the bay floor (0.2 below the outer face)
    key_clr=0.15,                        # sliding fit in hole and tunnel
    key_gap=0.15,                        # nose to switch plunger at rest
    key_shoulder=1.0,                    # the key's body is taller than its cap: it cannot leave outwards
    key_nose_h=1.9, key_stroke=0.6,
    rj_clr=0.3, foot_d=10.4, foot_depth=0.5,
)
H = G.BOARD_H
W = G.BOARD_W


def dev(xk, yk):
    """KiCad board coordinates -> device coordinates."""
    return xk, H - yk


def ws_dev(xv, yv):
    """Vendor module coordinates -> device X, Y."""
    return dev(*G.ws_xy(xv, yv))


ZM = G.WS_Z0 + P["plug_gap"]                 # vendor z -> device z
Z_TOP = WS["glass_z"][1] + ZM                # glass front = top face of the body
Z_GLASS = WS["glass_z"][0] + ZM              # glass underside = ledge
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

# USB-C end
Y_BAY = OY2 - P["bay_depth"]                           # floor of the bay
USB_X, USB_Z = G.WS_XC, WS["usbc_axis_z"] + ZM         # axis of the receptacle
USB_RIM_Y = ws_dev(0, WS["usbc_front_y"])[1]           # front edge of the receptacle shell
USB_BACK_Y = ws_dev(0, WS["usbc_back_y"])[1]           # where a plug's shell stops
BAY_Z = USB_Z + 0.2
KEY_Z = WS["button_act_z"] + ZM                        # axis of the switch plungers
KEYS = {name: ws_dev(xv, yv) for name, (xv, yv) in WS["buttons"].items()}   # x of the plunger, y of its tip
KEY_TIP_Y = max(y for _, y in KEYS.values())           # the two tips differ by 0.025 mm: one key fits both
KEY_STOP_Y = ws_dev(0, WS["tp_fpc_y"])[1] + 0.45       # nothing of the body reaches further in: drop-in path of the module

POSTS = [dev(x, y) for x, y in G.POSTS]
ARMS = [("L", 22.0), ("L", 80.0), ("R", 22.0), ("R", 80.0), ("U", W / 2 - P["arm_l"] / 2)]   # side, start along the wall


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
    body = body.faces(">Z").edges().fillet(P["r_top"])
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
    hw = G.V["RJ45_W"] / 2 + P["rj_clr"]
    z1, z2 = -P["rj_clr"], G.V["RJ45_H"] + P["rj_clr"] + 0.1
    for xc in G.CH_CENTERS:
        body = body.cut(box(xc - hw, OY1 - 1, xc + hw, CY1 + 0.5, z1, z2))
        ch = (cq.Workplane("XZ").workplane(offset=-OY1).center(xc, (z1 + z2) / 2).rect(2 * hw + 1.4, z2 - z1 + 1.4)
              .workplane(offset=-0.7).rect(2 * hw, z2 - z1).loft())
        body = body.cut(ch)
    # posts that hold the carrier down
    for px, py in POSTS:
        body = body.union(cyl(px, py, 0.0, Z_PLATE + 0.1, P["post_d"]))
    # port numbers, debossed
    for i, xc in enumerate(G.CH_CENTERS):
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
        body = body.cut(box(x1, 49.0, x2, 61.0, Z_BOT - 1, Z_BASE_BOT + 1.0))
    return body


# ----------------------------------------------------------------------------- base
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
    for px, py in POSTS:
        base = base.union(cyl(px, py, Z_BASE_TOP - 0.01, Z_CARR_BOT, P["post_d"]))
        base = base.union(cq.Workplane("XY").workplane(offset=Z_BASE_TOP - 0.01).center(px, py).circle(P["post_d"] / 2 + 2.0)
                          .workplane(offset=3.0).circle(P["post_d"] / 2).loft())
    # battery fence: corner stops for a cell up to 40 x 60 mm under the module
    bx1, bx2 = G.WS_XC - 20.5, G.WS_XC + 20.5
    by2 = CY2 - e - P["arm_t"] - P["arm_slot"] - 1.0
    by1 = by2 - 61.0
    for cx, sx in ((bx1, 1), (bx2, -1)):
        for cy, sy in ((by1, 1), (by2, -1)):
            base = base.union(box(min(cx, cx - sx * 1.4), min(cy, cy + sy * 9.0), max(cx, cx - sx * 1.4), max(cy, cy + sy * 9.0), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
            base = base.union(box(min(cx, cx + sx * 9.0), min(cy, cy - sy * 1.4), max(cx, cx + sx * 9.0), max(cy, cy - sy * 1.4), Z_BASE_TOP - 0.01, Z_BASE_TOP + 4.0))
    # feet recesses, finger recesses at the pry notches, label
    for fx in (x1 + 10, x2 - 10):
        for fy in (y1 + 10, y2 - 10):
            base = base.cut(cyl(fx, fy, Z_BASE_BOT - 1, Z_BASE_BOT + P["foot_depth"], P["foot_d"]))
    for xa, xb in ((x1 - 1, x1 + 1.2), (x2 - 1.2, x2 + 1)):
        base = base.cut(box(xa, 50.0, xb, 60.0, Z_BASE_BOT - 1, Z_BASE_BOT + 1.0))
    label = (cq.Workplane("XY").workplane(offset=Z_BASE_BOT + 0.4).center(W / 2, 30.0)
             .text("RS232 CONSOLE", 4.5, -1.0, font="Liberation Sans", halign="center", valign="center"))
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
    parts["glass"] = rbox(GX1, GY1, GX2, GY2, Z_GLASS, Z_TOP, WS["glass_r"])
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


def make_carrier():
    """Carrier board with its parts as boxes (fab outline x height), from the board generator."""
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
def checks(body, base, module, carrier, vendor, keys):
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
    report("module x carrier (pins in sockets excluded)", vol(mod_all.cut(module["headers"]).intersect(car_all)))
    if vendor is not None:
        report("body x module (vendor STEP)", vol(body.intersect(vendor)))
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
    print("snap arms (PETG, E ~ 2000 MPa):")
    y = P["bump"] - P["base_clr"]
    L, t, b = P["arm_l"] - 0.5 - P["bump_l"] / 2, P["arm_t"], P["arm_h"]
    strain = 1.5 * t * y / L ** 2
    force = 2000 * b * t ** 3 * y / (4 * L ** 3)
    mu = 0.3
    fin = force * (mu + math.tan(math.radians(P["lead_deg"]))) / (1 - mu * math.tan(math.radians(P["lead_deg"])))
    fout = force * (mu + 1.0) / (1 - mu)
    print(f"  deflection {y:.2f} mm, strain {strain * 100:.2f} % (limit about 2 %), force per arm {force:.1f} N")
    print(f"  push-in about {5 * fin:.0f} N, pull-out about {5 * fout:.0f} N for five arms")
    ok &= strain < 0.02
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
    ok = checks(body, base, module, carrier, vendor, keys)

    cq.exporters.export(body, os.path.join(OUT, "body.step"))
    cq.exporters.export(base, os.path.join(OUT, "base.step"))
    cq.exporters.export(body, os.path.join(OUT, "body.stl"), tolerance=0.02, angularTolerance=0.15)
    cq.exporters.export(base, os.path.join(OUT, "base.stl"), tolerance=0.02, angularTolerance=0.15)
    # one key, lying on its side as it is printed (two are needed)
    k0x = KEYS["RST"][0]
    key_flat = key_parts["RST"].translate((-k0x + P["key_w"] / 2, -KEY_STOP_Y, -KEY_Z)).rotate((0, 0, 0), (0, 1, 0), -90)
    cq.exporters.export(key_flat, os.path.join(OUT, "key.step"))
    cq.exporters.export(key_flat, os.path.join(OUT, "key.stl"), tolerance=0.01, angularTolerance=0.1)
    kb = key_flat.val().BoundingBox()
    print(f"key   {kb.xlen:.1f} x {kb.ylen:.1f} x {kb.zlen:.1f} mm (print two, flat as exported)")
    colours = {"glass": (0.04, 0.04, 0.05), "tp_sensor": (0.2, 0.2, 0.22), "lcm": (0.55, 0.55, 0.57), "pmma": (0.85, 0.9, 0.95),
               "pcb": (0.05, 0.15, 0.45), "headers": (0.85, 0.75, 0.2), "standoffs": (0.8, 0.65, 0.3), "usbc": (0.75, 0.75, 0.78),
               "buttons": (0.9, 0.9, 0.9), "bat_header": (0.95, 0.95, 0.9), "tf_slot": (0.7, 0.72, 0.75), "camera_fpc": (0.2, 0.2, 0.2)}
    mod = cq.Assembly(name="waveshare_esp32_s3_touch_lcd_2")
    for name, s in module.items():
        mod.add(s, name=name, color=cq.Color(*colours[name]))
    mod.save(os.path.join(OUT, "waveshare_module_envelope.step"))
    car = cq.Assembly(name="rs232_carrier")
    car.add(carrier["pcb"], name="pcb", color=cq.Color(0.1, 0.4, 0.2))
    car.add(carrier["parts_top"], name="parts_top", color=cq.Color(0.15, 0.15, 0.15))
    car.add(carrier["parts_bottom"], name="parts_bottom", color=cq.Color(0.15, 0.15, 0.15))
    car.save(os.path.join(OUT, "carrier_envelope.step"))
    asm = cq.Assembly(name="rs232_console")
    asm.add(body, name="body", color=cq.Color(0.9, 0.9, 0.88))
    asm.add(base, name="base", color=cq.Color(0.6, 0.6, 0.6))
    for name, k in key_parts.items():
        asm.add(k, name=f"key_{name}", color=cq.Color(0.35, 0.36, 0.38))
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
        full.add(vendor, name="waveshare_esp32_s3_touch_lcd_2_vendor")
        full.add(car, name="carrier")
        full.save(os.path.join(OUT, "rs232_console_assembly_vendor_module.step"))
    # meshes for the renderer
    shell, over = make_plug()
    extra = [("keys", keys), ("plug_shell", shell), ("plug_over", over)]
    for name, s in [("pcb_ws" if k == "pcb" else k, v) for k, v in module.items()] + list(carrier.items()) + extra:
        cq.exporters.export(s, os.path.join(OUT, f"_{name}.stl"), tolerance=0.02 if name in ("usbc", "buttons", "keys") else 0.03, angularTolerance=0.2)
    import json
    va = ws_dev(WS["glass"][0] + 3.06 + 30.99 / 2, WS["glass"][3] - 7.51 - 41.20 / 2)     # centre of the lit view area
    json.dump({"z_top": Z_TOP, "z_base_top": Z_BASE_TOP, "cy2": CY2, "view_area_centre": va, "outer": [bb.xlen, bb.ylen, bb.zlen],
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
