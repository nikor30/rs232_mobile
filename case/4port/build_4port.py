"""Photoreal Cycles render of the four-port RS232 console enclosure (hardware-carrier branch, rev 0.3.1).

The case itself is the real printable geometry: body.stl and base.stl from
hardware/enclosure/generate_enclosure.py. Everything the STLs don't contain (the
Waveshare module's glass and screen, the four RJ45 jacks, USB-C, carrier PCB,
RP-SMA bulkhead, rubber feet) is modelled here at the positions from
rs232_console_case.scad (mm, origin at the carrier PCB's corner, PCB top at z=0).

Run: blender --background --python build_4port.py -- [samples] [views]
     views: ports | front | ports,front (default both)
Outputs render/rs232_4port_<view>.png and rs232_4port.blend next to this script.
"""
import bpy, bmesh, math, os, sys
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SAMPLES = int(argv[0]) if argv else 512
VIEWS = (argv[1] if len(argv) > 1 else "ports,front").split(",")
MM = 0.001

# ---- geometry from rs232_console_case.scad -------------------------------------------
PCB_W, PCB_H, PCB_T = 78.0, 107.0, 1.6
Z_TOP = 18.2
CX = 39.0                                   # module centre line
GLASS_W, GLASS_L, GLASS_R = 37.1, 58.8, 2.6
GLASS_YC = 1.0 + 48.2 / 2                   # ws_y0 + ws_l/2
Z_GLASS_BOT = 12.6
DISP_W, DISP_D, DISP_YC = 30.6, 40.8, 27.5  # 2.0" 240x320 active area
CH_X0, CH_PITCH = 10.4, 19.0
USB_X, USB_Z = 39.0, 9.4
SMA_Y, SMA_Z = 12.0, 7.5
OUT_X2 = 80.5
Z_BASE_BOT = -12.6
FOOT_XY = [(7.8, 7.3), (70.2, 7.3), (7.8, 99.2), (70.2, 99.2)]
FOOT_H = 1.5
Z_FLOOR = Z_BASE_BOT - FOOT_H
CENTER = Vector((CX * MM, 52.5 * MM, Z_FLOOR * MM))

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


# ---- helpers ------------------------------------------------------------------------
def box(name, x1, y1, z1, x2, y2, z2):
    bpy.ops.mesh.primitive_cube_add(size=1)
    o = bpy.context.object; o.name = name
    o.scale = ((x2 - x1) * MM, (y2 - y1) * MM, (z2 - z1) * MM)
    o.location = ((x1 + x2) / 2 * MM, (y1 + y2) / 2 * MM, (z1 + z2) / 2 * MM)
    bpy.ops.object.transform_apply(location=False, scale=True)
    return o


def rbox(name, x1, y1, z1, x2, y2, z2, r, soft=0.0, segs=8):
    o = box(name, x1, y1, z1, x2, y2, z2)
    bm = bmesh.new(); bm.from_mesh(o.data)
    ve = [e for e in bm.edges if abs(e.verts[0].co.z - e.verts[1].co.z) > 1e-9]
    bmesh.ops.bevel(bm, geom=ve, offset=r * MM, segments=segs, affect='EDGES', profile=0.5)
    bm.to_mesh(o.data); bm.free()
    if soft:
        m = o.modifiers.new("soft", 'BEVEL'); m.width = soft * MM; m.segments = 3
        m.limit_method = 'ANGLE'; m.angle_limit = math.radians(60)
    return o


def cyl(name, r, depth, loc, rot=(0, 0, 0), verts=64):
    bpy.ops.mesh.primitive_cylinder_add(radius=r * MM, depth=depth * MM, vertices=verts,
                                        location=tuple(v * MM for v in loc), rotation=rot)
    o = bpy.context.object; o.name = name
    return o


def cut(target, cutter, apply=True):
    m = target.modifiers.new("cut", 'BOOLEAN'); m.operation = 'DIFFERENCE'; m.object = cutter; m.solver = 'EXACT'
    if apply:
        bpy.context.view_layer.objects.active = target
        bpy.ops.object.modifier_apply(modifier=m.name)
        bpy.data.objects.remove(cutter)


def mat(name, base=(0.5, 0.5, 0.5), rough=0.5, metal=0.0, **kw):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*base, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    for k, v in kw.items():
        b.inputs[k].default_value = v
    return m


def assign(o, m):
    o.data.materials.clear(); o.data.materials.append(m)


def printed_plastic(name, base):
    """FDM print: matte, with 0.2 mm layer lines along Z (bump) and fine grain."""
    m = mat(name, base, 0.55, **{"Specular IOR Level": 0.4})
    n, l = m.node_tree.nodes, m.node_tree.links
    b = n["Principled BSDF"]
    tc = n.new("ShaderNodeTexCoord")
    wave = n.new("ShaderNodeTexWave"); wave.wave_type = 'BANDS'; wave.bands_direction = 'Z'
    wave.inputs["Scale"].default_value = 3140.0            # ~0.2 mm period in metres
    wave.inputs["Distortion"].default_value = 0.4; wave.inputs["Detail"].default_value = 1.0
    l.new(tc.outputs["Object"], wave.inputs["Vector"])
    grain = n.new("ShaderNodeTexNoise"); grain.inputs["Scale"].default_value = 6000; grain.inputs["Detail"].default_value = 6
    mix = n.new("ShaderNodeMath"); mix.operation = 'MULTIPLY_ADD'
    mix.inputs[1].default_value = 0.25
    l.new(grain.outputs["Fac"], mix.inputs[0]); l.new(wave.outputs["Fac"], mix.inputs[2])
    bump = n.new("ShaderNodeBump"); bump.inputs["Strength"].default_value = 0.22; bump.inputs["Distance"].default_value = 0.00004
    l.new(mix.outputs["Value"], bump.inputs["Height"]); l.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


# ---- materials ------------------------------------------------------------------------
M_BODY = printed_plastic("PLA_body", (0.40, 0.39, 0.37))
M_BASE = printed_plastic("PLA_base", (0.20, 0.20, 0.20))
M_GLASS = mat("Glass_black", (0.004, 0.004, 0.005), 0.02, **{"Coat Weight": 1.0, "Coat Roughness": 0.01, "IOR": 1.52})
M_MODULE = mat("Module_frame", (0.01, 0.01, 0.012), 0.45)
M_SHIELD = mat("RJ45_shield", (0.70, 0.71, 0.72), 0.32, 1.0)
M_JACK = mat("RJ45_plastic", (0.012, 0.012, 0.013), 0.55)
M_GOLD = mat("Gold", (1.0, 0.72, 0.36), 0.18, 1.0)
M_PCB = mat("Soldermask", (0.03, 0.16, 0.08), 0.32, **{"Coat Weight": 0.5, "Coat Roughness": 0.15})
M_USB = mat("USB_shell", (0.66, 0.66, 0.68), 0.2, 1.0)
M_DARK = mat("Dark", (0.005, 0.005, 0.005), 0.6)
M_RUBBER = mat("Rubber", (0.012, 0.012, 0.012), 0.85)
M_DISP = mat("Display", (0, 0, 0), 0.3, **{"Coat Weight": 1.0, "Coat Roughness": 0.01, "Emission Strength": 1.5})
_dn = M_DISP.node_tree.nodes
IMG = {k: bpy.data.images.load(os.path.join(HERE, f)) for k, f in
       (("front", "screen_4port.png"), ("ports", "screen_4port_r.png"))}
disp_tex = _dn.new("ShaderNodeTexImage"); disp_tex.image = IMG["front"]; disp_tex.interpolation = 'Cubic'
M_DISP.node_tree.links.new(disp_tex.outputs["Color"], _dn["Principled BSDF"].inputs["Emission Color"])

# ---- the real printed case ------------------------------------------------------------
for fn, m in (("body.stl", M_BODY), ("base.stl", M_BASE)):
    bpy.ops.wm.stl_import(filepath=os.path.join(HERE, fn), global_scale=MM)
    o = bpy.context.selected_objects[0]; o.name = fn[:-4].capitalize(); assign(o, m)

# ---- Waveshare module: glass + LCD stack in the window --------------------------------
g = rbox("Glass", CX - GLASS_W / 2, GLASS_YC - GLASS_L / 2, Z_TOP - 1.1, CX + GLASS_W / 2, GLASS_YC + GLASS_L / 2, Z_TOP, GLASS_R, soft=0.3)
assign(g, M_GLASS)
st = rbox("ModuleStack", CX - GLASS_W / 2 + 0.3, GLASS_YC - GLASS_L / 2 + 0.3, Z_GLASS_BOT, CX + GLASS_W / 2 - 0.3, GLASS_YC + GLASS_L / 2 - 0.3, Z_TOP - 1.1, GLASS_R - 0.3)
assign(st, M_MODULE)
bpy.ops.mesh.primitive_plane_add(size=1, location=(CX * MM, DISP_YC * MM, (Z_TOP + 0.005) * MM))
dp = bpy.context.object; dp.name = "Display"; dp.scale = (DISP_W * MM, DISP_D * MM, 1)
bpy.ops.object.transform_apply(scale=True); assign(dp, M_DISP)

# ---- carrier PCB ----------------------------------------------------------------------
assign(box("CarrierPCB", 0, 0, -PCB_T, PCB_W, PCB_H, 0), M_PCB)

# ---- four RJ45 jacks (shielded, latch down), front face 3 mm behind the wall openings --
for i in range(4):
    c = CH_X0 + i * CH_PITCH
    y1, y2 = PCB_H - 21.9, PCB_H - 0.6
    house = box(f"RJ45_{i+1}_housing", c - 7.6, y1 + 0.3, 0, c + 7.6, y2 - 0.3, 13.2); assign(house, M_JACK)
    shell = box(f"RJ45_{i+1}_shield", c - 7.95, y1, 0, c + 7.95, y2, 13.5)
    cut(shell, box("in", c - 7.65, y1 - 1, -1, c + 7.65, y2 - 0.3, 13.2))
    assign(shell, M_SHIELD)
    for o in (house, shell):  # modular-jack opening: 11.7 wide body + latch slot at the bottom
        cut(o, box("open", c - 5.85, y2 - 14, 4.4, c + 5.85, y2 + 2, 12.0))
        cut(o, box("latch", c - 3.1, y2 - 14, 1.8, c + 3.1, y2 + 2, 4.5))
    for k in range(8):          # contact fingers along the top of the opening
        x = c - 4.445 + k * 1.27
        f = box(f"RJ45_{i+1}_pin{k}", x - 0.2, y2 - 9.5, 10.9, x + 0.2, y2 - 3.0, 11.3)
        f.rotation_euler.x = math.radians(-14); assign(f, M_GOLD)
    # two grounding tabs on the shield's top front edge
    for dx in (-5.0, 5.0):
        assign(box(f"RJ45_{i+1}_tab", c + dx - 1.0, y2 - 2.0, 13.5, c + dx + 1.0, y2 - 0.2, 13.75), M_SHIELD)

# ---- USB-C receptacle of the module + dark interior behind the end wall ---------------
assign(box("EndBackdrop", 24, -0.8, 4.5, 54, 8, 13.0), M_DARK)
# horizontal stadium tube: round the edges that run along Y
usb = box("USB_shell", USB_X - 4.47, -0.9, USB_Z - 1.63, USB_X + 4.47, 6.5, USB_Z + 1.63)
bm = bmesh.new(); bm.from_mesh(usb.data)
ye = [e for e in bm.edges if abs(e.verts[0].co.y - e.verts[1].co.y) > 1e-9]
bmesh.ops.bevel(bm, geom=ye, offset=1.55 * MM, segments=8, affect='EDGES', profile=0.5)
bm.to_mesh(usb.data); bm.free()
inner = box("usbin", USB_X - 4.1, -2, USB_Z - 1.25, USB_X + 4.1, 5.5, USB_Z + 1.25)
bm = bmesh.new(); bm.from_mesh(inner.data)
ye = [e for e in bm.edges if abs(e.verts[0].co.y - e.verts[1].co.y) > 1e-9]
bmesh.ops.bevel(bm, geom=ye, offset=1.2 * MM, segments=8, affect='EDGES', profile=0.5)
bm.to_mesh(inner.data); bm.free()
cut(usb, inner); assign(usb, M_USB)
assign(box("USB_tongue", USB_X - 3.3, 0.2, USB_Z - 0.35, USB_X + 3.3, 5.0, USB_Z + 0.35), M_DARK)

# ---- RP-SMA bulkhead jack on the right wall --------------------------------------------
rot_x = (0, math.radians(90), 0)
nut = cyl("SMA_nut", 4.6, 2.0, (OUT_X2 + 1.0, SMA_Y, SMA_Z), rot_x, verts=6); assign(nut, M_GOLD)
barrel = cyl("SMA_barrel", 3.15, 8.5, (OUT_X2 + 4.25, SMA_Y, SMA_Z), rot_x)
thr = barrel.modifiers.new("thread", 'BEVEL'); thr.width = 0.2 * MM
assign(barrel, M_GOLD)
assign(cyl("SMA_bore", 2.0, 1.0, (OUT_X2 + 8.6, SMA_Y, SMA_Z), rot_x), M_DARK)
assign(cyl("SMA_washer", 4.2, 0.5, (OUT_X2 + 2.25, SMA_Y, SMA_Z), rot_x), M_SHIELD)

# ---- rubber feet in the base recesses --------------------------------------------------
for x, y in FOOT_XY:
    f = cyl("Foot", 4.0, FOOT_H + 0.6, (x, y, Z_BASE_BOT - FOOT_H / 2 + 0.3))
    b = f.modifiers.new("b", 'BEVEL'); b.width = 0.4 * MM; b.segments = 3
    assign(f, M_RUBBER)

# ---- studio rig (sweep + lights + camera), rotated as one per view ---------------------
rig = bpy.data.objects.new("Rig", None); scene.collection.objects.link(rig); rig.location = CENTER


def to_rig(o):
    o.parent = rig; o.matrix_parent_inverse = rig.matrix_world.inverted()

bpy.ops.mesh.primitive_plane_add(size=1)
sweep = bpy.context.object; sweep.name = "Sweep"; sweep.scale = (1.2, 1.2, 1)
bpy.ops.object.transform_apply(scale=True)
bpy.ops.object.mode_set(mode='EDIT'); bm = bmesh.from_edit_mesh(sweep.data)
back = [e for e in bm.edges if all(v.co.y > 0.59 for v in e.verts)]
ext = bmesh.ops.extrude_edge_only(bm, edges=back)
for v in [x for x in ext["geom"] if isinstance(x, bmesh.types.BMVert)]: v.co.z += 0.9
bmesh.update_edit_mesh(sweep.data); bpy.ops.object.mode_set(mode='OBJECT')
sweep.location = CENTER + Vector((0, 0.28, 0))
b = sweep.modifiers.new("curve", 'BEVEL'); b.width = 0.35; b.segments = 24; b.limit_method = 'ANGLE'
assign(sweep, mat("Floor", (0.36, 0.37, 0.39), 0.34, **{"Specular IOR Level": 0.35}))
to_rig(sweep)

target = CENTER + Vector((0, 0, 0.012))


def area(name, off, size, energy, color=(1, 1, 1), size_y=None):
    bpy.ops.object.light_add(type='AREA', location=CENTER + Vector(off))
    lt = bpy.context.object; lt.name = name
    lt.data.shape = 'RECTANGLE'; lt.data.size = size; lt.data.size_y = size_y or size
    lt.data.energy = energy; lt.data.color = color
    lt.rotation_euler = (target - lt.location).to_track_quat('-Z', 'Y').to_euler()
    to_rig(lt)

# default orientation = "front" view (camera at -y,+x looking at the USB-C end)
area("Key",  (-0.40, -0.30, 0.48), 0.42, 14, (1.0, 0.97, 0.93), size_y=0.25)
area("Fill", (0.42, -0.25, 0.20), 0.55, 5, (0.92, 0.96, 1.0))
area("Rim",  (0.14, 0.48, 0.36), 0.18, 14, size_y=0.6)
area("Top",  (0.0, 0.0, 0.62), 0.35, 5)

world = bpy.data.worlds.new("World"); scene.world = world; world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.02, 0.021, 0.024, 1)

bpy.ops.object.camera_add(location=CENTER + Vector((0.286, -0.338, 0.30)))
cam = bpy.context.object; scene.camera = cam; to_rig(cam)
focus = bpy.data.objects.new("Focus", None); scene.collection.objects.link(focus); focus.location = target
tc = cam.constraints.new('TRACK_TO'); tc.target = focus; tc.track_axis = 'TRACK_NEGATIVE_Z'; tc.up_axis = 'UP_Y'
cam.data.lens = 85; cam.data.sensor_width = 36
cam.data.dof.use_dof = True; cam.data.dof.focus_object = focus; cam.data.dof.aperture_fstop = 16

# ---- clean shading on everything -------------------------------------------------------
for o in list(bpy.data.objects):
    if o.type != 'MESH':
        continue
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active = o
    for m in list(o.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.ops.object.shade_smooth_by_angle(angle=math.radians(30))

# ---- render settings ------------------------------------------------------------------
scene.render.engine = 'CYCLES'; cy = scene.cycles
prefs = bpy.context.preferences.addons["cycles"].preferences; device = "CPU"
for kind in ("OPTIX", "CUDA"):
    try:
        prefs.compute_device_type = kind; prefs.get_devices()
        if any(d.type == kind for d in prefs.devices):
            for d in prefs.devices: d.use = (d.type == kind)
            cy.device = 'GPU'; device = kind; break
    except Exception:
        pass
print(f"[render] device={device} samples={SAMPLES} views={VIEWS}")
cy.samples = SAMPLES; cy.use_adaptive_sampling = True; cy.adaptive_threshold = 0.006
cy.use_denoising = True; cy.denoiser = 'OPTIX' if device == "OPTIX" else 'OPENIMAGEDENOISE'
cy.max_bounces = 16; cy.glossy_bounces = 8; cy.caustics_reflective = False; cy.caustics_refractive = False
scene.render.resolution_x, scene.render.resolution_y = 2400, 1600
vs = scene.view_settings
try:
    vs.view_transform = 'AgX'; vs.look = 'AgX - Medium High Contrast'
except TypeError:
    vs.view_transform = 'Filmic'
scene.render.image_settings.file_format = 'PNG'; scene.render.image_settings.color_depth = '16'

bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "rs232_4port.blend"))
for view in VIEWS:
    # "ports": whole rig turned 180 deg -> camera looks at the RJ45 face; the device
    # auto-rotates its UI (orientation sensor), so the screen texture turns with it.
    rig.rotation_euler.z = math.radians(180) if view == "ports" else 0.0
    disp_tex.image = IMG[view]
    scene.render.filepath = os.path.join(HERE, "render", f"rs232_4port_{view}.png")
    bpy.ops.render.render(write_still=True)
    print(f"[render] wrote {scene.render.filepath}")
