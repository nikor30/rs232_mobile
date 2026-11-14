#!/usr/bin/env python3
"""Raytraced pictures of the case (POV-Ray) from the meshes that case.py wrote to out/.

    python3 render.py            # writes out/render_*.png
    python3 render.py --hat      # the two-port variant, out_hat/

Device coordinates as in case.py: X across, Y from the RJ45 edge to the USB-C end, Z up.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HAT = "--hat" in sys.argv                       # the two-port variant: reads and writes out_hat/
OUT = os.path.join(HERE, "out_hat" if HAT else "out")

LCD = os.path.join(HERE, "lcd_status.png")     # what the display shows in the pictures


def stl_to_inc(name):
    """Binary or ASCII STL -> POV-Ray mesh include."""
    path = os.path.join(OUT, f"{name}.stl")
    data = open(path, "rb").read()
    tris = []
    if data[:5] == b"solid" and b"facet" in data[:400]:
        vs = []
        for line in data.decode("ascii", "replace").splitlines():
            t = line.split()
            if len(t) == 4 and t[0] == "vertex":
                vs.append(tuple(float(v) for v in t[1:]))
        tris = [vs[i:i + 3] for i in range(0, len(vs), 3)]
    else:
        n = struct.unpack("<I", data[80:84])[0]
        for i in range(n):
            f = struct.unpack("<12fH", data[84 + 50 * i:134 + 50 * i])
            tris.append([f[3:6], f[6:9], f[9:12]])
    inc = os.path.join(OUT, f"{name}.inc")
    with open(inc, "w") as fh:
        fh.write(f"#declare m_{name.strip('_')} = mesh {{\n")
        for a, b, c in tris:
            fh.write("triangle{<%.3f,%.3f,%.3f>,<%.3f,%.3f,%.3f>,<%.3f,%.3f,%.3f>}\n" % (*a, *b, *c))
        fh.write("inside_vector <0.3, 0.2, 1>\n}\n")     # closed meshes: usable in CSG, cut faces get capped
    return len(tris)


PARTS = ["body", "base", "_glass", "_tp_sensor", "_lcm", "_pmma", "_pcb_ws", "_headers", "_standoffs", "_usbc", "_buttons",
         "_bat_header", "_tf_slot", "_camera_fpc", "_pcb", "_parts_top", "_parts_bottom", "_keys", "_plug_shell", "_plug_over"]

SCENE = r'''
#version 3.7;
global_settings { assumed_gamma 1.0 max_trace_level 8
  radiosity { pretrace_start 0.08 pretrace_end 0.005 count 220 nearest_count 10 error_bound 0.3 recursion_limit 2 brightness 0.9 } }
#ifndef (View) #declare View = 1; #end
__INCLUDES__
background { rgbt <0, 0, 0, 1> }
sphere { <40, 55, 0>, 2500 hollow no_image no_shadow no_reflection
  texture { pigment { gradient z color_map { [0.0 rgb <0.05, 0.055, 0.07>] [0.3 rgb <0.10, 0.11, 0.14>] [1.0 rgb <0.30, 0.32, 0.38>] } scale 2500 } finish { emission 1.0 ambient 0 diffuse 0 } } }
light_source { <-260, 380, 420> color rgb <1.0, 0.98, 0.95> * 1.15 area_light <120, 0, 0>, <0, 120, 0>, 9, 9 adaptive 1 jitter circular orient }
light_source { <320, -80, 260> color rgb <0.85, 0.9, 1.0> * 0.5 area_light <80, 0, 0>, <0, 80, 0>, 5, 5 adaptive 1 jitter circular orient shadowless }
#if (View = 11)
light_source { <39, 150, 200> color rgb <1, 1, 1> * 0.8 area_light <60, 0, 0>, <0, 60, 0>, 5, 5 adaptive 1 jitter circular orient shadowless }
#end
#if (View = 6 | View = 8)
light_source { <40, 60, -300> color rgb <1, 1, 1> * 0.9 area_light <80, 0, 0>, <0, 80, 0>, 5, 5 adaptive 1 jitter circular orient }
#end

#declare T_body = texture { pigment { rgb <0.90, 0.895, 0.875> } finish { diffuse 0.72 specular 0.18 roughness 0.06 brilliance 1.2 } }
#declare T_base = texture { pigment { rgb <0.42, 0.43, 0.45> } finish { diffuse 0.7 specular 0.12 roughness 0.08 } }
#declare T_glass = texture { pigment { rgb <0.03, 0.03, 0.035> } finish { diffuse 0.3 specular 0.6 roughness 0.004 reflection { 0.08, 0.35 fresnel } } }
#declare T_dark = texture { pigment { rgb <0.10, 0.10, 0.11> } finish { diffuse 0.6 specular 0.15 roughness 0.05 } }
#declare T_fr4 = texture { pigment { rgb <0.10, 0.36, 0.20> } finish { diffuse 0.7 specular 0.25 roughness 0.02 } }
#declare T_ws = texture { pigment { rgb <0.07, 0.16, 0.42> } finish { diffuse 0.7 specular 0.25 roughness 0.02 } }
#declare T_metal = texture { pigment { rgb <0.75, 0.75, 0.78> } finish { specular 0.7 roughness 0.02 reflection { 0.2 metallic } } }
#declare T_gold = texture { pigment { rgb <0.85, 0.70, 0.30> } finish { specular 0.8 roughness 0.01 reflection { 0.2 metallic } } }
#declare T_white = texture { pigment { rgb <0.92, 0.92, 0.88> } finish { diffuse 0.75 } }
#declare T_clear = texture { pigment { rgbt <0.85, 0.9, 0.95, 0.5> } finish { diffuse 0.4 specular 0.5 roughness 0.01 } }

#declare Module = union {
  object { m_glass texture { T_glass } }
  // the lit view area (30.99 x 41.20, 7.51 from the antenna end), just above the glass surface
  box { <0, 0, 0>, <1, 1, 0.02>
    texture { pigment { image_map { png "__LCD__" once } } finish { emission 1.0 ambient 0 diffuse 0.15 specular 0.5 roughness 0.004 reflection { 0.05, 0.25 fresnel } } }
    translate <-0.5, -0.5, 0> rotate <0, 0, 180> scale <__VAW__, __VAL__, 1> translate <__VAX__, __VAY__, __ZTOP__> }
  object { m_tp_sensor texture { T_dark } }
  object { m_lcm texture { T_metal } }
  object { m_pmma texture { T_clear } }
  object { m_pcb_ws texture { T_ws } }
  object { m_headers texture { T_gold } }
  object { m_standoffs texture { T_gold } }
  object { m_usbc texture { T_metal } }
  object { m_buttons texture { T_white } }
  object { m_bat_header texture { T_white } }
  object { m_tf_slot texture { T_metal } }
  object { m_camera_fpc texture { T_dark } }
}
#declare Carrier = union {
  object { m_pcb texture { T_fr4 } }
  object { m_parts_top texture { T_dark } }
  object { m_parts_bottom texture { T_dark } }
__JACKS__
}
#declare Battery = box { <__BX1__, __BY1__, __BZ1__>, <__BX2__, __BY2__, __BZ2__> texture { pigment { rgb <0.20, 0.22, 0.28> } finish { diffuse 0.6 specular 0.3 roughness 0.02 } } }
#declare Body = object { m_body texture { T_body } }
#declare Keys = object { m_keys texture { pigment { rgb <0.30, 0.31, 0.33> } finish { diffuse 0.7 specular 0.2 roughness 0.05 } } }
#declare Plug = union {
  object { m_plug_shell texture { T_metal } }
  object { m_plug_over texture { pigment { rgb <0.93, 0.93, 0.92> } finish { diffuse 0.75 specular 0.2 roughness 0.04 } } }
}
#declare Base = object { m_base texture { T_base } }
__REST_DECL__
// The camera positions are written for the four-port case (centre 39, 57.4; USB-C end at y = 116.73).
// A variant of another size moves and scales them: Cam about its centre, CamU with its USB-C end.
#declare CS = __CS__; #declare CCX = __CCX__; #declare CCY = __CCY__; #declare UY = __UY__;
#macro Cam(ex, ey, ez, lx, ly, lz, ang)
  camera { perspective location <CCX + (ex - 39) * CS, CCY + (ey - 57.4) * CS, ez * CS> look_at <CCX + (lx - 39) * CS, CCY + (ly - 57.4) * CS, lz>
           sky <0, 0, 1> right <-4/3, 0, 0> angle ang }
#end
#macro CamU(ex, ey, ez, lx, ly, lz, ang)
  camera { perspective location <CCX + ex - 39, UY + ey - 116.73, ez> look_at <CCX + lx - 39, UY + ly - 116.73, lz> sky <0, 0, 1> right <-4/3, 0, 0> angle ang }
#end
#switch (View)
#case (1)   // hero: USB-C end towards the viewer
  Cam(-85, 300, 150, 40, 62, 0, 30)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys }
#break
#case (2)   // port end
  Cam(150, -200, 110, 40, 45, 2, 30)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys }
#break
#case (3)   // exploded
  #declare CS = (CS + 1) / 2;
  Cam(-175, 345, 185, 40, 58, 0, 36)
  object { Module translate <0, 0, 62> }
  object { Body translate <0, 0, 30> }
  object { Keys translate <0, 14, 30> }
  __REST_UP__
  object { Carrier }
  object { Battery translate <0, 0, -24> }
  object { Base translate <0, 0, -34> }
#break
#case (4)   // section along the middle of the display
  Cam(-260, 150, 70, 40, 60, 3, 26)
  #declare Cut = plane { x, CCX inverse }
  __REST__
  intersection { object { Body } object { Cut } cutaway_textures }
  intersection { object { Base } object { Cut } cutaway_textures }
  intersection { object { Module } object { Cut } cutaway_textures }
  intersection { object { Carrier } object { Cut } cutaway_textures }
  intersection { object { Battery } object { Cut } cutaway_textures }
#break
#case (5)   // the base with its snap arms
  Cam(-120, 250, 150, 40, 56, -10, 32)
  object { Base } object { Battery }
#break
#case (6)   // from below
  Cam(-110, 260, -170, 40, 58, 0, 32)
  object { Body } object { Base } object { Keys }
#break
#case (7)   // the electronics without the case: carrier with the module plugged in
  Cam(-150, 300, 175, 40, 55, 6, 30)
  object { Module } object { Carrier } __REST__
#break
#case (8)   // the carrier from below: relays, battery connectors, protection diodes
  Cam(-130, 270, -190, 40, 55, 0, 30)
  object { Carrier }
#break
#case (9)   // the USB-C end: bay, port, the two keys
  CamU(8, 232, 52, 39, 116, 9.5, 24)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys }
#break
#case (10)  // the same with a plug in (overmould of the largest size the USB-IF allows)
  CamU(8, 232, 52, 39, 122, 9.5, 24)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Plug }
#break
#case (11)  // cut open level with the keys: receptacle, switches, keys in their tunnels
  CamU(39, 138, 86, 39, 108.5, 10, 30)
  #declare Cut = plane { z, __CUTZ__ }
  intersection { object { Body } object { Cut } cutaway_textures }
  intersection { object { Module } object { Cut } cutaway_textures }
  intersection { object { Keys } object { Cut } cutaway_textures }
  object { Carrier }
#break
#end
'''


def main():
    import shutil
    if HAT and "_rest" not in PARTS:
        PARTS.append("_rest")
    for n in PARTS:
        if n == "_pcb_ws":
            continue
        if not os.path.exists(os.path.join(OUT, f"{n}.stl")):
            print("missing", n, "- run case.py first")
            return 1
    inc = []
    for n in PARTS:
        src = "_pcb_ws" if n == "_pcb_ws" else n
        if n == "_pcb_ws":
            if not os.path.exists(os.path.join(OUT, "_pcb_ws.stl")):
                print("missing _pcb_ws.stl - run case.py first")
                return 1
        stl_to_inc(src)
        inc.append(f'#include "{src}.inc"')
    import json
    geo = json.load(open(os.path.join(OUT, "geometry.json")))
    cx, cy = geo["view_area_centre"]
    jacks = []
    jf = geo["jack_front"]                      # the jack fronts sit this far behind the board edge
    for xc in geo["jacks"]:
        jacks.append(f"  box {{ <{xc - 5.8}, {jf - 0.05 if not HAT else jf + 13.0}, 2.7>, <{xc + 5.8}, {jf + 6.0 if not HAT else jf + 13.9}, 10.7> texture {{ pigment {{ rgb <0.02, 0.02, 0.02> }} finish {{ diffuse 0.3 }} }} }}")
        for i in range(8):
            jacks.append(f"  box {{ <{xc - 4.5 + i * 1.27:.2f}, {jf - 0.08 if not HAT else jf + 1.0}, 9.0>, <{xc - 4.1 + i * 1.27:.2f}, {jf + 0.5 if not HAT else jf + 9.0}, {10.4 if not HAT else 9.4}> texture {{ T_gold }} }}")
    bx1, by1, bx2, by2 = geo["battery_rect"]
    bx1, bx2, by1 = bx1 + 1.0, bx2 - 1.0, by1 + 1.5
    by2 = by1 + geo["battery"][1] - 2.0
    scene = (SCENE.replace("__INCLUDES__", "\n".join(inc)).replace("__LCD__", LCD)
             .replace("__VAW__", "30.99").replace("__VAL__", "41.20").replace("__VAX__", f"{cx:.3f}").replace("__VAY__", f"{cy:.3f}")
             .replace("__ZTOP__", f"{geo['z_top']:.3f}").replace("__JACKS__", "\n".join(jacks))
             .replace("__BX1__", f"{bx1}").replace("__BX2__", f"{bx2}").replace("__BY1__", f"{by1:.2f}").replace("__BY2__", f"{by2:.2f}")
             .replace("__CS__", f"{(geo['oy'][1] - geo['oy'][0]) / 118.63:.3f}" if HAT else "1").replace("__CCX__", f"{geo['ws_xc']}")
             .replace("__CCY__", f"{(geo['oy'][0] + geo['oy'][1]) / 2:.2f}").replace("__UY__", f"{geo['oy'][1]:.2f}")
             .replace("__REST_DECL__", "#declare Rest = object { m_rest texture { pigment { rgb <0.30, 0.31, 0.33> } finish { diffuse 0.7 specular 0.2 roughness 0.05 } } }" if HAT else "")
             .replace("__REST_UP__", "object { Rest translate <0, 0, 16> }" if HAT else "").replace("__REST__", "object { Rest }" if HAT else "")
             .replace("__CUTZ__", f"{geo['key']['z'] - 0.15:.2f}")
             .replace("__BZ1__", f"{geo['z_base_top'] + 0.1:.2f}").replace("__BZ2__", f"{geo['z_base_top'] + 5.1:.2f}"))
    pov = os.path.join(OUT, "scene.pov")
    open(pov, "w").write(scene)
    names = {1: "hero", 2: "ports", 3: "exploded", 4: "section", 5: "base", 6: "below", 7: "electronics", 8: "carrier_bottom",
             9: "usb_end", 10: "usb_plug", 11: "usb_cut"}
    if HAT:
        names[7], names[8] = "electronics", "board_bottom"
    want = [int(a) for a in sys.argv[1:] if a.isdigit()] or list(names)
    ok = True
    for v in want:
        out = os.path.join(OUT, f"render_{names[v]}.png")
        r = subprocess.run(["povray", "+Iscene.pov", f"+O{out}", "+W1600", "+H1200", "+A0.2", "+AM2", "+R3", "+J0", "+UA", "-D", "-V", "+Q9",
                            f"Declare=View={v}"], cwd=OUT, capture_output=True, text=True)
        good = r.returncode == 0 and os.path.exists(out)
        ok &= good
        print(names[v], "ok" if good else r.stderr[-600:])
    details(geo)
    if geo.get("hat"):
        hat_layout(geo)
    return 0 if ok else 1


def hat_layout(geo):
    """Two-port variant: where board, module, posts and battery sit - plan and lengthwise section, with dimensions."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Circle, FancyBboxPatch, Rectangle
    h, P, z = geo["hat"], geo["P"], geo["z"]
    ox1, oy1, ox2, oy2 = h["outer"]
    cx1, cy1, cx2, cy2 = h["cavity"]
    red = "#b3261e"

    def dim(ax, p1, p2, text, off, side="h", fs=8):
        (x1, y1), (x2, y2) = p1, p2
        if side == "h":
            y = y1 + off
            for x, yy in ((x1, y1), (x2, y2)):
                ax.plot([x, x], [yy, y], color=red, lw=0.4)
            ax.annotate("", (x1, y), (x2, y), arrowprops=dict(arrowstyle="<->", color=red, lw=0.7, shrinkA=0, shrinkB=0))
            ax.text((x1 + x2) / 2, y + (0.5 if off >= 0 else -0.5), text, ha="center", va="bottom" if off >= 0 else "top", fontsize=fs, color=red)
        else:
            x = x1 + off
            for xx, y in ((x1, y1), (x2, y2)):
                ax.plot([xx, x], [y, y], color=red, lw=0.4)
            ax.annotate("", (x, y1), (x, y2), arrowprops=dict(arrowstyle="<->", color=red, lw=0.7, shrinkA=0, shrinkB=0))
            ax.text(x + (0.6 if off >= 0 else -0.6), (y1 + y2) / 2, text, ha="left" if off >= 0 else "right", va="center", fontsize=fs, color=red, rotation=90)

    fig = plt.figure(figsize=(15, 9.6))
    a1 = fig.add_axes([0.03, 0.05, 0.42, 0.88]); a2 = fig.add_axes([0.50, 0.30, 0.48, 0.58])
    # ---------------- plan, seen from above, port end at the bottom
    r = P["r_out"]
    a1.add_patch(FancyBboxPatch((ox1 + r, oy1 + r), ox2 - ox1 - 2 * r, oy2 - oy1 - 2 * r, boxstyle=f"round,pad={r}", fc="#ecebe7", ec="k", lw=1.2))
    a1.add_patch(Rectangle((cx1, cy1), cx2 - cx1, cy2 - cy1, fc="white", ec="#777", lw=0.6))
    b1, b2, b3, b4 = geo["battery_rect"]
    a1.add_patch(Rectangle((0, 0), h["W"], h["H"], fc="#2e7d32", ec="k", lw=1.0, alpha=0.8))
    for x1, y1, x2, y2 in h["jacks"]:
        a1.add_patch(Rectangle((x1, y1), x2 - x1, y2 - y1, fc="#cfd2d6", ec="k", lw=0.8))
    a1.text(geo["jacks"][0], 9, "PORT 1", ha="center", fontsize=8); a1.text(geo["jacks"][1], 9, "PORT 2", ha="center", fontsize=8)
    for name, x1, y1, x2, y2, hh in h["parts"]:
        a1.add_patch(Rectangle((x1, y1), x2 - x1, y2 - y1, fc="#222" if name.startswith("U") else "#b7a07a", ec="k", lw=0.4))
    for x, y in h["j3"]:
        a1.add_patch(Circle((x, y), 0.8, fc="#d8c36a", ec="k", lw=0.4))
    a1.text(h["j3"][0][0] - 1.5, h["j3"][0][1], "J3", ha="right", va="center", fontsize=7, color="w")
    for x, y in h["holes"]:
        a1.add_patch(Circle((x, y), P["post_d"] / 2, fc="#ecebe7", ec="k", lw=0.7))
        a1.add_patch(Circle((x, y), h["hole_d"] / 2 - 0.15, fc="#8f9094", ec="k", lw=0.4))
    m = h["module_pcb"]; g = h["glass"]
    a1.add_patch(Rectangle((m[0], m[1]), m[2] - m[0], m[3] - m[1], fc="none", ec="#1f3b73", lw=1.2, ls="--"))
    a1.add_patch(FancyBboxPatch((g[0] + 2.6, g[1] + 2.6), g[2] - g[0] - 5.2, g[3] - g[1] - 5.2, boxstyle="round,pad=2.6", fc="none", ec="k", lw=1.0, ls=":"))
    for x in h["hdr_x"]:
        for i in range(14):
            a1.add_patch(Circle((x, h["hdr_y"][0] + i * 2.54), 0.35, fc="#c9a227", ec="none"))
    for x, y in h["standoffs_base"]:
        a1.add_patch(Circle((x, y), 2.25, fc="#8f9094", ec="k", lw=0.7))
    sr = h["standoffs_rest"]
    xs = sorted(q[0] for q in sr)
    a1.add_patch(Rectangle((xs[0] - 3.95, sr[0][1] - h["rest_t"] / 2), xs[1] - xs[0] + 7.9, h["rest_t"], fc="#8f9094", ec="k", lw=0.7))
    for x, y in sr:
        a1.add_patch(Circle((x, y), 1.75, fc="none", ec="#7a5c00", lw=0.9))
    a1.add_patch(Rectangle((b1, b2), b3 - b1, b4 - b2, fc="none", ec="#555", lw=0.9, ls="-."))
    a1.text((b1 + b3) / 2, b4 - 9, f"battery below the board\nup to {geo['battery'][0]:.0f} x {geo['battery'][1]:.0f} x 7", ha="center", fontsize=7.5, color="#333")
    a1.annotate("module PCB (dashed)\nglass (dotted)", (m[2], m[3] - 12), (ox2 + 3, m[3] - 13), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5, color="#1f3b73")
    a1.annotate("module rest: stands on the board,\ncarries two standoffs", (xs[1] + 2, sr[0][1]), (ox2 + 3, sr[0][1] + 3), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a1.annotate("posts on the base carry\nthe other two standoffs", (h["standoffs_base"][1][0] + 2, h["standoffs_base"][1][1] - 1), (ox2 + 3, h["standoffs_base"][1][1] - 19), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a1.annotate("four posts with pins\nin the board's holes", (h["holes"][-1][0], min(q[1] for q in h["holes"])), (ox2 + 3, 0), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a1.text((ox1 + ox2) / 2, oy2 + 1.2, "USB-C, RST, BOOT", ha="center", fontsize=8)
    dim(a1, (ox1, oy1), (ox2, oy1), f"{ox2 - ox1:.1f}", off=-9)
    dim(a1, (ox1, oy1), (ox1, oy2), f"{oy2 - oy1:.1f}", off=-7, side="v")
    dim(a1, (0, 0), (h["W"], 0), f"board {h['W']:.0f}", off=-4.5)
    dim(a1, (h["W"], 0), (h["W"], h["H"]), f"board {h['H']:.0f}", off=6.5, side="v")
    dim(a1, (geo["jacks"][0], 14.0), (geo["jacks"][1], 14.0), f"{geo['jacks'][1] - geo['jacks'][0]:.1f}", off=0.0, fs=7)
    a1.annotate(f"glass ends {g[3] - h['H']:.1f}\npast the board", (g[2] - 1, g[3]), (ox2 + 3, g[3] - 3), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a1.set_xlim(ox1 - 12, ox2 + 34); a1.set_ylim(oy1 - 13, oy2 + 5); a1.set_aspect("equal"); a1.axis("off")
    a1.set_title("Two-port case from above (mm)", fontsize=11)
    # ---------------- lengthwise section through a row of standoffs
    grey, part = "#d9d7d2", "#8f9094"
    a2.add_patch(Rectangle((oy1, z["bot"]), cy1 - oy1, z["top"] - z["bot"], fc=grey, ec="k", lw=0.8))                    # port wall
    a2.add_patch(Rectangle((cy2, z["bot"]), oy2 - cy2, z["top"] - z["bot"], fc=grey, ec="k", lw=0.8))                    # USB-C wall
    a2.add_patch(Rectangle((cy1, z["plate"]), g[1] - P["glass_clr"] - cy1, z["top"] - z["plate"], fc=grey, ec="k", lw=0.8))   # top over the jacks
    a2.add_patch(Rectangle((g[1] - P["glass_clr"], z["plate"]), m[1] - 0.3 - g[1] + P["glass_clr"], z["glass"] - z["plate"], fc=grey, ec="k", lw=0.8))   # ledge
    a2.add_patch(Rectangle((cy1 + P["base_clr"], z["base_bot"]), cy2 - cy1 - 2 * P["base_clr"], z["base_top"] - z["base_bot"], fc=part, ec="k", lw=0.8))
    a2.add_patch(Rectangle((b2 + 1.5, z["base_top"] + 0.1), geo["battery"][1] - 2, 5.0, fc="#3a4254", ec="k", lw=0.6))
    a2.text(b2 + geo["battery"][1] / 2, z["base_top"] + 2.6, "battery", ha="center", va="center", fontsize=8, color="w")
    a2.add_patch(Rectangle((0, -1.6), h["H"], 1.6, fc="#2e7d32", ec="k", lw=0.8))
    j = h["jacks"][0]
    a2.add_patch(Rectangle((j[1], 0), j[3] - j[1], geo["jack_h"], fc="#cfd2d6", ec="k", lw=0.8))
    a2.text((j[1] + j[3]) / 2, geo["jack_h"] / 2, "RJ45", ha="center", va="center", fontsize=8)
    for y in (j[1] + 14.0, j[1] + 16.5):
        a2.add_patch(Rectangle((y, -1.6 - h["pin_below"]), 0.5, h["pin_below"], fc="#c9a227", ec="none"))
    hy = sorted(q[1] for q in h["holes"])
    for y in (hy[0], hy[-1]):
        a2.add_patch(Rectangle((y - 2.5, 0), 5.0, z["plate"], fc=grey, ec="k", lw=0.6, alpha=0.75))
        a2.add_patch(Rectangle((y - 2.5, z["base_top"]), 5.0, -1.6 - z["base_top"], fc=part, ec="k", lw=0.6, alpha=0.75))
        a2.add_patch(Rectangle((y - 1.2, -3.6), 2.4, 3.6, fc=grey, ec="k", lw=0.5))
    sy, sz = sr[0][1], h["standoff_z"]
    a2.add_patch(Rectangle((sy - h["rest_t"] / 2, 0), h["rest_t"], sz - P["rest_gap"], fc=part, ec="k", lw=0.7))
    by_ = h["standoffs_base"][0][1]
    a2.add_patch(Rectangle((by_ - 2.25, z["base_top"]), 4.5, sz - P["rest_gap"] - z["base_top"], fc=part, ec="k", lw=0.7))
    for y in (sy, by_):
        a2.add_patch(Rectangle((y - 1.75, sz), 3.5, 4.0, fc="#c9a227", ec="k", lw=0.5))
    a2.add_patch(Rectangle((m[1], h["pcb_z"][0]), m[3] - m[1], 1.6, fc="#1f3b73", ec="k", lw=0.8))
    a2.add_patch(Rectangle((m[1] + 0.2, h["pcb_z"][1]), m[3] - m[1] - 0.4, z["glass"] - h["pcb_z"][1], fc="#b9babd", ec="k", lw=0.6))
    a2.add_patch(Rectangle((g[1], z["glass"]), g[3] - g[1], z["top"] - z["glass"], fc="#1c1d22", ec="k", lw=0.8))
    for i in range(14):
        a2.add_patch(Rectangle((h["hdr_y"][0] + i * 2.54 - 0.3, h["pin_tip_z"]), 0.6, h["pcb_z"][0] - h["pin_tip_z"], fc="#c9a227", ec="none"))
    a2.text((m[1] + m[3]) / 2, (h["pcb_z"][1] + z["glass"]) / 2, "display", ha="center", va="center", fontsize=8)
    zt, zb = z["top"] + 4.2, z["bot"] - 8.5
    a2.annotate("module rest", (sy, 4.0), (sy - 5, zt), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a2.annotate("post under a standoff", (by_, 0), (by_ - 16, zt), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a2.annotate(f"jack front {j[1] - oy1:.1f} behind the outer face", (j[1], 5), (oy1 - 10, zt), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a2.annotate(f"pin tips {h['pin_tip_z']:.1f} above the board", (h["hdr_y"][0] + 9 * 2.54, h["pin_tip_z"]), (h["hdr_y"][0] + 12, zb), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    a2.annotate("pin of the body post in the board's hole,\nits end in the post of the base", (hy[-1], -2.6), (oy1 - 10, zb - 1.2), arrowprops=dict(arrowstyle="->", lw=0.6), fontsize=7.5)
    dim(a2, (oy1, z["bot"]), (oy1, z["top"]), f"{z['top'] - z['bot']:.1f}", off=-4, side="v")
    dim(a2, (oy1, z["bot"]), (oy2, z["bot"]), f"{oy2 - oy1:.1f}", off=-3.0)
    dim(a2, (j[3], geo["jack_h"]), (j[3], z["plate"]), f"{z['plate'] - geo['jack_h']:.1f}", off=3.5, side="v", fs=7)
    a2.set_xlim(oy1 - 12, oy2 + 6); a2.set_ylim(zb - 4, zt + 3); a2.set_aspect("equal"); a2.axis("off")
    a2.set_title("Section along the length, through one row of standoffs (port end on the left)", fontsize=11)
    fig.text(0.50, 0.21, "Assembly\n1. Solder the six wires from J3 to the module's pins.\n2. Both keys into their tunnels, module into the body from above, body face down.\n"
                         "3. Module rest onto the two standoffs next to the jacks.\n4. Board onto the four pins, its parts towards the module.\n5. Battery, then press the base in.",
             fontsize=9.5, va="top", linespacing=1.5)
    fig.savefig(os.path.join(OUT, "hat_layout.png"), dpi=125)


def details(geo):
    """Two dimensioned cross-sections: the snap arm in its pocket and the glass on its ledge."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle
    P, z = geo["P"], geo["z"]
    fig, ((a1, a2), (a3, a4)) = plt.subplots(2, 2, figsize=(14, 13.5))
    # --- snap: section across the left wall, x = distance from the wall's inner face (positive into the cavity)
    wall = [(-P["wall"], z["bot"]), (0, z["bot"]), (0, z["bot"] + 6.5), (-P["wall"], z["bot"] + 6.5)]
    a1.add_patch(Polygon(wall, closed=True, fc="#d9d7d2", ec="k", lw=1))
    rec = [(-r, zz) for r, zz in geo["recess_profile"]]
    a1.add_patch(Polygon(rec, closed=True, fc="white", ec="k", lw=1))
    a1.add_patch(Polygon([(0.01, z["bot"]), (-0.9, z["bot"]), (0.01, z["bot"] + 0.9)], closed=True, fc="white", ec="k", lw=1))
    c = P["base_clr"]
    arm = [(c, z["base_bot"]), (c + P["arm_t"], z["base_bot"]), (c + P["arm_t"], z["base_bot"] + P["arm_h"]), (c, z["base_bot"] + P["arm_h"])]
    a1.add_patch(Polygon(arm, closed=True, fc="#8f9094", ec="k", lw=1))
    bump = [(c - r, zz) for r, zz in geo["bump_profile"]]
    a1.add_patch(Polygon(bump, closed=True, fc="#8f9094", ec="k", lw=1))
    plate = [(c + P["arm_t"] + P["arm_slot"], z["base_bot"]), (9, z["base_bot"]), (9, z["base_top"]), (c + P["arm_t"] + P["arm_slot"], z["base_top"])]
    a1.add_patch(Polygon(plate, closed=True, fc="#8f9094", ec="k", lw=1))
    a1.annotate("", (c - P["bump"], z["bump"] + 1.3), (c, z["bump"] + 1.3), arrowprops=dict(arrowstyle="<->"))
    a1.text(c + P["arm_t"] + 0.2, z["bump"] + 1.2, f"catch {P['bump']:.1f}", ha="left", fontsize=8)
    a1.text(-P["wall"] / 2 - 0.2, z["bot"] + 5.6, f"wall {P['wall']:.1f}", ha="center", fontsize=8)
    a1.text(c + P["arm_t"] / 2, z["base_bot"] + P["arm_h"] + 0.25, f"arm {P['arm_t']:.1f} x {P['arm_h']:.1f}\n{P['arm_l']:.0f} long, flexes in plane", ha="center", fontsize=8)
    a1.text(c + P["arm_t"] + P["arm_slot"] / 2, z["base_bot"] + 0.9, f"slot\n{P['arm_slot']:.1f}", ha="center", fontsize=7)
    a1.text(5.7, (z["base_bot"] + z["base_top"]) / 2, "base plate", ha="center", va="center", fontsize=8)
    a1.annotate("45 deg: holds, releases when pried", (-0.35, z["bump"] - 0.35), (-4.0, z["bot"] - 0.55), arrowprops=dict(arrowstyle="->"), fontsize=7)
    a1.annotate(f"{P['lead_deg']:.0f} deg lead-in", (-0.3, z["bump"] + 1.9), (-4.0, z["bot"] + 7.0), arrowprops=dict(arrowstyle="->"), fontsize=7)
    a1.annotate("push in", (4.5, z["base_bot"] + 4.2), (4.5, z["base_bot"] + 2.4), arrowprops=dict(arrowstyle="->"), ha="center", fontsize=8)
    a1.set_xlim(-4.2, 9.5); a1.set_ylim(z["bot"] - 0.8, z["bot"] + 7.4); a1.set_aspect("equal")
    a1.set_title("Snap arm in its wall pocket (section, mm)"); a1.set_xlabel("from the wall's inner face"); a1.set_ylabel("z")
    # --- glass seat: section across the width at the middle of the display
    zm, ws = geo["zm"], geo["ws"]
    gx1, gx2 = geo["glass_x"]; ox1, ox2 = geo["open_x"]; g = P["glass_clr"]
    xl = gx1 - 6
    body = [(xl, z["plate"]), (ox1, z["plate"]), (ox1, z["glass"]), (gx1 - g, z["glass"]), (gx1 - g, z["top"]), (xl, z["top"])]
    a2.add_patch(Polygon(body, closed=True, fc="#d9d7d2", ec="k", lw=1))
    xc = (gx1 + gx2) / 2
    xr = gx1 + 12
    def layer(key, zkey, colour, label):
        b, zz = ws[key], ws[zkey]
        x1 = xc - (b[2] - b[0]) / 2
        a2.add_patch(Rectangle((x1, zz[0] + zm), xr - x1, zz[1] - zz[0], fc=colour, ec="k", lw=0.8))
        a2.text(gx1 + 5.2, (zz[0] + zz[1]) / 2 + zm, label, va="center", fontsize=7.5)
    layer("glass", "glass_z", "#1c1d22", "")
    a2.text(gx1 + 5.2, (ws["glass_z"][0] + ws["glass_z"][1]) / 2 + zm, "cover glass 1.10", va="center", fontsize=7.5, color="white")
    layer("tp_sensor", "tp_sensor_z", "#55565c", "")
    layer("lcm", "lcm_z", "#b9babd", "LCD 2.1")
    layer("pmma", "pmma_z", "#dfe8ef", "spacer 2.0")
    layer("pcb", "pcb_z", "#1f3b73", "")
    a2.text(gx1 + 5.2, (ws["pcb_z"][0] + ws["pcb_z"][1]) / 2 + zm, "module PCB 1.6", va="center", fontsize=7.5, color="white")

    a2.annotate(f"ledge {ox1 - gx1:.2f}", ((ox1 + gx1) / 2, z["glass"]), (gx1 - 5.2, z["plate"] - 1.6), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.text(xl + 2.6, z["top"] + 0.25, "top of the case = top of the glass", fontsize=8)
    a2.text(xl + 0.3, (z["plate"] + z["top"]) / 2, f"top plate {P['top_t']:.1f}", fontsize=8, va="center")
    a2.annotate(f"gap {g:.2f}", (gx1 - g / 2, z["top"]), (gx1 - 3.2, z["top"] + 1.1), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.set_xlim(xl - 0.5, gx1 + 12); a2.set_ylim(z["plate"] - 5.2, z["top"] + 1.8); a2.set_aspect("equal")
    a2.set_title("Display glass on its ledge, flush with the top (section across the width, mm)"); a2.set_xlabel("x"); a2.set_ylabel("z above the carrier")
    # --- a key: section along its axis (y to the left = outside, as seen from the BOOT side)
    k, u = geo["key"], geo["usb"]
    oy2, cy2, bay = u["oy2"], u["cy2"], u["bay_y"]
    kz, (tz1, tz2) = k["z"], k["tunnel_z"]
    hk = P["key_h"] / 2 + P["key_clr"]
    grey, part = "#d9d7d2", "#8f9094"
    # end wall (bay floor), above and below the cap hole
    a3.add_patch(Polygon([(bay, kz + hk), (cy2, kz + hk), (cy2, z["plate"]), (k["stop_y"], z["plate"]), (k["stop_y"], z["glass"]),
                          (k["glass_y"] + P["glass_clr"], z["glass"]), (k["glass_y"] + P["glass_clr"], z["top"]), (oy2 - 1.5, z["top"]), (oy2, z["top"] - 1.5),
                          (oy2, u["bay_z"] + P["bay_h"] / 2 + P["bay_depth"]), (bay, u["bay_z"] + P["bay_h"] / 2)], closed=True, fc=grey, ec="k", lw=1))
    a3.add_patch(Polygon([(bay, kz - hk), (cy2, kz - hk), (cy2, tz1 - 9), (oy2, tz1 - 9), (oy2, u["bay_z"] - P["bay_h"] / 2 - P["bay_depth"]),
                          (bay, u["bay_z"] - P["bay_h"] / 2)], closed=True, fc=grey, ec="k", lw=1))
    # tunnel block: roof up to the plate, floor
    a3.add_patch(Rectangle((k["stop_y"], tz2), cy2 - k["stop_y"], z["plate"] - tz2, fc=grey, ec="k", lw=1))
    a3.add_patch(Rectangle((k["stop_y"], tz1 - 1.2), cy2 - k["stop_y"], 1.2, fc=grey, ec="k", lw=1))
    a3.add_patch(Polygon(k["profile"], closed=True, fc=part, ec="k", lw=1))
    # module: glass, LCD stack, PCB, switch with plunger
    ws, zm = geo["ws"], geo["zm"]
    pe = k["pcb_edge_y"]
    yl = pe - 7.5
    a3.add_patch(Rectangle((yl, ws["glass_z"][0] + zm), k["glass_y"] - yl, 1.1, fc="#1c1d22", ec="k", lw=0.8))
    a3.add_patch(Rectangle((yl, ws["lcm_z"][0] + zm), pe - 0.01 - yl, 2.1, fc="#b9babd", ec="k", lw=0.8))
    a3.add_patch(Rectangle((yl, ws["pmma_z"][0] + zm), pe - 0.21 - yl, 2.0, fc="#dfe8ef", ec="k", lw=0.8))
    a3.add_patch(Rectangle((yl, k["pcb_z"][0]), pe - yl, 1.6, fc="#1f3b73", ec="k", lw=0.8))
    aw, ah, ap = k["act"]
    a3.add_patch(Rectangle((k["tip_y"] - ap - k["body_d"], k["body_z"][0]), k["body_d"], k["body_z"][1] - k["body_z"][0], fc="#f1efe6", ec="k", lw=0.8))
    a3.add_patch(Rectangle((k["tip_y"] - ap, kz - ah / 2), ap, ah, fc="#3a3a3d", ec="k", lw=0.8))
    a3.text(pe - 0.6, k["pcb_z"][0] + 0.8, "module PCB", fontsize=7.5, color="w", va="center", ha="left")
    a3.text(k["tip_y"] - ap - k["body_d"] / 2, k["body_z"][0] - 0.5, "switch\nTS24CA", fontsize=7, ha="center", va="top")
    a3.text((k["stop_y"] + cy2) / 2 + 0.3, kz + 2.4, "key", fontsize=8, ha="center")
    a3.text((k["stop_y"] + cy2) / 2, tz1 - 0.6, "tunnel", fontsize=7, ha="center", va="center")
    a3.text((k["stop_y"] + cy2) / 2, z["plate"] + 0.95, "top plate", fontsize=7.5, ha="center", va="center")
    a3.text(oy2 - 1.2, tz1 - 6.5, "end wall", fontsize=7.5, ha="center", rotation=90)
    tip = max(y for y, _ in k["profile"])
    xl = oy2 + 9.0
    a3.annotate(f"cap {P['key_proud']:.1f} above the bay floor,\n{oy2 - tip:.1f} below the outer face", (tip, kz + 2.0), (xl, kz + 5.0), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a3.annotate("shoulders: the key\ncannot leave outwards", (cy2 - 0.15, kz - P["key_h"] / 2 - 0.5), (xl, kz - 6.0), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a3.annotate(f"gap {P['key_gap']:.2f}, then the plunger\n(2.0 x 1.0, 1.2 proud of the switch)", (k["tip_y"] + P["key_gap"] / 2, kz - 0.4), (cy2 - 1.0, tz1 - 6.0),
                arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a3.annotate("", (oy2 + 0.5, kz), (oy2 + 3.2, kz), arrowprops=dict(arrowstyle="->", lw=1.4))
    a3.text(oy2 + 3.5, kz, f"press, stroke up to {P['key_stroke']:.1f}", fontsize=8, va="center", ha="right")
    a3.set_xlim(oy2 + 9.5, yl - 0.5); a3.set_ylim(tz1 - 9.5, z["top"] + 1.2); a3.set_aspect("equal")
    a3.set_title("Key for RST / BOOT: section along the key (mm)"); a3.set_xlabel("y (outside on the left)"); a3.set_ylabel("z above the carrier")
    # --- USB-C: plan section on the axis of the receptacle, plug fully in
    ux, plug = u["x"], u["plug"]
    hw = P["usb_w"] / 2
    for sx in (-1, 1):
        x_bay_out, x_bay = ux + sx * (P["bay_w"] / 2 + P["bay_depth"]), ux + sx * P["bay_w"] / 2
        xe = ux + sx * 17.5
        a4.add_patch(Polygon([(ux + sx * hw, bay), (x_bay, bay), (x_bay_out, oy2), (xe, oy2), (xe, cy2), (ux + sx * hw, cy2)], closed=True, fc=grey, ec="k", lw=1))
        # tunnel block beside the well, with the key in it
        kx = k["x"]["RST"] if sx < 0 else k["x"]["BOOT"]
        for a, b in ((kx - 1.65 - 1.1, kx - 1.65), (kx + 1.65, kx + 1.65 + 1.1)):
            a4.add_patch(Rectangle((a, k["stop_y"]), b - a, cy2 - k["stop_y"], fc=grey, ec="k", lw=1))
        a4.add_patch(Rectangle((kx - 1.5, k["tip_y"] + P["key_gap"]), 3.0, tip - k["tip_y"] - P["key_gap"], fc=part, ec="k", lw=1))
        a4.add_patch(Rectangle((kx - k["act"][0] / 2, k["tip_y"] - ap), k["act"][0], ap, fc="#3a3a3d", ec="k", lw=0.8))
        a4.add_patch(Rectangle((kx - 2.275, k["tip_y"] - ap - k["body_d"]), 4.55, k["body_d"], fc="#f1efe6", ec="k", lw=0.8))
        # receptacle shell walls
        so, oo = u["shell"][0] / 2, u["open"][0] / 2
        a4.add_patch(Rectangle((ux + sx * oo if sx > 0 else ux - so, u["rear_y"]), so - oo, u["rim_y"] - u["rear_y"], fc="#b9babd", ec="k", lw=0.8))
        # plug: shell walls and overmould
        ps = plug["shell_w"] / 2
        a4.add_patch(Rectangle((ux + sx * ps - (0.3 if sx > 0 else 0), u["back_y"]), 0.3, plug["shell_l"], fc="#e3c14f", ec="k", lw=0.6))
    a4.add_patch(Rectangle((ux - u["open"][0] / 2, u["rear_y"]), u["open"][0], u["back_y"] - u["rear_y"], fc="#55565c", ec="k", lw=0.8))
    a4.add_patch(Rectangle((ux - u["tongue"][0] / 2, u["back_y"]), u["tongue"][0], u["tongue_tip_y"] - u["back_y"], fc="#2a2a2d", ec="k", lw=0.8))
    over_y = u["back_y"] + plug["shell_l"]
    a4.add_patch(Rectangle((ux - plug["over_w"] / 2, over_y), plug["over_w"], 9.0, fc="#f4f4f1", ec="k", lw=1))
    a4.plot([ux - plug["shell_w"] / 2, ux + plug["shell_w"] / 2], [u["back_y"], u["back_y"]], color="#e3c14f", lw=1.2)
    a4.text(ux, over_y + 4.6, f"plug overmould\n{plug['over_w']:.2f} x {plug['over_h']:.1f} (largest allowed)", ha="center", va="center", fontsize=7.5)
    a4.text(ux, u["back_y"] - 1.3, "receptacle", ha="center", va="center", fontsize=7.5, color="w")
    a4.plot([ux - 17.5, ux + 17.5], [pe, pe], color="#1f3b73", lw=0.8, ls="--")
    a4.text(ux - 23.5, pe, "edge of the\nmodule PCB", fontsize=7, color="#1f3b73", va="center")
    xd = ux + 20.0
    for ya, yb, txt in ((u["rim_y"], oy2, f"rim {oy2 - u['rim_y']:.2f} behind the outer face"), ):
        a4.annotate("", (xd, ya), (xd, yb), arrowprops=dict(arrowstyle="<->"))
        a4.plot([ux + 4.5, xd + 0.4], [ya, ya], color="k", lw=0.4, ls=":")
        a4.text(xd, yb + 0.6, "rim %.2f behind\nthe outer face" % (oy2 - u["rim_y"]), fontsize=7.5, va="bottom", ha="center")
    a4.annotate("", (ux - 20.0, over_y), (ux - 20.0, oy2), arrowprops=dict(arrowstyle="<->"))
    a4.plot([ux - 20.4, ux - 6.2], [over_y, over_y], color="k", lw=0.4, ls=":")
    a4.plot([ux - 20.4, ux - 17.5], [oy2, oy2], color="k", lw=0.4, ls=":")
    a4.plot([ux + 17.5, xd + 0.4], [oy2, oy2], color="k", lw=0.4, ls=":")
    a4.text(ux - 20.6, oy2 + 0.6, f"overmould {oy2 - over_y:.2f}\ndeep in the case", fontsize=7.5, va="bottom", ha="center")
    a4.annotate(f"{plug['shell_l']:.2f} shell, {u['rim_y'] - u['back_y']:.2f} of it in the receptacle", (ux + plug["shell_w"] / 2, u["back_y"] + 1.2), (ux + 2.0, u["rear_y"] - 4.0),
                arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a4.annotate(f"{(P['usb_w'] - plug['over_w']) / 2:.2f} each side\n({P['usb_w']:.1f} x {P['usb_h']:.1f} well)", (ux + (hw + plug["over_w"] / 2) / 2, oy2 - 0.4), (ux + 7.5, oy2 + 4.5),
                arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a4.text(k["x"]["RST"], oy2 + 1.2, "RST", ha="center", fontsize=8); a4.text(k["x"]["BOOT"], oy2 + 1.2, "BOOT", ha="center", fontsize=8)
    a4.set_xlim(ux - 25, ux + 25); a4.set_ylim(u["rear_y"] - 6.5, oy2 + 10.5); a4.set_aspect("equal")
    a4.set_title("USB-C end from above, cut on the axis of the port, plug fully in (mm)"); a4.set_xlabel("x"); a4.set_ylabel("y (outside at the top)")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "detail_sections.png"), dpi=130)


if __name__ == "__main__":
    sys.exit(main())
