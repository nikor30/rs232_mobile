#!/usr/bin/env python3
"""Printability check of the print-ready files (out/print, out_hat/print), needs numpy only.

    python3 printcheck.py            # four-port case
    python3 printcheck.py --hat      # two-port case

Every part is taken as it lies on the bed and cut into vertical columns on a 0.2 mm grid.  A column in which
material starts above the bed on a face flatter than a printer lays freely (more than 50 degrees from the
vertical) is an overhang.  Each overhang is then judged by what holds it in the layer below:

  bridge      material on both sides in x or in y, at most BRIDGE_MAX apart
  ledge       material on one side only, at most LEDGE_MAX away
  on a rib    one of the built-in break-away ribs ends one layer below it

Anything else would need support from the slicer and fails the check.  The faces themselves decide the angle,
the columns decide the support, so a face that merely looks like a bridge but hangs free on one side is caught.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out_hat" if "--hat" in sys.argv else "out", "print")
G = 0.2                 # mm, grid pitch
LIMIT_DEG = 50.0        # overhang measured from the vertical
BRIDGE_MAX = 18.0       # mm, a span PLA bridges cleanly with the part fan on
LEDGE_MAX = 1.0         # mm, about two lines of a 0.4 mm nozzle
RIB_GAP = 0.25          # mm, a rib ending this close below a face carries it (one layer of 0.2 mm)


def load(path):
    data = open(path, "rb").read()
    n = struct.unpack("<I", data[80:84])[0]
    a = np.frombuffer(data, dtype=np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")]), count=n, offset=84)
    return a["v"].astype(float)


def columns(v):
    """Vertical rays through the mesh: per grid cell the heights where a ray crosses the surface."""
    lo, hi = v.reshape(-1, 3).min(axis=0), v.reshape(-1, 3).max(axis=0)
    ox, oy = lo[0] - 2 * G + 0.0137, lo[1] - 2 * G + 0.0071        # off every model edge
    nx, ny = int((hi[0] - ox) / G) + 3, int((hi[1] - oy) / G) + 3
    cs, zs, ns = [], [], []
    for a, b, c in v:
        n = np.cross(b - a, c - a)
        ln = np.linalg.norm(n)
        if ln < 1e-12 or abs(n[2]) / ln < 1e-6:
            continue
        i0, i1 = int(np.ceil((min(a[0], b[0], c[0]) - ox) / G)), int(np.floor((max(a[0], b[0], c[0]) - ox) / G))
        j0, j1 = int(np.ceil((min(a[1], b[1], c[1]) - oy) / G)), int(np.floor((max(a[1], b[1], c[1]) - oy) / G))
        if i1 < i0 or j1 < j0:
            continue
        ii, jj = np.meshgrid(np.arange(i0, i1 + 1), np.arange(j0, j1 + 1), indexing="ij")
        x, y = ox + ii * G, oy + jj * G
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        w0 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d
        w1 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d
        w2 = 1 - w0 - w1
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            continue
        cs.append((ii[m] * ny + jj[m]).ravel())
        zs.append((w0[m] * a[2] + w1[m] * b[2] + w2[m] * c[2]).ravel())
        ns.append(np.full(int(m.sum()), n[2] / ln))
    col, z, nz = np.concatenate(cs), np.concatenate(zs), np.concatenate(ns)
    order = np.lexsort((z, col))
    col, z, nz = col[order], z[order], nz[order]
    first = np.r_[True, col[1:] != col[:-1]]
    start = np.maximum.accumulate(np.where(first, np.arange(len(col)), 0))
    rank = np.arange(len(col)) - start
    count = np.bincount(col, minlength=nx * ny)
    odd = (count % 2 == 1)                                        # a ray that grazed an edge: left out
    return dict(nx=nx, ny=ny, ox=ox, oy=oy, col=col, z=z, nz=nz, rank=rank, odd=odd, zmin=lo[2])


def solid_at(c, q):
    """Cells with material at height q."""
    below = np.bincount(c["col"][c["z"] < q], minlength=c["nx"] * c["ny"])
    return ((below % 2 == 1) & ~c["odd"]).reshape(c["nx"], c["ny"])


def reach(ovh, sup, axis, rev):
    """Distance from each overhang cell back along one axis to the material that holds it, inf if there is none."""
    o, s = np.moveaxis(ovh, axis, 0), np.moveaxis(sup, axis, 0)
    if rev:
        o, s = o[::-1], s[::-1]
    d = np.full(o.shape, np.inf)
    cur = np.full(o.shape[1], np.inf)
    for i in range(o.shape[0]):
        cur = np.where(s[i], 0.0, np.where(o[i], cur + G, np.inf))
        d[i] = cur
    if rev:
        d = d[::-1]
    return np.moveaxis(d, 0, axis)


def regions(mask):
    """Connected groups of cells (edges and corners), as lists of index arrays."""
    seen = np.zeros(mask.shape, bool)
    out = []
    for i, j in zip(*np.nonzero(mask)):
        if seen[i, j]:
            continue
        stack, cells = [(i, j)], []
        seen[i, j] = True
        while stack:
            a, b = stack.pop()
            cells.append((a, b))
            for da in (-1, 0, 1):
                for db in (-1, 0, 1):
                    p, q = a + da, b + db
                    if 0 <= p < mask.shape[0] and 0 <= q < mask.shape[1] and mask[p, q] and not seen[p, q]:
                        seen[p, q] = True
                        stack.append((p, q))
        out.append(np.array(cells))
    return out


def check(path):
    v = load(path)
    lo, hi = v.reshape(-1, 3).min(axis=0), v.reshape(-1, 3).max(axis=0)
    c = columns(v)
    nx, ny, zmin = c["nx"], c["ny"], c["zmin"]
    enter = (c["rank"] % 2 == 0) & ~c["odd"][c["col"]]
    prev_z = np.r_[-np.inf, c["z"][:-1]]
    gap = np.where(c["rank"] > 0, c["z"] - prev_z, np.inf)        # air between this face and the material below it
    flat = c["nz"] < -np.sin(np.radians(LIMIT_DEG))
    start = enter & flat & (c["z"] > zmin + 0.05)
    on_rib = start & (gap <= RIB_GAP)
    free = start & ~on_rib
    print(f"{os.path.basename(path)}: {hi[0] - lo[0]:.1f} x {hi[1] - lo[1]:.1f} x {hi[2] - lo[2]:.1f} mm on the bed, {len(v)} triangles, "
          f"{free.sum() * G * G:.0f} mm2 start above the bed, {on_rib.sum() * G * G:.0f} mm2 of that over ribs"
          + (f", {int(c['odd'].sum())} rays left out" if c["odd"].any() else ""))
    rows, bad = [], 0
    level = np.round((c["z"] - zmin) / 0.1).astype(int)
    for lv in np.unique(level[start]):
        q = zmin + lv * 0.1
        ovh = np.zeros(nx * ny, bool)
        ovh[c["col"][free & (level == lv)]] = True
        rib = np.zeros(nx * ny, bool)
        rib[c["col"][on_rib & (level == lv)]] = True
        ovh, rib = ovh.reshape(nx, ny), rib.reshape(nx, ny)
        if not ovh.any():
            continue
        # a rounded ceiling starts a little lower from cell to cell: what began up to 0.3 mm below is already
        # printed and carries on the span, so the walk to the support goes through it
        near = np.zeros(nx * ny, bool)
        near[c["col"][free & (level < lv) & (level >= lv - 3)]] = True
        sup = solid_at(c, q - 0.1) | rib
        walk = (ovh | near.reshape(nx, ny)) & ~sup
        d = [reach(walk, sup, ax, rv) for ax in (0, 1) for rv in (False, True)]
        span = np.minimum(d[0] + d[1], d[2] + d[3]) - G            # between the two supports
        ledge = np.minimum.reduce(d) - G / 2                       # from the nearest support
        for cells in regions(ovh):
            i, j = cells[:, 0], cells[:, 1]
            area = len(cells) * G * G
            sp, lg = span[i, j], ledge[i, j]
            as_ledge = lg <= LEDGE_MAX
            as_bridge = ~as_ledge & (sp <= BRIDGE_MAX)
            fail = ~as_ledge & ~as_bridge
            over_rib = bool(rib[max(i.min() - 1, 0):i.max() + 2, max(j.min() - 1, 0):j.max() + 2].any())
            parts = []
            if as_bridge.any():
                parts.append(f"bridge, longest span {sp[as_bridge].max():.1f} mm")
            if as_ledge.any():
                parts.append(f"ledge, {max(lg[as_ledge].max(), 0):.1f} mm from its support")
            if over_rib:
                parts.append("between break-away ribs")
            if fail.any():
                parts.append(f"NEEDS SUPPORT ({fail.sum() * G * G:.1f} mm2 hang free)")
                bad += 1
            rows.append((area, (i.max() - i.min() + 1) * G, (j.max() - j.min() + 1) * G, q - zmin, "; ".join(parts), bool(fail.any())))
    small = [r for r in rows if r[0] < 0.5 and not r[5]]
    for area, dx, dy, h, text, _ in sorted((r for r in rows if r not in small), key=lambda r: -r[0]):
        print(f"    {area:7.1f} mm2  {dx:5.1f} x {dy:5.1f} mm, {h:5.1f} mm above the bed: {text}")
    if small:
        print(f"    and {len(small)} patch(es) under 0.5 mm2, all held")
    return bad


def main():
    bad = 0
    for name in sorted(os.listdir(OUT)):
        if name.endswith(".stl"):
            bad += check(os.path.join(OUT, name))
    print("result:", "every part prints without support from the slicer" if not bad else f"{bad} patch(es) need support")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
