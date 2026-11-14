"""Photoreal Cycles render of an RS232 Console enclosure (Waveshare ESP32-S3-Touch-LCD-2).

Run: blender --background --python build_case.py -- [samples]
Outputs render/rs232_case.png and rs232_case.blend next to this script.
Units are metres (real scale, so light falloff and depth of field behave physically).
"""
import bpy, bmesh, math, os, sys
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SAMPLES = int(argv[0]) if argv else 512
MM = 0.001

# Enclosure dimensions (mm). Display: 2.0" 240x320 portrait, active area ~30.6 x 40.8.
W, D, H = 50.0, 74.0, 15.0          # body footprint X/Y and height
GLASS_T = 1.2
DISP_W, DISP_D = 30.6, 40.8
R_CORNER = 7.0

# ---------------------------------------------------------------- scene reset
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.system = 'METRIC'


def rounded_box(name, w, d, h, r, bevel_h, segs=8):
    """Box with rounded vertical corners (radius r) and soft top/bottom edges."""
    bpy.ops.mesh.primitive_cube_add(size=1)
    ob = bpy.context.object
    ob.name = name
    ob.scale = (w * MM, d * MM, h * MM)
    bpy.ops.object.transform_apply(scale=True)
    # vertical corners
    bm = bmesh.new(); bm.from_mesh(ob.data)
    vert_edges = [e for e in bm.edges if abs(e.verts[0].co.z - e.verts[1].co.z) > 1e-9]
    bmesh.ops.bevel(bm, geom=vert_edges, offset=r * MM, segments=segs, affect='EDGES', profile=0.5)
    bm.to_mesh(ob.data); bm.free()
    # soft edges everywhere else (catches highlights like moulded plastic)
    m = ob.modifiers.new("soft", 'BEVEL')
    m.width = bevel_h * MM; m.segments = 4; m.limit_method = 'ANGLE'
    m.angle_limit = math.radians(60)
    ob.data.shade_smooth() if hasattr(ob.data, "shade_smooth") else None
    for p in ob.data.polygons: p.use_smooth = True
    return ob


def boolean(target, cutter, op='DIFFERENCE'):
    m = target.modifiers.new("bool_" + cutter.name, 'BOOLEAN')
    m.operation = op; m.object = cutter; m.solver = 'EXACT'
    cutter.hide_render = True; cutter.display_type = 'WIRE'


def apply_all(ob):
    bpy.context.view_layer.objects.active = ob
    for m in list(ob.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


# ---------------------------------------------------------------- materials
def mat(name):
    m = bpy.data.materials.new(name); m.use_nodes = True
    return m, m.node_tree.nodes, m.node_tree.links, m.node_tree.nodes["Principled BSDF"]


def plastic():
    m, n, l, b = mat("Plastic_matte")
    b.inputs["Base Color"].default_value = (0.016, 0.017, 0.020, 1)
    b.inputs["Roughness"].default_value = 0.48
    b.inputs["Specular IOR Level"].default_value = 0.45
    # fine moulding grain -> bump
    tex = n.new("ShaderNodeTexNoise"); tex.inputs["Scale"].default_value = 9000; tex.inputs["Detail"].default_value = 8
    bump = n.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.06; bump.inputs["Distance"].default_value = 0.00002
    l.new(tex.outputs["Fac"], bump.inputs["Height"]); l.new(bump.outputs["Normal"], b.inputs["Normal"])
    # very subtle roughness variation (handling marks)
    tex2 = n.new("ShaderNodeTexNoise"); tex2.inputs["Scale"].default_value = 180
    ramp = n.new("ShaderNodeMapRange"); ramp.inputs["To Min"].default_value = 0.42; ramp.inputs["To Max"].default_value = 0.56
    l.new(tex2.outputs["Fac"], ramp.inputs["Value"]); l.new(ramp.outputs["Result"], b.inputs["Roughness"])
    return m


def glass_front():
    m, n, l, b = mat("Glass_front")
    b.inputs["Base Color"].default_value = (0.005, 0.005, 0.006, 1)
    b.inputs["Roughness"].default_value = 0.02
    b.inputs["Coat Weight"].default_value = 1.0
    b.inputs["Coat Roughness"].default_value = 0.01
    b.inputs["IOR"].default_value = 1.52
    return m


def display():
    m, n, l, b = mat("Display")
    img = bpy.data.images.load(os.path.join(HERE, "screen_ui.png"))
    t = n.new("ShaderNodeTexImage"); t.image = img; t.interpolation = 'Cubic'
    b.inputs["Base Color"].default_value = (0, 0, 0, 1)
    b.inputs["Roughness"].default_value = 0.3
    l.new(t.outputs["Color"], b.inputs["Emission Color"])
    b.inputs["Emission Strength"].default_value = 1.6
    b.inputs["Coat Weight"].default_value = 1.0
    b.inputs["Coat Roughness"].default_value = 0.01
    return m


def cover_glass():
    m, n, l, b = mat("Cover_glass")
    b.inputs["Base Color"].default_value = (1, 1, 1, 1)
    b.inputs["Roughness"].default_value = 0.0
    b.inputs["Transmission Weight"].default_value = 1.0
    b.inputs["IOR"].default_value = 1.52
    return m


def metal(name="Metal_port", rough=0.22, col=(0.62, 0.62, 0.64)):
    m, n, l, b = mat(name)
    b.inputs["Base Color"].default_value = (*col, 1)
    b.inputs["Metallic"].default_value = 1.0
    b.inputs["Roughness"].default_value = rough
    return m


def rubber():
    m, n, l, b = mat("Rubber")
    b.inputs["Base Color"].default_value = (0.012, 0.012, 0.012, 1)
    b.inputs["Roughness"].default_value = 0.85
    return m


def solid(name, rgb, rough=0.5):
    m, n, l, b = mat(name)
    b.inputs["Base Color"].default_value = (*rgb, 1)
    b.inputs["Roughness"].default_value = rough
    return m


M_PLASTIC, M_GLASS, M_DISP, M_COVER = plastic(), glass_front(), display(), cover_glass()
M_METAL, M_RUBBER = metal(), rubber()
M_DARK = solid("Port_inside", (0.006, 0.006, 0.006), 0.6)
M_LED = mat("LED")[0]
_b = M_LED.node_tree.nodes["Principled BSDF"]
_b.inputs["Base Color"].default_value = (0.05, 0.4, 0.1, 1)
_b.inputs["Emission Color"].default_value = (0.19, 0.82, 0.35, 1); _b.inputs["Emission Strength"].default_value = 6


def assign(ob, m):
    ob.data.materials.clear(); ob.data.materials.append(m)


# ---------------------------------------------------------------- geometry
body = rounded_box("Body", W, D, H, R_CORNER, 0.9)
body.location.z = H / 2 * MM
assign(body, M_PLASTIC)

# Parting line: thin groove 3 mm below the top
groove = rounded_box("GrooveCut", W + 2, D + 2, 0.35, R_CORNER + 1, 0.05)
inner = rounded_box("GrooveKeep", W - 0.7, D - 0.7, 0.6, R_CORNER - 0.35, 0.05)
groove.location.z = (H - 3.0) * MM; inner.location.z = (H - 3.0) * MM
boolean(groove, inner)  # ring
apply_all(groove); bpy.data.objects.remove(inner)
boolean(body, groove)

# Recess on top for the glass
recess = rounded_box("RecessCut", W - 3.0, D - 3.0, 4, R_CORNER - 1.5, 0.05)
recess.location.z = (H - GLASS_T / 2 + 2 - 0.15) * MM
boolean(body, recess)

# USB-C opening on the bottom short side (-Y)
usb_cut = rounded_box("USBCut", 9.6, 10, 3.8, 1.85, 0.05)
usb_cut.location = (0, (-D / 2) * MM, 4.2 * MM)
boolean(body, usb_cut)

# Side slot for the BOOT button (+X side)
btn_cut = rounded_box("BtnCut", 6, 5.0, 2.8, 1.3, 0.05)
btn_cut.location = ((W / 2) * MM, 12 * MM, 7.5 * MM)
boolean(body, btn_cut)

# microSD slot (+Y side)
sd_cut = rounded_box("SDCut", 13, 6, 1.6, 0.7, 0.05)
sd_cut.location = (0, (D / 2) * MM, 3.0 * MM)
boolean(body, sd_cut)

bpy.context.view_layer.objects.active = body
apply_all(body)
for c in (groove, recess, usb_cut, btn_cut, sd_cut):
    bpy.data.objects.remove(c)

# Glass front panel (black glass with display window printed around)
glass = rounded_box("GlassFront", W - 3.3, D - 3.3, GLASS_T, R_CORNER - 1.65, 0.35)
glass.location.z = (H - GLASS_T / 2 - 0.1) * MM
assign(glass, M_GLASS)

# Display, sitting a hair above the glass surface, under a clear cover layer
DISP_Y = 3.0  # display sits a little toward the top edge, chin for the logo below
bpy.ops.mesh.primitive_plane_add(size=1)
disp = bpy.context.object; disp.name = "Display"
disp.scale = (DISP_W * MM, DISP_D * MM, 1)
disp.location = (0, DISP_Y * MM, (H - 0.1 + 0.005) * MM)
bpy.ops.object.transform_apply(scale=True)
assign(disp, M_DISP)
bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.uv.reset(); bpy.ops.object.mode_set(mode='OBJECT')


# USB-C receptacle: metal shell + dark tongue cavity
shell = rounded_box("USBShell", 8.9, 6, 3.2, 1.55, 0.05)
shell.location = (0, (-D / 2 + 2.2) * MM, 4.2 * MM)
cav = rounded_box("USBCav", 8.1, 7, 2.4, 1.15, 0.05)
cav.location = shell.location.copy(); cav.location.y -= 0.6 * MM
boolean(shell, cav); apply_all(shell); bpy.data.objects.remove(cav)
assign(shell, M_METAL)
tongue = rounded_box("USBTongue", 6.6, 4.5, 0.7, 0.3, 0.05)
tongue.location = (0, (-D / 2 + 3.4) * MM, 4.2 * MM); assign(tongue, M_DARK)
back = rounded_box("USBBack", 9.0, 0.5, 3.4, 1.5, 0.02)
back.location = (0, (-D / 2 + 5.0) * MM, 4.2 * MM); assign(back, M_DARK)

# BOOT button (rubber cap, slightly proud)
btn = rounded_box("BootButton", 3.2, 4.4, 2.2, 1.0, 0.4)
btn.location = ((W / 2 - 1.0) * MM, 12 * MM, 7.5 * MM); assign(btn, M_PLASTIC)

# microSD card peeking out
sd = rounded_box("SDCard", 11, 3, 0.8, 0.4, 0.1)
sd.location = (0, (D / 2 - 0.6) * MM, 3.0 * MM); assign(sd, solid("SD_label", (0.08, 0.09, 0.12), 0.35))

# Status LED pin-hole next to the USB port
led = rounded_box("LED", 1.4, 1.0, 1.4, 0.69, 0.1)
led.location = (9.5 * MM, (-D / 2 + 0.3) * MM, 4.2 * MM); assign(led, M_LED)

# Rubber feet
for sx in (-1, 1):
    for sy in (-1, 1):
        bpy.ops.mesh.primitive_cylinder_add(radius=3.0 * MM, depth=0.8 * MM, vertices=48,
                                            location=(sx * (W / 2 - 7) * MM, sy * (D / 2 - 8) * MM, -0.35 * MM))
        f = bpy.context.object; f.name = "Foot"
        bv = f.modifiers.new("b", 'BEVEL'); bv.width = 0.3 * MM; bv.segments = 3
        for p in f.data.polygons: p.use_smooth = True
        assign(f, M_RUBBER)

# Brand mark printed on the glass chin: connector shell with ">_" (white print)
def print_text(txt, size, loc, name, align='CENTER'):
    bpy.ops.object.text_add(location=loc)
    t = bpy.context.object; t.name = name
    t.data.body = txt; t.data.size = size; t.data.align_x = align; t.data.align_y = 'CENTER'
    fnt = "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"
    if os.path.exists(fnt): t.data.font = bpy.data.fonts.load(fnt)
    t.data.extrude = 0.00002
    return t

M_PRINT = solid("Print_white", (0.85, 0.86, 0.88), 0.35)
top_z = (H - 0.1 + 0.002) * MM
chin_y = (DISP_Y - DISP_D / 2 - 7.0) * MM
label = print_text("RS232 CONSOLE", 2.2 * MM, (2.6 * MM, chin_y, top_z), "Label")
assign(label, M_PRINT)
# small logo tile in accent blue with ">_"
tile = rounded_box("LogoTile", 4.6, 4.6, 0.04, 1.1, 0.01)
tile.location = (-9.3 * MM, chin_y, top_z)
assign(tile, solid("Accent", (0.002, 0.085, 0.55), 0.55))
mark = print_text(">_", 2.4 * MM, (-9.3 * MM, chin_y - 0.1 * MM, top_z + 0.08 * MM), "Mark")
assign(mark, M_PRINT)

# Engraved text on the body side (debossed look via a darker, rougher print)
side = print_text("RS232 · WLAN", 2.0 * MM, ((W / 2 + 0.01) * MM, -16 * MM, 7.2 * MM), "SideText")
side.rotation_euler = (math.radians(90), 0, math.radians(90))
assign(side, solid("Deboss", (0.018, 0.019, 0.022), 0.75))

# ---------------------------------------------------------------- studio
# seamless sweep (floor that curves into a backdrop)
bpy.ops.mesh.primitive_plane_add(size=1)
sweep = bpy.context.object; sweep.name = "Sweep"
sweep.scale = (0.8, 0.8, 1); bpy.ops.object.transform_apply(scale=True)
bpy.ops.object.mode_set(mode='EDIT')
bm = bmesh.from_edit_mesh(sweep.data)
back_edge = [e for e in bm.edges if all(v.co.y > 0.39 for v in e.verts)]
ext = bmesh.ops.extrude_edge_only(bm, edges=back_edge)
for v in [x for x in ext["geom"] if isinstance(x, bmesh.types.BMVert)]:
    v.co.z += 0.6; v.co.y += 0.0
bmesh.update_edit_mesh(sweep.data); bpy.ops.object.mode_set(mode='OBJECT')
sweep.location = (0, 0.12, -0.0008)
bv = sweep.modifiers.new("curve", 'BEVEL'); bv.width = 0.25; bv.segments = 24; bv.limit_method = 'ANGLE'
sub = sweep.modifiers.new("sub", 'SUBSURF'); sub.levels = 1; sub.render_levels = 2
for p in sweep.data.polygons: p.use_smooth = True
mfloor, n, l, b = mat("Floor")
b.inputs["Base Color"].default_value = (0.34, 0.35, 0.37, 1)
b.inputs["Roughness"].default_value = 0.32
b.inputs["Specular IOR Level"].default_value = 0.35
assign(sweep, mfloor)


def area(name, loc, size, energy, color=(1, 1, 1), shape='RECTANGLE', size_y=None, target=(0, 0, 0.008)):
    bpy.ops.object.light_add(type='AREA', location=loc)
    lt = bpy.context.object; lt.name = name
    lt.data.shape = shape; lt.data.size = size; lt.data.size_y = size_y or size
    lt.data.energy = energy; lt.data.color = color
    d = Vector(target) - lt.location
    lt.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    return lt

area("Key",  (-0.28, -0.22, 0.34), 0.30, 9,  (1.0, 0.97, 0.93), size_y=0.18)
area("Fill", (0.30, -0.18, 0.14),  0.40, 2.5,  (0.92, 0.96, 1.0))
area("Rim",  (0.10, 0.34, 0.26),   0.12, 7,  (1.0, 1.0, 1.0), size_y=0.45)
area("Top",  (0.0, 0.0, 0.45),     0.25, 2.5,  (1.0, 1.0, 1.0))  # long glossy streak on the glass

world = bpy.data.worlds.new("World"); scene.world = world; world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.020, 0.021, 0.024, 1); bg.inputs["Strength"].default_value = 1.0

# ---------------------------------------------------------------- camera
bpy.ops.object.camera_add(location=(0.16, -0.235, 0.16))
cam = bpy.context.object; scene.camera = cam
focus = bpy.data.objects.new("Focus", None); scene.collection.objects.link(focus)
focus.location = (0.0, -0.004, 0.011)
tc = cam.constraints.new('TRACK_TO'); tc.target = focus; tc.track_axis = 'TRACK_NEGATIVE_Z'; tc.up_axis = 'UP_Y'
cam.data.lens = 85; cam.data.sensor_width = 36
cam.data.dof.use_dof = True; cam.data.dof.focus_object = focus; cam.data.dof.aperture_fstop = 14

# ---------------------------------------------------------------- clean shading
# Apply modifiers, then smooth by angle: rounded corners/bevels (<=22.5 deg steps) shade
# smooth, real 90-degree edges and flat boolean faces stay crisp.
for ob in list(bpy.data.objects):
    if ob.type != 'MESH':
        continue
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True); bpy.context.view_layer.objects.active = ob
    for m in list(ob.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.ops.object.shade_smooth_by_angle(angle=math.radians(30))

# ---------------------------------------------------------------- render
scene.render.engine = 'CYCLES'
cy = scene.cycles
prefs = bpy.context.preferences.addons["cycles"].preferences
device = "CPU"
for kind in ("OPTIX", "CUDA"):
    try:
        prefs.compute_device_type = kind; prefs.get_devices()
        gpus = [d for d in prefs.devices if d.type == kind]
        if gpus:
            for d in prefs.devices: d.use = (d.type == kind)
            cy.device = 'GPU'; device = kind; break
    except Exception:
        pass
print(f"[render] device={device} samples={SAMPLES}")
cy.samples = SAMPLES
cy.use_adaptive_sampling = True; cy.adaptive_threshold = 0.006
cy.use_denoising = True
cy.denoiser = 'OPTIX' if device == "OPTIX" else 'OPENIMAGEDENOISE'
cy.max_bounces = 16; cy.glossy_bounces = 8; cy.transmission_bounces = 12
cy.caustics_reflective = False; cy.caustics_refractive = False
scene.render.resolution_x = 2400; scene.render.resolution_y = 1600; scene.render.resolution_percentage = 100
scene.render.film_transparent = False
vs = scene.view_settings
try:
    vs.view_transform = 'AgX'; vs.look = 'AgX - Medium High Contrast'
except TypeError:
    vs.view_transform = 'Filmic'
vs.exposure = 0.0
scene.render.image_settings.file_format = 'PNG'; scene.render.image_settings.color_depth = '16'

out = os.path.join(HERE, "render", "rs232_case.png")
scene.render.filepath = out
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "rs232_case.blend"))
bpy.ops.render.render(write_still=True)
print(f"[render] wrote {out}")
