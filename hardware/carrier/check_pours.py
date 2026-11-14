#!/usr/bin/env python3
"""Independent check that every copper pour hangs together.

    python3 check_pours.py        # after route.py; needs the pcbnew module of KiCad 7

route.py leaves a ground pad that gets no track of its own to the pour.  Whether the pour really
reaches it depends on KiCad's fill, not on the router's model of it.  This script fills the zones,
takes the islands KiCad produced and groups them by real copper contact (vias, pads, tracks of the
same net).  A net is good when all its islands and pads end up in one group.  It says where a
separate island lies, which the DRC report does not.
"""
import math
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "kicad", "rs232_carrier.kicad_pcb")


def check(board, net):
    isl = []                                  # (layer, outline, index in its zone)
    for z in board.Zones():
        if z.GetNetname() != net:
            continue
        for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
            if z.IsOnLayer(layer):
                ps = z.GetFilledPolysList(layer)
                isl += [(layer, ps.Outline(k), k) for k in range(ps.OutlineCount())]
    n = len(isl)

    def touching(layer, x, y, acc):
        pt = pcbnew.VECTOR2I(int(x), int(y))
        return [k for k, (l, o, _) in enumerate(isl) if (layer is None or l == layer) and o.PointInside(pt, int(acc))]

    items = []                                # (kind, name, where, islands it touches)
    for t in board.GetTracks():
        if t.GetNetname() != net:
            continue
        if t.GetClass() == "PCB_VIA":
            p = t.GetPosition()
            items.append(("via", "via", [(p.x, p.y, None, 0.36e6)], touching(None, p.x, p.y, 0.36e6)))
        else:
            s, e = t.GetStart(), t.GetEnd()
            m = max(1, int(math.hypot(e.x - s.x, e.y - s.y) / 0.2e6))
            ks = set()
            for i in range(m + 1):
                ks.update(touching(t.GetLayer(), s.x + (e.x - s.x) * i / m, s.y + (e.y - s.y) * i / m, t.GetWidth() / 2))
            items.append(("trk", "track", [(s.x, s.y, t.GetLayer(), 0.15e6), (e.x, e.y, t.GetLayer(), 0.15e6)], sorted(ks)))
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetname() != net:
                continue
            c, sz = p.GetPosition(), p.GetSize()
            tht = p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH
            acc = max(sz.x, sz.y) / 2 + 0.3e6 if tht else min(sz.x, sz.y) / 2
            ks = []
            for l in ([None] if tht else [l for l in (pcbnew.F_Cu, pcbnew.B_Cu) if p.IsOnLayer(l)]):
                ks += touching(l, c.x, c.y, acc)
            items.append(("pad", f"{fp.GetReference()}.{p.GetNumber()}", [(c.x, c.y, None, 0.9e6)], ks))

    parent = list(range(n + len(items)))

    def find(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    for q, it in enumerate(items):
        for k in it[3]:
            parent[find(k)] = find(n + q)
    ends = [(q, p) for q, it in enumerate(items) for p in it[2]]
    for q, it in enumerate(items):
        if it[0] != "trk":
            continue
        for ex, ey, el, _ in it[2]:
            for q2, (px, py, pl, r) in ends:
                if q2 != q and (pl is None or pl == el) and math.hypot(px - ex, py - ey) <= r:
                    parent[find(n + q)] = find(n + q2)
    if not n:
        return True
    main = find(max(range(n), key=lambda k: isl[k][1].Area()))
    loose_pads = [it[1] for q, it in enumerate(items) if it[0] == "pad" and find(n + q) != main]
    loose_isl = [k for k in range(n) if find(k) != main]
    print(f"{net:14s} {n:3d} islands, {'one group' if not loose_isl and not loose_pads else 'NOT CONNECTED'}")
    for k in loose_isl:
        l, o, idx = isl[k]
        bb = o.BBox()
        print(f"    separate island on {board.GetLayerName(l)}: {o.Area() / 1e12:.1f} mm2, x {bb.GetLeft() / 1e6:.1f}..{bb.GetRight() / 1e6:.1f}, "
              f"y {bb.GetTop() / 1e6:.1f}..{bb.GetBottom() / 1e6:.1f}")
    if loose_pads:
        print("    pads off the main group:", ", ".join(loose_pads))
    return not loose_isl and not loose_pads


def main():
    board = pcbnew.LoadBoard(PCB)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    nets = sorted({z.GetNetname() for z in board.Zones() if z.GetNetname()})
    ok = all([check(board, net) for net in nets])
    print("pours:", "all connected" if ok else "FAILED - try another ROUTE_SEED (see route.py)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
