#!/usr/bin/env python3
"""Generate a real KiCad 7 schematic (.kicad_sch) from parts.py's components+nets.

Approach: every pin gets a short stub wire + a net label (validated against a
render test), instead of point-to-point routed wires. Same-named labels are
electrically joined by KiCad regardless of screen position, so this is robust
to generate without doing schematic auto-routing / crossing-avoidance.
"""
import math, re, uuid, sys
import sexpdata as sx
from sexpdata import Symbol as S
import kicad_symlib as kicad_inspect  # local inspect.py helper module
from parts import components, nets, SYM

def uid():
    return str(uuid.uuid4())

# ---------------------------------------------------------------- library ---
_lib_cache = {}
def load_lib(path):
    if path not in _lib_cache:
        with open(path) as f:
            _lib_cache[path] = sx.loads(f.read())
    return _lib_cache[path]

def raw_symbol_text(path, name):
    """Return the exact S-expression text for symbol `name` inside `path`."""
    with open(path) as f:
        text = f.read()
    # locate '(symbol "name"' at any indentation, then match balanced parens
    needle = f'(symbol "{name}"'
    i = text.find(needle)
    if i < 0:
        raise KeyError(f"{name} not found in {path}")
    depth = 0
    j = i
    while True:
        if text[j] == '(':
            depth += 1
        elif text[j] == ')':
            depth -= 1
            if depth == 0:
                j += 1
                break
        j += 1
    return text[i:j]

def get_pins(sym_key):
    libpath, name, base, footprint = SYM[sym_key]
    lib = load_lib(libpath)
    node = kicad_inspect.find(lib, name)
    extends = next((x for x in node[1:] if isinstance(x, list) and x[0] == S('extends')), None)
    pin_source_node = node
    if extends:
        pin_source_node = kicad_inspect.find(lib, extends[1])
    subunits = [n for n in pin_source_node[1:] if isinstance(n, list) and n and n[0] == S('symbol')]
    pins = []
    for u in subunits:
        pins.extend(kicad_inspect.pins_of_unit(u))
    return pins

def flatten_extends(libpath, base_name, derived_name):
    """Merge a base symbol + an `(extends ...)` derived symbol into ONE
    standalone symbol (no `extends`) named `derived_name`.

    Empirically required: kicad-cli 7.0.11's schematic loader fails to load
    (exit 3, "Failed to load schematic file") a `.kicad_sch` whose
    `lib_symbols` contains a raw `(extends ...)` symbol copied verbatim from
    a library file -- even with its base symbol also present alongside it.
    Real KiCad-authored schematics apparently never cache `extends` this way
    (no demo file under /usr/share/kicad/demos uses it); flattening into one
    fully self-contained symbol is what actually loads (verified via a
    minimal repro in svg_bisect/flat_test.kicad_sch -> exit 0).
    """
    base_text = raw_symbol_text(libpath, base_name)
    derived_text = raw_symbol_text(libpath, derived_name)
    header_m = re.match(r'\(symbol "' + re.escape(base_name) + r'"(.*?)\n', base_text)
    header_attrs = header_m.group(1)
    derived_m = re.search(
        r'\(extends "' + re.escape(base_name) + r'"\)\n(.*)\n  \)\s*$', derived_text, re.S)
    derived_props = derived_m.group(1)
    subunits_m = re.search(
        r'(\(symbol "' + re.escape(base_name) + r'_.*)\n  \)\s*$', base_text, re.S)
    base_subunits = subunits_m.group(1).replace(f'"{base_name}_', f'"{derived_name}_')
    return f'(symbol "{derived_name}"{header_attrs}\n{derived_props}\n{base_subunits}\n  )'

def lib_id(sym_key):
    libpath, name, base, footprint = SYM[sym_key]
    libname = re.sub(r'\.kicad_sym$', '', libpath.rsplit('/', 1)[-1])
    return f'{libname}:{name}'

def prefix_top_symbol_name(text, bare_name, full_id):
    """Rename ONLY the top-level `(symbol "bare_name" ...)` declaration to
    `(symbol "full_id" ...)` (full_id = "Library:SymbolName"), leaving any
    nested subunit symbol names (e.g. "bare_name_1_1") untouched.

    Empirically required: kicad-cli 7.0.11 loads a schematic fine either
    way (extends-flattening fixed the hard load failure), but silently
    draws NOTHING for any placed symbol unless the lib_symbols cache entry's
    own name is the full "Library:Symbol" id that instances' `lib_id`
    reference -- confirmed by a minimal repro (Device:R placed with a
    lib_symbols entry named bare "R" renders blank; renaming that cache
    entry to "Device:R" makes it render). text always starts with exactly
    `(symbol "bare_name"` so a prefix slice is safe and unambiguous.
    """
    needle = f'(symbol "{bare_name}"'
    assert text.startswith(needle), (text[:60], needle)
    return f'(symbol "{full_id}"' + text[len(needle):]

def get_lib_symbol_texts(sym_key):
    """Symbol text(s) needed in lib_symbols for this component class.

    Returns a single-element list: the symbol's raw text (or a flattened
    merge when the library defines it via `extends`, see flatten_extends),
    with its top-level name rewritten to the full "Library:Symbol" id (see
    prefix_top_symbol_name).
    """
    libpath, name, base, footprint = SYM[sym_key]
    text = flatten_extends(libpath, base, name) if base else raw_symbol_text(libpath, name)
    return [prefix_top_symbol_name(text, name, lib_id(sym_key))]

# ----------------------------------------------------------- placement -----
# rows of refs, laid out left-to-right; y advances per row. Widths are chosen
# generously per component "class" so nothing overlaps even with many pins.
# Grouping derived from parts.nets (functional clusters), refs cross-checked
# 1:1 against parts.components (see self-test below).
ROWS = [
    ['J2', 'U10', 'C1', 'C2', 'C3', 'J1'],                                       # battery tap + local LDO + connector to base board
    ['X1', 'C4', 'R1', 'R2', 'R3', 'R4', 'C5'],                                  # shared osc + I2C/IRQ/RESET pull-ups
    ['U6', 'C6', 'U2', 'C7', 'C8', 'C9', 'C10', 'R5', 'R6', 'R7', 'R8', 'J10', 'J11'],   # Ports 1+2: SC16IS752 + MAX3232 (both ch.) + 2x RJ45
    ['U7', 'C11', 'U3', 'C12', 'C13', 'C14', 'C15', 'R9', 'R10', 'R11', 'R12', 'J12', 'J13'],  # Ports 3+4
]

# Safety check: ROWS must be a 1:1 partition of parts.components' keys.
_rows_flat = [r for row in ROWS for r in row]
assert len(_rows_flat) == len(set(_rows_flat)), "duplicate ref in ROWS"
assert set(_rows_flat) == set(components.keys()), \
    f"ROWS/components mismatch: missing={set(components)-set(_rows_flat)} extra={set(_rows_flat)-set(components)}"
CLASS_W = {'SC16IS752': 65, 'AMS1117': 35, 'OSC': 35, 'MAX3232': 55,
           'RJ45': 40, 'CONN2': 30, 'CONN4JST': 30,
           'R': 22, 'C': 22}
ROW_H = 90  # MAX3232's GND(pin15)/VCC(pin16) stubs point straight down/up from the
            # symbol's own centerline, reaching +-(30.48+5.08)=35.56mm -- the tallest
            # reach of any repeated-per-row part. 70mm still let consecutive port rows'
            # MAX3232 stubs overlap by ~1mm (U2 GND into U3 VCC), silently shorting
            # +3V3 to GND in the exported netlist despite a visually clean render.
            # 90mm > 2*35.56 clears it with margin; check_no_shorted_stubs() below
            # verifies this for every part, not just MAX3232, on every regeneration.
            # SC16IS752IPW is TSSOP-28 (taller than the 750's TSSOP-24, 24 vs 28 pins,
            # +-25.4mm pin reach per the KiCad symbol) -- still comfortably under 90mm.

def build_layout():
    pos = {}
    y = 20.0
    for row in ROWS:
        x = 20.0
        maxh = 0
        for ref in row:
            key = components[ref][0]
            w = CLASS_W.get(key, 30)
            pos[ref] = (x + w / 2, y + 20)
            x += w + 12
        y += ROW_H
    return pos, y

POS, SHEET_H = build_layout()
SHEET_W = max(20 + sum(CLASS_W.get(components[r][0], 30) + 12 for r in row) for row in ROWS) + 20

# --------------------------------------------------------------- geometry ---
def outward_vec(angle_lib):
    a = math.radians((angle_lib + 180) % 360)
    dx = round(math.cos(a))
    dy = -round(math.sin(a))
    return dx, dy

STUB = 5.08

def pin_file_pos(px, py, p):
    return px + p['x'], py - p['y']

# ------------------------------------------------------------- assembly ----
lib_symbols_text = {}   # lib_id -> text (dedup by symbol key, not per-instance)
placed_sym_keys = set()
body = []               # sch body s-expr lines (raw text)
sheet_uuid = uid()

def esc(s):
    return str(s).replace('\\', '\\\\').replace('"', '\\"')

for ref, (key, value) in components.items():
    if key not in placed_sym_keys:
        placed_sym_keys.add(key)
        for t in get_lib_symbol_texts(key):
            # index by the symbol's own quoted name to dedupe (e.g. MAX232 appears once)
            m = re.match(r'\(symbol "([^"]+)"', t)
            lib_symbols_text[m.group(1)] = t

# reverse lookup: (ref, pin_number) -> net_name, built once from parts.nets
pin_net = {}
for net_name, pin_list in nets.items():
    for (ref, pin_number) in pin_list:
        key_ = (ref, pin_number)
        if key_ in pin_net and pin_net[key_] != net_name:
            raise ValueError(f"pin {key_} assigned to two nets: {pin_net[key_]!r} and {net_name!r}")
        pin_net[key_] = net_name

instances = []
wires = []
labels = []

for ref, (key, value) in components.items():
    px, py = POS[ref]
    lid = lib_id(key)
    fp = SYM[key][3]
    iid = uid()
    instances.append(
        f'  (symbol (lib_id "{lid}") (at {px:.2f} {py:.2f} 0) (unit 1)\n'
        f'    (uuid {iid})\n'
        f'    (property "Reference" "{ref}" (at {px:.2f} {py-8:.2f} 0) (effects (font (size 1.27 1.27))))\n'
        f'    (property "Value" "{esc(value)}" (at {px:.2f} {py+8:.2f} 0) (effects (font (size 1.27 1.27))))\n'
        f'    (property "Footprint" "{fp}" (at {px:.2f} {py:.2f} 0) (effects (font (size 1.27 1.27)) hide))\n'
        f'  )'
    )
    for p in get_pins(key):
        fx, fy = pin_file_pos(px, py, p)
        dx, dy = outward_vec(p['angle'])
        lx, ly = fx + dx * STUB, fy + dy * STUB
        wires.append(f'  (wire (pts (xy {fx:.2f} {fy:.2f}) (xy {lx:.2f} {ly:.2f})) '
                     f'(stroke (width 0) (type default)) (uuid {uid()}))')
        # net name for this (ref, pin_number)
        net = pin_net.get((ref, p['number']))
        if net is None:
            continue  # genuinely unused pin -> left as a stub with no label (visually open)
        ang = 0 if dy == 0 else 90
        justify = 'right' if (dx < 0) else ('left' if dy == 0 else 'left')
        labels.append(f'  (label "{esc(net)}" (at {lx:.2f} {ly:.2f} {ang}) '
                       f'(effects (font (size 1.0 1.0)) (justify {justify})))')

# ---------------------------------------------------- safety: no shorted stubs ---
# Bug class found empirically: two DIFFERENT components' stub wires (different nets)
# can overlap/touch along their length -- not just at a shared endpoint -- if row/
# column spacing is too tight for a tall symbol's pin extents. KiCad treats any such
# touch as an electrical junction, silently MERGING the two nets (e.g. GND shorted to
# +3V3). This does NOT show up in a quick visual render (labels still print correctly
# at their own spot) -- it only surfaces in the exported netlist. So: check every pair
# of stub-wire segments belonging to different (ref) and different (net) for any
# overlap (shared sub-segment or one's endpoint landing on the other's interior),
# and refuse to write a schematic that has one.
def _on_segment(px, py, ax, ay, bx, by, eps=1e-6):
    cross = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
    if abs(cross) > eps:
        return False
    return (min(ax, bx) - eps <= px <= max(ax, bx) + eps and
            min(ay, by) - eps <= py <= max(ay, by) + eps)

def _segs_overlap(s1, s2):
    ax, ay, bx, by = s1[:4]; cx, cy, dx, dy = s2[:4]
    return (any(_on_segment(px, py, ax, ay, bx, by) for px, py in [(cx, cy), (dx, dy)]) or
            any(_on_segment(px, py, cx, cy, dx, dy) for px, py in [(ax, ay), (bx, by)]))

def check_no_shorted_stubs():
    idx = 0
    owner = {}
    for ref, (key, value) in components.items():
        for p in get_pins(key):
            owner[idx] = (ref, p['number']); idx += 1
    segs = []
    for i, w in enumerate(wires):
        m = re.search(r'\(xy ([\-\d\.]+) ([\-\d\.]+)\) \(xy ([\-\d\.]+) ([\-\d\.]+)\)', w)
        fx, fy, lx, ly = (float(v) for v in m.groups())
        ref, pin = owner[i]
        segs.append((fx, fy, lx, ly, ref, pin, pin_net.get((ref, pin))))
    bad = []
    for i in range(len(segs)):
        for j in range(i + 1, len(segs)):
            s1, s2 = segs[i], segs[j]
            if s1[4] == s2[4] or s1[6] == s2[6]:
                continue  # same component, or legitimately the same net
            if _segs_overlap(s1, s2):
                bad.append((s1, s2))
    assert not bad, (
        f"{len(bad)} stub-wire pair(s) from DIFFERENT nets overlap/touch (would short "
        f"in KiCad's netlist) -- widen ROW_H/CLASS_W. First: {bad[0][0][4]}.{bad[0][0][5]} "
        f"(net {bad[0][0][6]}) vs {bad[0][1][4]}.{bad[0][1][5]} (net {bad[0][1][6]})")

check_no_shorted_stubs()

sch = []
sch.append('(kicad_sch (version 20230121) (generator rs232_gen)')
sch.append(f'  (uuid {sheet_uuid})')
sch.append(f'  (paper "A1")')
sch.append('  (title_block')
sch.append('    (title "RS232 WLAN-Konsole - Kleinserie PCB (Entwurf v0.1)")')
sch.append('    (date "2026-09-21")')
sch.append('    (rev "0.1")')
sch.append('    (company "DIY / Niko")')
sch.append('  )')
sch.append('  (lib_symbols')
for txt in lib_symbols_text.values():
    sch.append('    ' + txt)
sch.append('  )')
sch.extend(wires)
sch.extend(labels)
sch.extend(instances)
sch.append(')')

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'rs232_console.kicad_sch'
    with open(out_path, 'w') as f:
        f.write('\n'.join(sch) + '\n')
    n_pins_total = sum(len(get_pins(components[r][0])) for r in components)
    n_labeled = len(labels)
    print(f"wrote {out_path}")
    print(f"components={len(components)} pins_total={n_pins_total} "
          f"stub_wires={len(wires)} labels={n_labeled} "
          f"unlabeled_stubs={len(wires)-n_labeled}")
    print(f"sheet size approx {SHEET_W:.0f} x {SHEET_H:.0f} mm")
