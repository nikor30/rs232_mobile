#!/usr/bin/env python3
"""Raytraced pictures of the case (POV-Ray) from the meshes that case.py wrote to out/.

    python3 render.py            # writes out/render_*.png
    python3 render.py --hat      # the two-port variant, out_hat/
    python3 render.py --drawings # only the dimensioned sheets (detail_sections.png, hat_layout.png), no ray tracing

Device coordinates as in case.py: X across, Y from the RJ45 edge to the USB-C end, Z up.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HAT = "--hat" in sys.argv                       # the two-port variant: reads and writes out_hat/
OUT = os.path.join(HERE, "out_hat" if HAT else "out")

STUDIO = "--studio" in sys.argv                 # product shots on a floor: out*/studio_*.png
# what the display shows in the pictures: screenshots of the firmware's home page, its sheet from below
# (baud ruler, keys) and its sheet from above (QR code, addresses), 240 x 320 enlarged three times
LCD = os.path.join(HERE, "lcd_status.png")
LCD2 = os.path.join(HERE, "lcd_sheet.png")
LCD3 = os.path.join(HERE, "lcd_qr.png")


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


PARTS = ["body", "base", "_frame", "_glass", "_tp_sensor", "_lcm", "_pmma", "_pcb_ws", "_headers", "_standoffs", "_usbc", "_buttons",
         "_bat_header", "_tf_slot", "_camera_fpc", "_pcb", "_parts_top", "_parts_bottom", "_keys", "_plug_shell", "_plug_over"]

SCENE = r'''
#version 3.7;
global_settings { assumed_gamma 1.0 max_trace_level 8
  radiosity { pretrace_start 0.08 pretrace_end 0.005 count 220 nearest_count 10 error_bound 0.3 recursion_limit 2 brightness 0.9 } }
#ifndef (View) #declare View = 1; #end
#ifndef (Floor) #declare Floor = 0; #end     // 0 none (documentation pictures), 1 black gloss, 2 light sweep
#ifndef (AR) #declare AR = 4/3; #end         // width / height of the picture
#ifndef (Lcd) #declare Lcd = 1; #end         // 1 home page, 2 sheet with the baud ruler, 3 sheet with the QR code
#ifndef (Zf) #declare Zf = __ZFLOOR__; #end  // height of the floor
#ifndef (Empty) #declare Empty = 0; #end     // 1: the floor alone, the picture a light-sweep render is divided by
#ifndef (SBa) #declare SBa = 62; #end        // soft box: what it covers on the glass, across and along the view (mm)
#ifndef (SBl) #declare SBl = 84; #end
#ifndef (SBo) #declare SBo = 0; #end         // shifted along the view (mm on the glass)
#ifndef (SBe) #declare SBe = 0.8; #end       // brightness at its far edge
#ifndef (SBp) #declare SBp = 1.5; #end       // how late the brightness rises
__INCLUDES__
#if (Floor = 0) background { rgbt <0, 0, 0, 1> } #else background { rgb 0 } #end
sphere { <40, 55, 0>, 2500 hollow no_image no_shadow no_reflection
  texture { pigment { gradient z color_map { [0.0 rgb <0.05, 0.055, 0.07>] [0.3 rgb <0.10, 0.11, 0.14>] [1.0 rgb <0.30, 0.32, 0.38>] } scale 2500 } finish { emission 1.0 ambient 0 diffuse 0 } } }
#if (Floor = 2)   // far away and large: the same light everywhere on the floor, shadows still soft
light_source { <40, 55, 0> + <-300, 325, 420> * 8 color rgb <1.0, 0.99, 0.97> * 1.0 area_light <1100, 0, 0>, <0, 1100, 0>, 9, 9 adaptive 1 jitter circular orient }
light_source { <40, 55, 0> + <280, -135, 260> * 8 color rgb <0.9, 0.94, 1.0> * 0.4 area_light <1100, 0, 0>, <0, 1100, 0>, 9, 9 adaptive 1 jitter circular orient }
#else
light_source { <-260, 380, 420> color rgb <1.0, 0.98, 0.95> * 1.15 area_light <120, 0, 0>, <0, 120, 0>, 9, 9 adaptive 1 jitter circular orient }
light_source { <320, -80, 260> color rgb <0.85, 0.9, 1.0> * 0.5 area_light <80, 0, 0>, <0, 80, 0>, 5, 5 adaptive 1 jitter circular orient shadowless }
#end
#if (View = 11)
light_source { <39, 150, 200> color rgb <1, 1, 1> * 0.8 area_light <60, 0, 0>, <0, 60, 0>, 5, 5 adaptive 1 jitter circular orient shadowless }
#end
#if (View = 6 | View = 8)
light_source { <40, 60, -300> color rgb <1, 1, 1> * 0.9 area_light <80, 0, 0>, <0, 80, 0>, 5, 5 adaptive 1 jitter circular orient }
#end

#if (Floor = 0)
#declare T_body = texture { pigment { rgb <0.90, 0.895, 0.875> } finish { diffuse 0.72 specular 0.18 roughness 0.06 brilliance 1.2 } }
#else   // printed PLA: layers of 0.2 mm on the walls, the grain of the build plate on the top face
#declare T_body = texture { pigment { rgb <0.83, 0.83, 0.82> }
  normal { average normal_map { [1 gradient z 0.5 sine_wave scale 0.2] [1 bumps 0.12 scale 0.09] } }
  finish { diffuse 0.74 specular 0.14 roughness 0.09 brilliance 1.1 } }
#end
#declare T_base = texture { pigment { rgb <0.42, 0.43, 0.45> } finish { diffuse 0.7 specular 0.12 roughness 0.08 } }
#declare T_glass = texture { pigment { rgb <0.03, 0.03, 0.035> } finish { diffuse 0.3 specular 0.6 roughness 0.004 reflection { 0.08, 0.35 fresnel } } }
#declare T_dark = texture { pigment { rgb <0.10, 0.10, 0.11> } finish { diffuse 0.6 specular 0.15 roughness 0.05 } }
#declare T_fr4 = texture { pigment { rgb <0.10, 0.36, 0.20> } finish { diffuse 0.7 specular 0.25 roughness 0.02 } }
#declare T_ws = texture { pigment { rgb <0.07, 0.16, 0.42> } finish { diffuse 0.7 specular 0.25 roughness 0.02 } }
#declare T_metal = texture { pigment { rgb <0.75, 0.75, 0.78> } finish { specular 0.7 roughness 0.02 reflection { 0.2 metallic } } }
#declare T_gold = texture { pigment { rgb <0.85, 0.70, 0.30> } finish { specular 0.8 roughness 0.01 reflection { 0.2 metallic } } }
#declare T_white = texture { pigment { rgb <0.92, 0.92, 0.88> } finish { diffuse 0.75 } }
#declare T_clear = texture { pigment { rgbt <0.85, 0.9, 0.95, 0.5> } finish { diffuse 0.4 specular 0.5 roughness 0.01 } }

#switch (Lcd) #case (2) #declare LcdFile = "__LCD2__"; #break #case (3) #declare LcdFile = "__LCD3__"; #break #else #declare LcdFile = "__LCD__"; #end
#declare Module = union {
  object { m_glass texture { T_glass } }
  // the lit view area (30.99 x 41.20, 7.51 from the antenna end), just above the glass surface
  box { <0, 0, 0>, <1, 1, 0.02>
    texture { pigment { image_map { png LcdFile once } } finish { emission 1.0 ambient 0 diffuse 0.15 specular 0.5 roughness 0.004 reflection { 0.05, 0.25 fresnel } } }
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
__PLUGS__
}
#declare Carrier = union {
  object { m_pcb texture { T_fr4 } }
  object { m_parts_top texture { T_dark } }
  object { m_parts_bottom texture { T_dark } }
__JACKS__
}
#declare Battery = box { <__BX1__, __BY1__, __BZ1__>, <__BX2__, __BY2__, __BZ2__> texture { pigment { rgb <0.20, 0.22, 0.28> } finish { diffuse 0.6 specular 0.3 roughness 0.02 } } }
#declare Body = object { m_body texture { T_body } }
// the two keys are part of the frame (one printed piece); they are a mesh of their own so a view can pick them out
#declare Keys = object { m_keys texture { pigment { rgb <0.25, 0.26, 0.28> } finish { diffuse 0.7 specular 0.2 roughness 0.05 } } }
#declare KeysHi = object { m_keys texture { pigment { rgb <0.04, 0.42, 0.92> } finish { diffuse 0.8 specular 0.05 roughness 0.1 } } }
#declare Plug = union {
  object { m_plug_shell texture { T_metal } }
  object { m_plug_over texture { pigment { rgb <0.93, 0.93, 0.92> } finish { diffuse 0.75 specular 0.2 roughness 0.04 } } }
}
#declare Base = object { m_base texture { T_base } }
#declare Frame = object { m_frame texture { pigment { rgb <0.25, 0.26, 0.28> } finish { diffuse 0.7 specular 0.2 roughness 0.05 } } }
// the parts as they lie on the printer's bed
#declare PrintParts = union {
__PRINT__
}
// The camera positions are written for the four-port case (centre 39, 57.4; USB-C end at y = 116.73).
// A variant of another size moves and scales them: Cam about its centre, CamU with its USB-C end.
#declare CS = __CS__; #declare CCX = __CCX__; #declare CCY = __CCY__; #declare UY = __UY__;
#macro Cam(ex, ey, ez, lx, ly, lz, ang)
  #declare CamLoc = <CCX + (ex - 39) * CS, CCY + (ey - 57.4) * CS, ez * CS>;
  camera { perspective location <CCX + (ex - 39) * CS, CCY + (ey - 57.4) * CS, ez * CS> look_at <CCX + (lx - 39) * CS, CCY + (ly - 57.4) * CS, lz>
           sky <0, 0, 1> right <-AR, 0, 0> angle ang }
#end
#macro CamU(ex, ey, ez, lx, ly, lz, ang)
  #declare CamLoc = <CCX + ex - 39, UY + ey - 116.73, ez>;
  camera { perspective location <CCX + ex - 39, UY + ey - 116.73, ez> look_at <CCX + lx - 39, UY + ly - 116.73, lz> sky <0, 0, 1> right <-AR, 0, 0> angle ang }
#end
// ---- studio: floor, feet, a light panel that shows in the glass, cables
#if (Floor = 1)
plane { z, Zf pigment { rgb 0 } finish { diffuse 0 ambient 0 specular 0 reflection { 0.02, 0.45 fresnel on } } interior { ior 1.5 } }
#end
#if (Floor = 2)
plane { z, Zf pigment { rgb <0.96, 0.96, 0.97> } finish { diffuse 0.95 ambient 0 } }   // brighter than the case, so the case stands out
#end
#declare Feet = union {
__FEET__
}
#declare T_cable = texture { pigment { rgb <0.90, 0.90, 0.89> } finish { diffuse 0.75 specular 0.25 roughness 0.03 } }
#declare T_patch = texture { pigment { rgb <0.05, 0.25, 0.62> } finish { diffuse 0.7 specular 0.3 roughness 0.03 } }
// USB-C cable: from the back of the plug's grip in a bow down to the floor and away
#declare PX = CCX; #declare PY = UY + 19.32; #declare PZ = __PLUGZ__;
#declare UsbCable = union {
  cone { <PX, PY - 0.5, PZ>, 2.9, <PX, PY + 9, PZ>, 2.0 texture { T_cable } }
  sphere_sweep { cubic_spline 7,
    <PX, PY - 10, PZ>, 1.7, <PX, PY + 5, PZ>, 1.7, <PX + 2, PY + 30, PZ - 3>, 1.7, <PX + 14, PY + 58, Zf + 4>, 1.7,
    <PX + 44, PY + 84, Zf + 1.7>, 1.7, <PX + 110, PY + 96, Zf + 1.7>, 1.7, <PX + 260, PY + 90, Zf + 1.7>, 1.7
    tolerance 0.05 texture { T_cable } }
}
// a patch cable in jack N (0 = first): clear plug with its contacts, boot, cable in a bow to the floor
#macro Patch(N, Side)
  #local JX = __JACK0__ + N * __JACKP__; #local JY = __JACKY__; #local JZ = 6.7;
  union {
    box { <JX - 5.8, JY - 12.5, JZ - 3.3>, <JX + 5.8, JY + 3, JZ + 3.3>
      texture { pigment { rgbt <0.86, 0.90, 0.93, 0.55> } finish { diffuse 0.3 specular 0.7 roughness 0.008 reflection { 0.04, 0.3 fresnel on } } } interior { ior 1.45 } }
    #local I = 0; #while (I < 8)
      box { <JX - 4.5 + I * 1.27, JY - 6, JZ + 2.4>, <JX - 4.1 + I * 1.27, JY + 2.5, JZ + 3.1> texture { T_gold } }
    #local I = I + 1; #end
    box { <JX - 2.2, JY - 12, JZ - 5.2>, <JX + 2.2, JY - 2, JZ - 3.3> texture { pigment { rgbt <0.86, 0.90, 0.93, 0.45> } finish { diffuse 0.3 specular 0.7 roughness 0.008 } } }
    cone { <JX, JY - 12.5, JZ>, 4.6, <JX, JY - 30, JZ>, 3.0 texture { T_patch } }
    sphere_sweep { cubic_spline 7,
      <JX, JY, JZ>, 2.7, <JX, JY - 28, JZ>, 2.7, <JX + Side * 3, JY - 52, JZ - 5>, 2.7, <JX + Side * 16, JY - 80, Zf + 5>, 2.7,
      <JX + Side * 46, JY - 104, Zf + 2.7>, 2.7, <JX + Side * 120, JY - 116, Zf + 2.7>, 2.7, <JX + Side * 280, JY - 108, Zf + 2.7>, 2.7
      tolerance 0.05 texture { T_patch } }
  }
#end
#if (Empty)
  #declare Nothing = sphere { <0, 0, -100000>, 0.001 }
  #declare Body = Nothing; #declare Base = Nothing; #declare Module = Nothing; #declare Carrier = Nothing;
  #declare Keys = Nothing; #declare Feet = Nothing; #declare Plug = Nothing; #declare UsbCable = Nothing;
  #declare Frame = Nothing; #declare Battery = Nothing;
#end
#switch (View)
#case (21)  // studio: the whole device, USB-C end towards the viewer
  Cam(-84, 292, 118, 40, 58, 2, 29)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Feet }
#break
#case (22)  // studio: the port end
  Cam(150, -208, 98, 40, 50, -1, 31)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Feet }
#break
#case (23)  // studio: the display, close
  CamU(-38, 232, 118, 38, 82, 14, 20)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Feet }
#break
#case (24)  // studio: exploded, no floor
  #declare CS = (CS + 1) / 2;
  Cam(-175, 345, 185, 40, 58, __EXZ__, __EXA__ * 0.80)
  object { Module translate <0, 0, __EXM__> }
  object { Body translate <0, 0, __EXB__> }
  object { Frame translate <0, 0, __EXF__> }
  object { Keys translate <0, 0, __EXF__> }
  object { Carrier }
  object { Battery translate <0, 0, __EXC__> }
  object { Base translate <0, 0, __EXS__> }
#break
#case (25)  // studio: the USB-C end with a cable plugged in
  CamU(-52, 262, 62, 39, 112, 3, 24)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Feet } object { Plug } object { UsbCable }
#break
#case (26)  // studio: the electronics alone
  Cam(-150, 300, 175, 40, 55, 6, 23)
  object { Module } object { Carrier } object { Battery }
#break
#case (27)  // studio: at work - a patch cable in port 1, charging over USB-C
  Cam(176, -232, 124, 46, 36, -2, 33)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Feet } object { Plug } object { UsbCable }
  Patch(0, 1)
#break
#case (1)   // hero: USB-C end towards the viewer
  Cam(-85, 300, 150, 40, 62, 0, 30)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame }
#break
#case (2)   // port end
  Cam(150, -200, 110, 40, 45, 2, 30)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame }
#break
#case (3)   // exploded
  #declare CS = (CS + 1) / 2;
  Cam(-175, 345, 185, 40, 58, __EXZ__, __EXA__)
  object { Module translate <0, 0, __EXM__> }
  object { Body translate <0, 0, __EXB__> }
  object { Frame translate <0, 0, __EXF__> }
  object { Keys translate <0, 0, __EXF__> }
  object { Carrier }
  object { Battery translate <0, 0, __EXC__> }
  object { Base translate <0, 0, __EXS__> }
#break
#case (4)   // section along the middle of the display
  Cam(-260, 150, 70, 40, 60, 3, 26)
  #declare Cut = plane { x, CCX inverse }
  intersection { object { Body } object { Cut } cutaway_textures }
  intersection { object { Frame } object { Cut } cutaway_textures }
  intersection { object { Keys } object { Cut } cutaway_textures }
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
  object { Body } object { Base } object { Keys } object { Frame }
#break
#case (7)   // the electronics without the case: carrier with the module plugged in
  Cam(-150, 300, 175, 40, 55, 6, 30)
  object { Module } object { Carrier } object { Battery }
#break
#case (8)   // the carrier from below: relays, battery connectors, protection diodes
  Cam(-130, 270, -190, 40, 55, 0, 30)
  object { Carrier }
#break
#case (9)   // the USB-C end: bay, port, the two keys
  CamU(8, 232, 52, 39, 116, 9.5, 24)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame }
#break
#case (10)  // the same with a plug in (overmould of the largest size the USB-IF allows)
  CamU(8, 232, 52, 39, 122, 9.5, 24)
  object { Body } object { Base } object { Module } object { Carrier } object { Keys } object { Frame } object { Plug }
#break
#case (11)  // cut open level with the switches: receptacle, switches, the heads of the keys with their noses
  CamU(39, 138, 86, 39, 108.5, 10, 30)
  #declare Cut = plane { z, __CUTZ__ }
  intersection { object { Body } object { Cut } cutaway_textures }
  intersection { object { Module } object { Cut } cutaway_textures }
  intersection { object { KeysHi } object { Cut } cutaway_textures }
  intersection { object { Frame } object { Cut } cutaway_textures }
  object { Carrier } object { Base }
#break
#case (12)  // how the module is held: frame, module and keys without the body, seen from the jaw side
  Cam(210, 250, 150, 40, __CLY__, __CLZ__, 22)
  object { Frame } object { Module } object { Keys }
#break
#case (13)  // the frame alone from below: jaws, legs, the two keys on their leaves
  Cam(160, 265, -150, 40, __CLY__, __CLZ__, 24)
  object { Frame } object { Keys }
#break
#case (14)  // every part as it lies on the bed of the printer
  camera { perspective location <128, -250, 260> look_at <128, 105, 8> sky <0, 0, 1> right <-AR, 0, 0> angle 38 }
  #declare CamLoc = <128, -250, 260>;
  box { <0, 0, -2>, <256, 256, 0> texture { pigment { rgb <0.13, 0.13, 0.14> } normal { bumps 0.3 scale 0.6 } finish { diffuse 0.6 specular 0.25 roughness 0.08 } } }
  object { PrintParts texture { T_body } }
#break
#case (15)  // the body alone from below: skin, opening for the glass, open ends
  Cam(-110, 260, -170, 40, 58, 8, 32)
  object { Body }
#break
#case (16)  // the keys: the frame turned as it lies on the bed (seat down), the keys picked out in colour
  camera { perspective location <CCX - 44, UY + 118, __KZ__ - 78> look_at <CCX, UY - 5, __KZ__ + 1> sky <0, 0, -1> right <-AR, 0, 0> angle 31 }
  #declare CamLoc = <CCX - 44, UY + 118, __KZ__ - 78>;
  object { Frame } object { KeysHi }
#break
#case (17)  // the keys at work: frame and module seen from the USB-C end, the body left away
  camera { perspective location <CCX - 40, UY + 104, __KZ__ + 34> look_at <CCX, UY - 4, __KZ__ - 1> sky <0, 0, 1> right <-AR, 0, 0> angle 31 }
  #declare CamLoc = <CCX - 40, UY + 104, __KZ__ + 34>;
  object { Frame } object { KeysHi } object { Module }
#break
#case (18)  // the keys from straight above, module left away: leaves, slots, stops
  camera { orthographic location <CCX, UY - 14, 400> look_at <CCX, UY - 14, 0> sky <0, 1, 0> right <-__KW__, 0, 0> up <0, __KW__ / AR, 0> }
  #declare CamLoc = <CCX, UY - 14, 400>;
  object { Frame } object { KeysHi }
#break
#end
#if (Floor > 0)
  // A soft box overhead, placed where the display glass mirrors it for this camera: the sheen in the glass,
  // dark at the near edge and bright at the far one.  The floor mirrors the box in nearly the same direction,
  // so it is kept narrower than the top face of the case: there its mirror image lies hidden behind the device.
  #local G = <__VAX__, __VAY__, __ZTOP__>;
  #local D = G - CamLoc;
  #local T = (420 - G.z) / (-D.z);
  #local U = vnormalize(<D.x, D.y, 0>);
  box { <-0.5, -0.5, 0>, <0.5, 0.5, 1> no_image no_shadow
    texture { pigment { function { 0.999 * pow(max(0, min(1, y + 0.5)), SBp) * min(1, max(0, (0.5 - abs(x)) / 0.2)) }
                        color_map { [0 rgb 0] [1 rgb SBe] } }
              finish { emission 1 ambient 0 diffuse 0 } }
    scale <SBa * (1 + T), SBl * (1 + T), 1> rotate <0, 0, degrees(atan2(-U.x, U.y))>
    translate G + <D.x, D.y, -D.z> * T + U * SBo * (1 + T) }
#end
'''


def main():
    import shutil
    if HAT and "_plugs" not in PARTS:
        PARTS.append("_plugs")
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
    if "--drawings" in sys.argv:                 # only the dimensioned sheets, no ray tracing
        details(geo)
        if HAT:
            hat_layout(geo)
        return 0
    cx, cy = geo["view_area_centre"]
    jacks = []
    jf = geo["jack_front"]                      # the jack fronts sit this far behind the board edge
    for xc in geo["jacks"]:
        jacks.append(f"  box {{ <{xc - 5.8}, {jf - 0.05 if not HAT else jf + 13.0}, 2.7>, <{xc + 5.8}, {jf + 6.0 if not HAT else jf + 13.9}, 10.7> texture {{ pigment {{ rgb <0.02, 0.02, 0.02> }} finish {{ diffuse 0.3 }} }} }}")
        for i in range(8):
            jacks.append(f"  box {{ <{xc - 4.5 + i * 1.27:.2f}, {jf - 0.08 if not HAT else jf + 1.0}, 9.0>, <{xc - 4.1 + i * 1.27:.2f}, {jf + 0.5 if not HAT else jf + 9.0}, {10.4 if not HAT else 9.4}> texture {{ T_gold }} }}")
    bx1, by1, bx2, by2, bz1, bz2 = geo["cell_box"]
    # the print-ready files, side by side on a bed of 256 x 256 mm
    pd = os.path.join(OUT, "print")
    prints, x0, y0, row = [], 12.0, 14.0, 0.0
    for name in ("body_print", "base_print", "frame_print"):
        data = open(os.path.join(pd, name + ".stl"), "rb").read()
        n = struct.unpack("<I", data[80:84])[0]
        tris = [struct.unpack("<12fH", data[84 + 50 * k:134 + 50 * k]) for k in range(n)]
        xs = [t[3 + 3 * q] for t in tris for q in range(3)]
        ys = [t[4 + 3 * q] for t in tris for q in range(3)]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        if x0 + w > 246:
            x0, y0, row = 12.0, y0 + row + 14.0, 0.0
        dx, dy = x0 - min(xs), y0 - min(ys)
        with open(os.path.join(OUT, f"p_{name}.inc"), "w") as fh:
            fh.write(f"#declare m_p_{name} = mesh {{\n")
            for t in tris:
                fh.write("triangle{<%.3f,%.3f,%.3f>,<%.3f,%.3f,%.3f>,<%.3f,%.3f,%.3f>}\n" % (t[3] + dx, t[4] + dy, t[5], t[6] + dx, t[7] + dy, t[8], t[9] + dx, t[10] + dy, t[11]))
            fh.write("}\n")
        inc.append(f'#include "p_{name}.inc"')
        prints.append(f"  object {{ m_p_{name} }}")
        x0, row = x0 + w + 14.0, max(row, h)
    P = geo["P"]
    zf = geo["z"]["bot"] - 0.9                   # the case stands on stick-on feet 1.5 mm thick
    fx = (geo["ox"][0] + P["wall"] + P["base_inset"] + 10, geo["ox"][1] - P["wall"] - P["base_inset"] - 10)
    fy = (geo["oy"][0] + P["wall_rj"] + P["base_inset"] + 10, geo["oy"][1] - P["wall"] - P["base_inset"] - 10)
    feet = "\n".join(f"  cylinder {{ <{x:.2f}, {y:.2f}, {zf:.2f}>, <{x:.2f}, {y:.2f}, {zf + 1.5:.2f}>, 5.00 texture {{ T_dark }} }}"
                     for x in fx for y in fy)
    scene = (SCENE.replace("__INCLUDES__", "\n".join(inc)).replace("__LCD__", LCD).replace("__LCD2__", LCD2).replace("__LCD3__", LCD3)
             .replace("__ZFLOOR__", f"{zf:.2f}").replace("__FEET__", feet).replace("__PLUGZ__", f"{geo['usb']['z']:.2f}")
             .replace("__JACK0__", f"{geo['jacks'][0]}").replace("__JACKP__", f"{geo['jacks'][1] - geo['jacks'][0]}")
             .replace("__JACKY__", f"{geo['jack_front'] + 0.5:.2f}")
             .replace("__VAW__", "30.99").replace("__VAL__", "41.20").replace("__VAX__", f"{cx:.3f}").replace("__VAY__", f"{cy:.3f}")
             .replace("__ZTOP__", f"{geo['z_glass_top']:.3f}").replace("__JACKS__", "\n".join(jacks))
             .replace("__BX1__", f"{bx1}").replace("__BX2__", f"{bx2}").replace("__BY1__", f"{by1:.2f}").replace("__BY2__", f"{by2:.2f}")
             .replace("__CS__", f"{(geo['oy'][1] - geo['oy'][0]) / 118.63 + 0.07:.3f}" if HAT else "1").replace("__CCX__", f"{geo['ws_xc']}")
             .replace("__CCY__", f"{(geo['oy'][0] + geo['oy'][1]) / 2:.2f}").replace("__UY__", f"{geo['oy'][1]:.2f}")
             .replace("__PRINT__", "\n".join(prints))
             .replace("__EXF__", "30" if HAT else "24").replace("__EXC__", "-16" if HAT else "-20").replace("__EXS__", "-40" if HAT else "-36")
             .replace("__CLY__", f"{(geo['frame'][1] + geo['frame'][3]) / 2:.1f}").replace("__CLZ__", f"{geo['z']['glass'] - 6:.1f}")
             .replace("__EXZ__", "40" if HAT else "20").replace("__EXA__", "50" if HAT else "44")
             .replace("__EXM__", "104" if HAT else "82").replace("__EXB__", "66" if HAT else "52")
             .replace("__CUTZ__", f"{geo['key']['z'] - 0.15:.2f}").replace("__KZ__", f"{geo['key']['z']:.2f}")
             .replace("__KW__", f"{geo['frame'][2] - geo['frame'][0] + 8:.1f}")
             .replace("__BZ1__", f"{bz1:.2f}").replace("__BZ2__", f"{bz2:.2f}")
             .replace("__PLUGS__", "  object { m_plugs texture { pigment { rgb <0.03, 0.03, 0.03> } finish { diffuse 0.5 specular 0.3 roughness 0.03 } } }" if HAT else "")
             )
    pov = os.path.join(OUT, "scene.pov")
    open(pov, "w").write(scene)
    if STUDIO:
        return studio()
    names = {1: "hero", 2: "ports", 3: "exploded", 4: "section", 5: "base", 6: "below", 7: "electronics", 8: "carrier_bottom",
             9: "usb_end", 10: "usb_plug", 11: "usb_cut", 12: "clamp", 13: "frame", 14: "print_bed", 15: "body_inside",
             16: "keys", 17: "keys_module", 18: "keys_top"}
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
    if any(a.isdigit() for a in sys.argv[1:]):      # single views were asked for: leave the drawings alone
        return 0 if ok else 1
    details(geo)
    if geo.get("hat"):
        hat_layout(geo)
    return 0 if ok else 1


# Product shots: name, view, floor (0 none, 1 black gloss, 2 light sweep), page on the display, width, height, extra declares
STUDIO_VIEWS = [
    ("hero", 21, 1, 2, 2000, 1500, {}),
    ("hero_light", 21, 2, 1, 2000, 1500, {}),
    ("ports", 22, 1, 1, 2000, 1500, {}),
    ("ports_light", 22, 2, 1, 2000, 1500, {}),
    ("display", 23, 1, 1, 1500, 1800, {}),
    ("exploded", 24, 0, 1, 1500, 1800, {}),
    ("usb_cable", 25, 1, 2, 2000, 1500, {}),
    ("electronics", 26, 0, 1, 2000, 1500, {}),
    ("at_work", 27, 1, 1, 2000, 1500, {}),
]
LIGHT_BG = (245, 245, 247)       # the light slides of the presentation


def flatten_backdrop(path, flat):
    """Light sweep: divide the picture by the picture of the empty floor, so that the open floor comes out in
    exactly LIGHT_BG and only the shadows remain.  On a slide of that colour the picture then has no border."""
    import numpy as np
    from PIL import Image, ImageFilter
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    b = np.asarray(Image.open(flat).convert("RGB").filter(ImageFilter.GaussianBlur(12))).astype(float)
    a = a / np.maximum(b, 1.0) * np.array(LIGHT_BG)       # the device itself may be brighter than the backdrop
    Image.fromarray(feather(a)).save(path)
    os.remove(flat)


def feather(a):
    """Let the outer 7 % of a light picture run into LIGHT_BG: a shadow that reaches the edge then ends softly."""
    import numpy as np
    h, w, _ = a.shape
    yy, xx = np.mgrid[0:h, 0:w]
    t = np.clip(np.minimum.reduce([xx, yy, w - 1 - xx, h - 1 - yy]) / (0.07 * w), 0, 1)
    k = (t * t * (3 - 2 * t))[..., None]
    return np.clip(a * k + np.array(LIGHT_BG) * (1 - k), 0, 255).astype("uint8")


def studio():
    """python3 render.py --studio [--hat] [name ...] [--draft]"""
    want = [a for a in sys.argv[1:] if not a.startswith("-")]
    draft = "--draft" in sys.argv
    ok = True
    for name, view, floor, lcd, w, h, extra in STUDIO_VIEWS:
        if want and name not in want:
            continue
        if draft:
            w, h = w // 3, h // 3
        out = os.path.join(OUT, f"studio_{name}.png")
        args = ["povray", "+Iscene.pov", f"+O{out}", f"+W{w}", f"+H{h}", "+A0.3" if draft else "+A0.1", "+AM2", "+R3", "+J0", "-D", "-V", "+Q9",
                f"Declare=View={view}", f"Declare=Floor={floor}", f"Declare=Lcd={lcd}", f"Declare=AR={w / h:.5f}"]
        args += [f"Declare={k}={v}" for k, v in extra.items()]
        if floor == 0:
            args.append("+UA")
        r = subprocess.run(args, cwd=OUT, capture_output=True, text=True)
        good = r.returncode == 0 and os.path.exists(out)
        if good and floor == 2:
            flat = os.path.join(OUT, "_flat.png")
            subprocess.run([a for a in args if not a.startswith("+O")] + [f"+O{flat}", "Declare=Empty=1"], cwd=OUT, capture_output=True, text=True)
            flatten_backdrop(out, flat)
        ok &= good
        print("studio", name, "ok" if good else r.stderr[-900:])
    return 0 if ok else 1


def _plt():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle
    return plt, Polygon, Rectangle


GREY, PART, DARK = "#d9d7d2", "#8f9094", "#55575c"
C_GLASS, C_LCD, C_SPACER, C_PCB, C_CELL = "#1c1d22", "#b9babd", "#dfe8ef", "#1f3b73", "#3a4254"


def hat_layout(geo):
    """Two-port variant: plan and lengthwise section of what lies where, and the order of assembly."""
    plt, Polygon, Rectangle = _plt()
    h, P, z, zm, ws = geo["hat"], geo["P"], geo["z"], geo["zm"], geo["ws"]
    W, H = geo["W"], geo["H"]
    (ox1, ox2), (oy1, oy2), (cx1, cx2), (cy1, cy2) = geo["ox"], geo["oy"], geo["cx"], geo["cy"]
    gx1, gy1, gx2, gy2 = geo["glass"]
    px1, py1, px2, py2 = geo["module_pcb"]
    fx1, fy1, fx2, fy2 = geo["frame"]
    bx1, by1, bx2, by2, bz1, bz2 = geo["cell_box"]
    fig = plt.figure(figsize=(17, 12.5))
    a1 = fig.add_axes([0.04, 0.07, 0.36, 0.88])
    a2 = fig.add_axes([0.46, 0.40, 0.52, 0.55])
    a3 = fig.add_axes([0.46, 0.04, 0.52, 0.30]); a3.axis("off")
    # ---- plan, seen from above (display side), X across, Y from the port end to the USB-C end
    a1.add_patch(Rectangle((ox1, oy1), ox2 - ox1, oy2 - oy1, fc=GREY, ec="k", lw=1.2))
    a1.add_patch(Rectangle((cx1, cy1), cx2 - cx1, cy2 - cy1, fc="white", ec="k", lw=0.8))
    a1.add_patch(Rectangle((0, 0), W, H, fc="#2f6b45", ec="k", lw=1))
    for j in h["jacks"]:
        a1.add_patch(Rectangle((j[0], j[1]), j[2] - j[0], j[3] - j[1], fc="#cfd2d6", ec="k", lw=0.8))
    a1.text(geo["jacks"][0], 9, "PORT 1", ha="center", fontsize=8); a1.text(geo["jacks"][1], 9, "PORT 2", ha="center", fontsize=8)
    for name, x1, y1, x2, y2, _ in h["parts"]:
        a1.add_patch(Rectangle((x1, y1), x2 - x1, y2 - y1, fc="#222", ec="k", lw=0.4))
    for x, y in h["j3"]:
        a1.plot(x, y, "o", ms=4.5, mfc="#e3c14f", mec="k", mew=0.5)
    a1.text(h["j3"][0][0] - 1.5, h["j3"][0][1], "J3", ha="right", va="center", fontsize=8, color="w")
    for x, y in h["holes"]:
        a1.plot(x, y, "o", ms=6, mfc="white", mec="k", mew=0.8)
    a1.add_patch(Rectangle((bx1, by1), bx2 - bx1, by2 - by1, fc=C_CELL, ec="k", lw=0.8))
    c = geo["cell"]
    a1.text((bx1 + bx2) / 2, by1 + 21.0, f"battery {c[0]:.0f} x {c[1]:.0f} x {c[2]:.1f}\non the base, behind the board", ha="center", va="center", fontsize=8, color="w")
    r = h["rib"]
    a1.add_patch(Rectangle((r[0], r[1]), r[2] - r[0], r[3] - r[1], fc=PART, ec="k", lw=0.6))
    a1.add_patch(Rectangle((fx1, fy1), fx2 - fx1, fy2 - fy1, fill=False, ec="#b0362b", lw=1.4, ls="--"))
    a1.text(fx1 + 1.0, fy1 + 1.6, "frame (dashed)", fontsize=7.5, color="#b0362b")
    a1.add_patch(Rectangle((px1, py1), px2 - px1, py2 - py1, fill=False, ec=C_PCB, lw=1.3))
    a1.text(px1 + 1.0, py2 - 2.4, "module board", fontsize=7.5, color=C_PCB)
    a1.add_patch(Rectangle((gx1, gy1), gx2 - gx1, gy2 - gy1, fill=False, ec="k", lw=1.0, ls=":"))
    a1.text(gx2 - 1.0, gy1 + 1.4, "glass", fontsize=7.5, ha="right")
    for x1, y1, x2, y2 in geo["legs"]:
        a1.add_patch(Rectangle((x1, y1), x2 - x1, y2 - y1, fc="#b0362b", ec="k", lw=0.6, alpha=0.75))
    a1.text(geo["legs"][0][2] + 0.8, geo["legs"][0][1] + 2.0, "legs of\nthe frame", fontsize=7, color="#b0362b", va="center")
    for px, py in geo["body_posts"]:
        a1.add_patch(plt.Circle((px, py), P["post_d"] / 2, fill=False, ec="k", lw=1.0))
    a1.text(geo["body_posts"][0][0] + 3.2, geo["body_posts"][0][1], "post of\nthe body", fontsize=7, va="center")
    for name, (x, y) in h["plugs"].items():
        a1.add_patch(Rectangle((x - 1.27, y - 1.27), 2.54, 2.54, fc="#111", ec="w", lw=0.5))
        left = x < W / 2
        a1.text(x + (-2.0 if left else 2.0), y, name, fontsize=6.5, va="center", ha="right" if left else "left", color="k",
                bbox=dict(fc="white", ec="none", pad=0.6, alpha=0.85))
    a1.annotate("", (ox1 - 2.5, oy1), (ox1 - 2.5, oy2), arrowprops=dict(arrowstyle="<->"))
    a1.text(ox1 - 3.3, (oy1 + oy2) / 2, f"{oy2 - oy1:.1f}", rotation=90, va="center", ha="right", fontsize=8)
    a1.annotate("", (ox1, oy1 - 2.5), (ox2, oy1 - 2.5), arrowprops=dict(arrowstyle="<->"))
    a1.text((ox1 + ox2) / 2, oy1 - 3.4, f"{ox2 - ox1:.1f}", ha="center", va="top", fontsize=8)
    a1.text((ox1 + ox2) / 2, oy2 + 1.2, "USB-C end", ha="center", fontsize=8.5)
    a1.set_xlim(ox1 - 9, ox2 + 3); a1.set_ylim(oy1 - 6.5, oy2 + 4); a1.set_aspect("equal")
    a1.set_title("Two-port case from above: what lies where (mm)"); a1.set_xlabel("x"); a1.set_ylabel("y from the port end")
    # ---- lengthwise section through the middle
    a2.add_patch(Rectangle((cy1 + 0.25, z["base_bot"]), cy2 - cy1 - 0.5, z["base_top"] - z["base_bot"], fc=PART, ec="k", lw=0.8))
    a2.add_patch(Rectangle((oy1, z["base_bot"]), cy1 - oy1 + 0.25, z["sill"] - z["base_bot"], fc=PART, ec="k", lw=0.8))                 # sill
    a2.add_patch(Rectangle((cy2 + 0.05, z["base_bot"]), oy2 - cy2 - 0.05, z["tongue"] - z["base_bot"], fc=PART, ec="k", lw=0.8))         # tongue
    a2.add_patch(Rectangle((r[1], z["base_top"]), r[3] - r[1], r[4] - z["base_top"], fc=PART, ec="k", lw=0.8))                           # stop rib
    a2.add_patch(Polygon([(r[3], r[4] - 1.8), (r[3] + 1.0, r[4] - 0.8), (r[3] + 1.0, r[4]), (r[3], r[4])], closed=True, fc=PART, ec="k", lw=0.8))
    a2.add_patch(Rectangle((by1, bz1), by2 - by1, bz2 - bz1, fc=C_CELL, ec="k", lw=0.8))
    a2.text((by1 + by2) / 2, (bz1 + bz2) / 2, "battery", ha="center", va="center", fontsize=8, color="w")
    a2.add_patch(Rectangle((0, z["carr_bot"]), H, -z["carr_bot"], fc="#2f6b45", ec="k", lw=0.8))
    j = h["jacks"][0]
    a2.add_patch(Rectangle((j[1], 0), j[3] - j[1], geo["jack_h"], fc="#cfd2d6", ec="k", lw=0.8))
    a2.text((j[1] + j[3]) / 2, geo["jack_h"] / 2, "RJ45", ha="center", va="center", fontsize=8)
    # body: top plate over the jacks, skin over the frame, the strip of the end wall above the USB-C opening
    a2.add_patch(Polygon([(oy1, z["slot_top"]), (cy1, z["slot_top"]), (cy1, z["plate"]), (fy1 - 0.2, z["plate"]), (fy1 - 0.2, z["glass"]),
                          (gy1 - 0.25, z["glass"]), (gy1 - 0.25, z["top"]), (oy1 + 1.2, z["top"]), (oy1, z["top"] - 1.2)], closed=True, fc=GREY, ec="k", lw=1))
    yl_ = oy2 - P["key_land"]
    a2.add_patch(Polygon([(cy2, z["io_top"] + yl_ - cy2), (yl_, z["io_top"]), (oy2, z["io_top"]), (oy2, z["top"] - 1.2), (oy2 - 1.2, z["top"]), (cy2, z["top"])], closed=True, fc=GREY, ec="k", lw=1))
    # frame: end rail, and projected: a key (head and paddle), the angle that keeps the battery down, a leg
    a2.add_patch(Rectangle((fy1, z["frame_bot"]), py1 - 0.1 - fy1, z["glass"] - z["frame_bot"], fc=DARK, ec="k", lw=0.8))
    kg = geo["key"]["geo"]["BOOT"]
    zn = z["nose"] - P["key_nose_h"]
    a2.add_patch(Polygon([(kg["y_in"], z["glass"]), (kg["y_in"], zn), (fy2, zn), (fy2, kg["z_bot"]), (kg["y_f"], kg["z_bot"]), (kg["y_f"], kg["z0"]),
                          (fy2, kg["z0"] + kg["y_f"] - fy2), (fy2, z["glass"])], closed=True, fc="#2a7fd8", ec="k", lw=0.8, alpha=0.8))
    hd = geo["holds"][1]
    a2.add_patch(Rectangle((hd[1], z["tower"]), hd[3] - hd[1], z["frame_bot"] - z["tower"], fc=DARK, ec="k", lw=0.8, alpha=0.55))
    lg = geo["legs"][0]
    a2.add_patch(Polygon([(lg[1], 0), (lg[3], 0), (lg[3], z["frame_bot"]), (lg[1] - 4.0, z["frame_bot"])], closed=True, fc=DARK, ec="k", lw=0.8, alpha=0.55))
    # module
    for key, zkey, col in (("glass", "glass_z", C_GLASS), ("lcm", "lcm_z", C_LCD), ("pmma", "pmma_z", C_SPACER), ("pcb", "pcb_z", C_PCB)):
        b, zz = ws[key], ws[zkey]
        ya, yb = py2 - (b[3] - ws["pcb"][1]) + 0.0, py2 - (b[1] - ws["pcb"][1])
        a2.add_patch(Rectangle((ya, zz[0] + zm), yb - ya, zz[1] - zz[0], fc=col, ec="k", lw=0.6))
    a2.add_patch(Rectangle((h["hdr_y"][1] - 1.27, h["hdr_plastic_z"]), h["hdr_y"][0] - h["hdr_y"][1] + 2.54, ws["pcb_z"][0] + zm - h["hdr_plastic_z"], fc="#111", ec="k", lw=0.6))
    plugged = {round(y, 2) for _, (x, y) in h["plugs"].items()}
    for k in range(14):
        y = h["hdr_y"][0] + (h["hdr_y"][1] - h["hdr_y"][0]) * k / 13
        if round(y, 2) in plugged:
            a2.add_patch(Rectangle((y - 1.27, h["hdr_plastic_z"] - h["plug"][0]), 2.54, h["plug"][0], fc="#333", ec="k", lw=0.5))
        else:
            a2.plot([y, y], [h["pin_tip_z"], h["hdr_plastic_z"]], color="#b8962e", lw=1.2)
    a2.annotate("plugs of the jumper wires", (h["hdr_y"][0] + 2.0, h["hdr_plastic_z"] - 9), (cy1 + 1, z["plate"] - 3.5), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("legs of the frame stand on the board", (lg[3] - 2.5, 6.0), (cy1 + 1, z["plate"] - 6.5), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("angles of the frame\nkeep the battery down", (hd[3] - 1.5, z["tower"] + 6), (oy2 + 2, 7.0), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("key (blue): part of the frame,\npaddle in the opening", (kg["y_f"] - 0.8, (kg["z_bot"] + kg["z0"]) / 2), (oy2 + 2, 14.5), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("tongue of the base", (oy2 - 1.0, z["tongue"] - 6), (oy2 + 2, z["base_bot"] + 3), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("sill", (oy1 + 0.8, z["sill"] - 2.5), (oy1 - 9, z["base_bot"] + 3), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("lip", (r[3] + 0.6, r[4] - 0.6), (r[3] + 6, r[4] + 3.5), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("glass flush with the top,\non the frame all round", (gy1 + 14, z["top"]), (gy1 + 2, z["top"] + 4.0), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate("", (oy2 + 26.0, z["bot"]), (oy2 + 26.0, z["top"]), arrowprops=dict(arrowstyle="<->"))
    a2.text(oy2 + 26.8, (z["bot"] + z["top"]) / 2, f"{z['top'] - z['bot']:.1f}", rotation=90, va="center", fontsize=8)
    a2.set_xlim(oy1 - 11, oy2 + 30); a2.set_ylim(z["bot"] - 2.5, z["top"] + 6.5); a2.set_aspect("equal")
    a2.set_title("Section along the middle (mm): port end on the left, USB-C end on the right"); a2.set_xlabel("y"); a2.set_ylabel("z above the board")
    a3.text(0.0, 1.0, "Assembly\n"
                      "1. Bridge the solder jumpers JP1 - JP4 (they lie under the module later).  Solder the six jumper wires into J3.\n"
                      "2. Press the module into the frame from the glass side, pins first, until the glass lies on the frame.\n"
                      "    The two jaws give way and then hold the module's board.  The two keys are part of the frame.\n"
                      "3. Lay the body face down.  Put the frame with the module into it: the glass goes into its opening,\n"
                      "    the paddles of the keys slide down the opening of the USB-C end.\n"
                      "4. Plug the six wires onto the module's pins and the battery into the module.\n"
                      "5. Lay the board on the legs of the frame and the two posts of the body, parts towards the module.\n"
                      "6. Tuck the battery under the lip on the base.  Push the base in until its four arms click.\n"
                      "    Its sill closes the jack slot, its tongue the USB-C opening; its posts press the board against the legs,\n"
                      "    and through them the frame against the top.",
            va="top", fontsize=10.5, family="DejaVu Sans")
    fig.savefig(os.path.join(OUT, "hat_layout.png"), dpi=120)


def details(geo):
    """Dimensioned sections: snap arm, glass seat on both sides, a key, the USB-C end, the frame from above."""
    plt, Polygon, Rectangle = _plt()
    P, z, zm, ws = geo["P"], geo["z"], geo["zm"], geo["ws"]
    gx1, gy1, gx2, gy2 = geo["glass"]
    px1, py1, px2, py2 = geo["module_pcb"]
    fx1, fy1, fx2, fy2 = geo["frame"]
    (cx1, cx2), (ox1, ox2) = geo["cx"], geo["ox"]
    g, s, jx = P["glass_clr"], P["stack_clr"], geo["jaw_x"]
    fig, ((a1, a2, a3), (a4, a5, a6)) = plt.subplots(2, 3, figsize=(21, 13.5))
    # --- snap arm: section across a side wall, x = distance from the wall's inner face
    wall = [(-P["wall"], z["bot"]), (0, z["bot"]), (0, z["bot"] + 6.5), (-P["wall"], z["bot"] + 6.5)]
    a1.add_patch(Polygon(wall, closed=True, fc=GREY, ec="k", lw=1))
    a1.add_patch(Polygon([(-r, zz) for r, zz in geo["recess_profile"]], closed=True, fc="white", ec="k", lw=1))
    a1.add_patch(Polygon([(0.01, z["bot"]), (-0.9, z["bot"]), (0.01, z["bot"] + 0.9)], closed=True, fc="white", ec="k", lw=1))
    c = P["base_clr"]
    a1.add_patch(Rectangle((c, z["base_bot"]), P["arm_t"], P["arm_h"], fc=PART, ec="k", lw=1))
    a1.add_patch(Polygon([(c - r, zz) for r, zz in geo["bump_profile"]], closed=True, fc=PART, ec="k", lw=1))
    a1.add_patch(Rectangle((c + P["arm_t"] + P["arm_slot"], z["base_bot"]), 9 - c - P["arm_t"] - P["arm_slot"], z["base_top"] - z["base_bot"], fc=PART, ec="k", lw=1))
    a1.text(c + P["arm_t"] + 0.2, z["bump"] + 1.2, f"catch {P['bump']:.1f}", ha="left", fontsize=8)
    a1.text(-P["wall"] / 2 - 0.2, z["bot"] + 5.6, f"wall {P['wall']:.1f}", ha="center", fontsize=8)
    a1.text(c + P["arm_t"] / 2, z["base_bot"] + P["arm_h"] + 0.25, f"arm {P['arm_t']:.1f} x {P['arm_h']:.1f}\n{P['arm_l']:.0f} long, flexes in plane", ha="center", fontsize=8)
    a1.text(5.7, (z["base_bot"] + z["base_top"]) / 2, "base plate", ha="center", va="center", fontsize=8)
    a1.annotate("45 deg: holds, releases when pried", (-0.35, z["bump"] - 0.35), (-4.0, z["bot"] - 0.55), arrowprops=dict(arrowstyle="->"), fontsize=7)
    a1.annotate(f"{P['lead_deg']:.0f} deg lead-in", (-0.3, z["bump"] + 1.9), (-4.0, z["bot"] + 7.0), arrowprops=dict(arrowstyle="->"), fontsize=7)
    a1.set_xlim(-4.2, 9.5); a1.set_ylim(z["bot"] - 0.8, z["bot"] + 7.4); a1.set_aspect("equal")
    a1.set_title("Snap arm of the base in its wall pocket"); a1.set_xlabel("from the wall's inner face"); a1.set_ylabel("z")

    def stack(ax, x_from, x_to, side):
        """The module's layers between x_from and x_to; side = -1: their left edges are drawn, +1: their right edges."""
        for key, zkey, col, label, tc in (("glass", "glass_z", C_GLASS, "cover glass 1.10", "w"), ("tp_sensor", "tp_sensor_z", DARK, "", "k"),
                                          ("lcm", "lcm_z", C_LCD, "LCD 2.1", "k"), ("pmma", "pmma_z", C_SPACER, "spacer 2.0", "k"), ("pcb", "pcb_z", C_PCB, "module board 1.6", "w")):
            b, zz = ws[key], ws[zkey]
            half = (b[2] - b[0]) / 2
            xc = (px1 + px2) / 2
            xa, xb = (xc - half, x_to) if side < 0 else (x_from, xc + half)
            ax.add_patch(Rectangle((xa, zz[0] + zm), xb - xa, zz[1] - zz[0], fc=col, ec="k", lw=0.8))
            if label:
                ax.text((x_to - 0.3) if side < 0 else (x_from + 0.3), (zz[0] + zz[1]) / 2 + zm, label, va="center", ha="right" if side < 0 else "left", fontsize=7.5, color=tc)
    # --- glass seat, fixed side
    xl, xr = px1 - 9.5, px1 + 7.5
    a2.add_patch(Rectangle((cx1 - P["wall"], z["frame_bot"] - 3), P["wall"], z["top"] - z["frame_bot"] + 3, fc=GREY, ec="k", lw=1))
    a2.add_patch(Rectangle((cx1, z["glass"]), gx1 - g - cx1, z["top"] - z["glass"], fc=GREY, ec="k", lw=1))
    a2.add_patch(Rectangle((fx1, z["frame_bot"]), px1 - s - fx1, z["glass"] - z["frame_bot"], fc=DARK, ec="k", lw=1))
    stack(a2, xl, xr, -1)
    a2.text((fx1 + px1) / 2, z["frame_bot"] + 2.2, "frame\nfixed side", ha="center", va="center", fontsize=8, color="w")
    a2.text((cx1 + gx1) / 2 - 0.4, z["top"] + 0.3, f"top skin {z['top'] - z['glass']:.1f} = glass + {P['glass_recess']:.1f}", ha="center", fontsize=8)
    a2.annotate(f"seat {px1 - s - gx1:.2f}", ((gx1 + px1 - s) / 2, z["glass"]), (cx1 + 0.4, z["frame_bot"] - 1.4), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate(f"board against the frame\n({s:.2f} drawn)", (px1 - s / 2, zm - 2.8), (px1 + 1.0, z["frame_bot"] - 2.2), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.annotate(f"gap {g:.2f}", (gx1 - g / 2, z["top"]), (gx1 + 1.5, z["top"] + 1.4), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a2.set_xlim(cx1 - P["wall"] - 0.6, xr); a2.set_ylim(z["frame_bot"] - 3.4, z["top"] + 2.2); a2.set_aspect("equal")
    a2.set_title("Glass on the frame, flush with the top: fixed side"); a2.set_xlabel("x"); a2.set_ylabel("z above the board")
    # --- glass seat, jaw side
    xl, xr = px2 - 7.5, cx2 + P["wall"] + 0.6
    a3.add_patch(Rectangle((cx2, z["frame_bot"] - 3), P["wall"], z["top"] - z["frame_bot"] + 3, fc=GREY, ec="k", lw=1))
    a3.add_patch(Rectangle((gx2 + g, z["glass"]), cx2 - gx2 - g, z["top"] - z["glass"], fc=GREY, ec="k", lw=1))
    t, sl, bite = P["jaw_t"], P["jaw_slot"], P["jaw_bite"]
    a3.add_patch(Rectangle((jx, z["frame_bot"]), t, z["glass"] - z["frame_bot"], fc=PART, ec="k", lw=1))
    xp, zc = px2 - bite, zm - 0.1
    a3.add_patch(Polygon([(jx, z["frame_bot"]), (xp, z["frame_bot"]), (xp, zc), (jx, zc + jx - xp)], closed=True, fc=PART, ec="k", lw=1))
    a3.add_patch(Rectangle((jx + t + sl, z["frame_bot"]), fx2 - jx - t - sl, z["glass"] - z["frame_bot"], fc=DARK, ec="k", lw=1))
    stack(a3, xl, xr, 1)
    a3.text(jx + t / 2, z["frame_bot"] + 1.2, "jaw", ha="center", va="center", fontsize=8)
    a3.text((jx + t + sl + fx2) / 2, z["frame_bot"] + 2.6, "frame", ha="center", va="center", fontsize=8, color="w", rotation=90)
    a3.annotate(f"pad: drawn unbent, {bite:.1f} into the\nboard's place; it bears on board\nand spacer, not on the LCD", (xp + 0.2, zm - 2.0), (px2 - 7.0, z["frame_bot"] - 2.2),
                arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a3.annotate("ramp: the board pushes\nthe jaw aside on its way in", ((jx + xp) / 2, zc + 0.5), (px2 - 7.0, z["top"] + 0.8), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a3.annotate(f"slot {sl:.1f}", (jx + t + sl / 2, z["glass"] - 1.0), (jx + t - 0.5, z["top"] + 1.2), arrowprops=dict(arrowstyle="->"), fontsize=7.5)
    a3.set_xlim(xl, xr); a3.set_ylim(z["frame_bot"] - 3.4, z["top"] + 2.2); a3.set_aspect("equal")
    a3.set_title(f"Jaw side: each jaw {t:.1f} x {P['frame_t']:.0f}, {P['jaw_l']:.0f} long, flexes in the plane of the frame"); a3.set_xlabel("x")
    # --- a key: section through its nose (outside on the left)
    k, u = geo["key"], geo["usb"]
    oy2, cy2 = u["oy2"], u["cy2"]
    kz, kg = k["z"], k["geo"]["BOOT"]
    zn, znb = z["nose"], z["nose"] - P["key_nose_h"]
    y_t = k["tip_y"] + P["key_gap"]
    cc = max(kg["y_in"] - y_t - P["key_ledge"], 0)
    yl_ = oy2 - P["key_land"]
    a4.add_patch(Polygon([(cy2, z["io_top"] + yl_ - cy2), (yl_, z["io_top"]), (oy2, z["io_top"]), (oy2, z["top"] - 1.2), (oy2 - 1.2, z["top"]), (cy2, z["top"])], closed=True, fc=GREY, ec="k", lw=1))
    a4.add_patch(Rectangle((cy2 + 0.05, z["tongue"] - 4), oy2 - cy2 - 0.05, 4, fc=PART, ec="k", lw=1))
    head = [(kg["y_in"], z["glass"]), (kg["y_in"], zn), (y_t + cc, zn), (y_t, zn - cc), (y_t, znb), (fy2, znb), (fy2, kg["z_bot"]), (kg["y_f"] - 0.4, kg["z_bot"]),
            (kg["y_f"], kg["z_bot"] + 0.4), (kg["y_f"], kg["z0"]), (fy2, kg["z0"] + kg["y_f"] - fy2), (fy2, z["glass"])]
    a4.add_patch(Polygon(head, closed=True, fc="#2a7fd8", ec="k", lw=1))
    pe = k["pcb_edge_y"]
    yl = pe - 7.5
    a4.add_patch(Rectangle((yl, ws["glass_z"][0] + zm), k["glass_y"] - yl, 1.1, fc=C_GLASS, ec="k", lw=0.8))
    a4.add_patch(Rectangle((yl, ws["lcm_z"][0] + zm), pe - 0.01 - yl, 2.1, fc=C_LCD, ec="k", lw=0.8))
    a4.add_patch(Rectangle((yl, ws["pmma_z"][0] + zm), pe - 0.21 - yl, 2.0, fc=C_SPACER, ec="k", lw=0.8))
    a4.add_patch(Rectangle((yl, k["pcb_z"][0]), pe - yl, 1.6, fc=C_PCB, ec="k", lw=0.8))
    aw, ah, ap = k["act"]
    a4.add_patch(Rectangle((k["tip_y"] - ap - k["body_d"], k["body_z"][0]), k["body_d"], k["body_z"][1] - k["body_z"][0], fc="#f1efe6", ec="k", lw=0.8))
    a4.add_patch(Rectangle((k["tip_y"] - ap, kz - ah / 2), ap, ah, fc="#3a3a3d", ec="k", lw=0.8))
    a4.text(pe - 0.6, k["pcb_z"][0] + 0.8, "module board", fontsize=7.5, color="w", va="center", ha="left")
    a4.text((kg["y_in"] + fy2) / 2, z["glass"] - 2.6, "head of\nthe key", fontsize=8, ha="center", va="center", color="w")
    a4.text(oy2 + 0.6, z["tongue"] - 1.2, "tongue of the base", fontsize=7.5, ha="right", va="center")
    xo = oy2 + 12.0
    a4.annotate(f"paddle: {P['key_under']['BOOT']:.1f} below the outer face\n(RST: {P['key_under']['RST']:.1f})", (kg["y_f"], kz - 1.0), (xo, kz - 1.5), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left", va="center")
    a4.annotate("45 deg under the bevel of the wall:\nprints without support, seat down", ((fy2 + kg["y_f"]) / 2, kg["z0"] + (kg["y_f"] - fy2) / 2), (xo, z["top"] + 1.4), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left", va="center")
    a4.annotate(f"nose: {P['key_gap']:.1f} before the plunger;\nledge {P['key_ledge']:.1f}, then 45 deg", (y_t + 0.2, zn - 0.5), (pe - 0.3, znb - 3.2), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a4.annotate(f"clear of the board also at the\nstop ({P['key_stroke']:.1f} of travel)", (kg["y_in"], k["pcb_z"][0] + 0.8), (pe + 3.0, z["top"] + 1.4), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left", va="center")
    a4.set_xlim(oy2 + 12.5, yl - 0.5); a4.set_ylim(znb - 5.0, z["top"] + 3.0); a4.set_aspect("equal")
    a4.set_title("Key (BOOT), section through its nose: one piece with the frame"); a4.set_xlabel("y (outside on the left)"); a4.set_ylabel("z above the board")
    # --- USB-C end seen from outside
    ux, uz, plug = u["x"], u["z"], u["plug"]
    io1, io2 = geo["io_x"]
    zlo = z["tongue"] - 9.0
    a5.add_patch(Rectangle((io1 - 9, zlo), io2 - io1 + 18, z["top"] - zlo, fc=GREY, ec="k", lw=1))
    a5.add_patch(Rectangle((io1, zlo), io2 - io1, z["io_top"] - zlo, fc="#2b2c30", ec="k", lw=1))
    a5.add_patch(Rectangle((io1 + P["cut_clr"], zlo), io2 - io1 - 2 * P["cut_clr"], z["tongue"] - zlo, fc=PART, ec="k", lw=1))
    for name, kx in k["x"].items():
        q = k["geo"][name]
        xa, xb = sorted((q["x_in"], q["x_out"]))
        zt = min(q["z0"], z["io_top"])                 # a deep paddle reaches up behind the wall: what shows ends at the opening's edge
        a5.add_patch(Rectangle((xa, q["z_bot"]), xb - xa, zt - q["z_bot"], fc="#2a7fd8", ec="k", lw=1))
        a5.text((xa + xb) / 2, q["z_bot"] + 1.5, f"{name}\n{P['key_under'][name]:.1f} deep", ha="center", va="center", fontsize=7.5, color="w")
        a5.plot([kx], [kz], marker="+", color="w", ms=6, mew=1.2)
    sw, sh = u["shell"]
    a5.add_patch(Rectangle((ux - sw / 2, uz - sh / 2), sw, sh, fc="#c9cacd", ec="k", lw=0.8))
    a5.add_patch(Rectangle((ux - u["open"][0] / 2, uz - u["open"][1] / 2), u["open"][0], u["open"][1], fc="#111", ec="k", lw=0.6))
    a5.add_patch(Rectangle((ux - plug["over_w"] / 2, uz - plug["over_h"] / 2), plug["over_w"], plug["over_h"], fill=False, ec="#e3c14f", lw=1.3, ls="--"))
    a5.annotate(f"largest plug grip the standard allows\n{plug['over_w']:.2f} x {plug['over_h']:.1f}; room {P['usb_w'] + 2 * P['key_clr']:.1f} x {P['usb_h'] + P['key_clr']:.1f}", (ux - plug["over_w"] / 2, uz - 2.6), (io1 - 10.0, z["tongue"] - 5.0),
                arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a5.annotate("paddles: the white cross is\nthe switch behind", (k["x"]["RST"], kz + 1.5), (io1 - 10.0, z["io_top"] + 2.0), arrowprops=dict(arrowstyle="->"), fontsize=7.5, ha="left")
    a5.text(ux, z["tongue"] - 4.5, "tongue of the base", ha="center", va="center", fontsize=8)
    a5.text(io2 + 4.5, (zlo + z["top"]) / 2, "body", ha="center", va="center", fontsize=8, rotation=90)
    a5.annotate("", (io1, zlo + 1.5), (io2, zlo + 1.5), arrowprops=dict(arrowstyle="<->"))
    a5.text(ux, zlo + 2.0, f"opening {io2 - io1:.1f}", ha="center", fontsize=8)
    a5.set_xlim(io2 + 9.5, io1 - 21); a5.set_ylim(zlo - 0.5, z["top"] + 3.0); a5.set_aspect("equal")      # seen from outside: x runs to the left
    a5.set_title("USB-C end from outside: body, the two paddles of the frame, tongue of the base"); a5.set_xlabel("x (seen from outside: BOOT on the left)"); a5.set_ylabel("z above the board")
    # --- the frame from above
    a6.add_patch(Rectangle((cx1 - P["wall"], fy1 - 6), cx2 - cx1 + 2 * P["wall"], fy2 - fy1 + 6 + 0.2 + P["wall"], fc=GREY, ec="k", lw=1))
    a6.add_patch(Rectangle((cx1, fy1 - 6), cx2 - cx1, fy2 - fy1 + 6.2, fc="white", ec="k", lw=0.8))
    a6.add_patch(Rectangle((fx1, fy1), fx2 - fx1, fy2 - fy1, fc=DARK, ec="k", lw=1))
    a6.add_patch(Rectangle((px1 - s, py1 - s), jx - px1 + s, py2 - py1 + 2 * s, fc="white", ec="k", lw=0.8))
    ym = (py1 + py2) / 2
    ja, jb = geo["jaw_y"]
    for ya, yb in ((ja, ym - P["jaw_root"] / 2), (ym + P["jaw_root"] / 2, jb)):
        a6.add_patch(Rectangle((jx, ya), t, yb - ya, fc=PART, ec="k", lw=0.8))
        a6.add_patch(Rectangle((jx + t, ya), sl, yb - ya, fc="white", ec="k", lw=0.5))
    for tipy, sg in ((ja, 1), (jb, -1)):
        a6.add_patch(Rectangle((px2 - bite, min(tipy, tipy + sg * P["jaw_pad"])), jx - px2 + bite, P["jaw_pad"], fc="#b0362b", ec="k", lw=0.6))
    a6.add_patch(Rectangle((px1, py1), px2 - px1, py2 - py1, fill=False, ec=C_PCB, lw=1.4))
    a6.add_patch(Rectangle((gx1, gy1), gx2 - gx1, gy2 - gy1, fill=False, ec="k", lw=1.0, ls=":"))
    for x1, y1, x2, y2 in geo["legs"]:
        a6.add_patch(Rectangle((x1, y1), x2 - x1, y2 - y1, fill=False, ec="#b0362b", lw=1.2, hatch="///"))
    # USB-C end: open between the keys; each key is a head on a leaf, with a slot behind the leaf
    xf = sorted(q["x_fix"] for q in k["geo"].values())
    a6.add_patch(Rectangle((xf[0], k["stop_y"] - 0.3), xf[1] - xf[0], fy2 - k["stop_y"] + 0.6, fc="white", ec="none"))
    lt, ls_ = P["key_leaf_t"], P["key_slot"]
    for q in k["geo"].values():
        xa, xb = sorted((q["x_root"], q["x_fix"]))
        a6.add_patch(Rectangle((xa, fy2 - lt - ls_), xb - xa, ls_, fc="white", ec="k", lw=0.5))
        xa, xb = sorted((q["x_root"], q["x_out"]))
        a6.add_patch(Rectangle((xa, fy2 - lt), xb - xa, lt, fc="#2a7fd8", ec="k", lw=0.5))
        xa, xb = sorted((q["x_out"], q["x_in"]))
        a6.add_patch(Rectangle((xa, q["y_in"]), xb - xa, q["y_f"] - q["y_in"], fc="#2a7fd8", ec="k", lw=0.8))
    a6.text((px1 + px2) / 2, ym + 9, "module board\n(blue outline)", ha="center", va="center", fontsize=8, color=C_PCB)
    a6.text((px1 + px2) / 2, ym - 9, "glass (dotted)", ha="center", va="center", fontsize=8)
    a6.annotate("fixed side", (px1 - s, ym), (cx1 - P["wall"] - 9.5, ym), arrowprops=dict(arrowstyle="->"), fontsize=8, va="center")
    a6.annotate("two jaws,\npads in red", (jx + t / 2, ym + 12), (cx2 + P["wall"] + 1.5, ym + 12), arrowprops=dict(arrowstyle="->"), fontsize=8, va="center")
    a6.annotate("legs\n(hatched)", (geo["legs"][-1][2] - 1.0, geo["legs"][-1][1] + 2), (cx2 + P["wall"] + 1.5, geo["legs"][-1][1] - 3), arrowprops=dict(arrowstyle="->"), fontsize=8, va="center")
    qb = k["geo"]["BOOT"]
    a6.annotate("key (blue): head on a leaf\nspring, one piece with\nthe frame", ((qb["x_out"] + qb["x_root"]) / 2, fy2 - lt / 2), (cx2 + P["wall"] + 1.5, fy2 - 6), arrowprops=dict(arrowstyle="->"), fontsize=8, va="center")
    a6.annotate("end stops of the board", (px1 + 3, py1 - s), (cx1 - P["wall"] - 9.5, py1 - 4), arrowprops=dict(arrowstyle="->"), fontsize=8, va="center")
    a6.set_xlim(cx1 - P["wall"] - 10, cx2 + P["wall"] + 16); a6.set_ylim(fy1 - 7, fy2 + P["wall"] + 2.5); a6.set_aspect("equal")
    a6.set_title("The frame from above (USB-C end at the top)"); a6.set_xlabel("x"); a6.set_ylabel("y")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "detail_sections.png"), dpi=110)


if __name__ == "__main__":
    sys.exit(main())
