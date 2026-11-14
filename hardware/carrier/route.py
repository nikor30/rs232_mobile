#!/usr/bin/env python3
"""Grid maze router for the carrier board (2 layers, vias).

    python3 route.py            # routes kicad/rs232_carrier.kicad_pcb in place

The router knows the design model from generate.py (pads, nets, keep-outs) and
appends tracks and vias to the generated board file.  Clearances are net-class
aware: nets of one isolated port keep 1.5 mm to the other ports and 3 mm to the
ESP side, so the isolation is respected by construction and then verified by
KiCad's DRC (drc.py).
"""
import heapq
import math
import os
import sys
import time
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import generate as G                     # noqa: E402
from kigen import U, f                   # noqa: E402

# Where the router has to choose between equals (which nets to rip up first) it follows this number, so a
# run can be repeated exactly.  A plane pad that gets no track of its own is left to the pour; whether the
# pour really reaches it only shows in KiCad's fill (drc.py).  If drc.py reports unconnected items, try
# another seed:  ROUTE_SEED=3 python3 route.py
SEED = int(os.environ.get("ROUTE_SEED", "1"))


def tie(name):
    return zlib.crc32(f"{SEED}:{name}".encode())


GRID = 0.2
CLR = 0.2                 # default copper clearance
HW_SIG, HW_PWR = 0.125, 0.2   # track half widths
VIA_D, VIA_DRILL = 0.7, 0.3
EDGE = 0.6                # keep tracks this far from the board edge
VIA_COST = 2.5            # mm equivalent
SLACK = 0.06              # grid sampling margin added to every clearance
NX = int(round(G.BOARD_W / GRID)) + 1
NY = int(round(G.BOARD_H / GRID)) + 1
F, B = 0, 1
VIA_KERNEL = np.array([[0, 1, 1, 1, 0], [1, 1, 1, 1, 1], [1, 1, 1, 1, 1], [1, 1, 1, 1, 1], [0, 1, 1, 1, 0]], bool)


def group(net: str) -> int:
    """0 = ESP side, 1..4 = isolated island of that port."""
    if net.startswith("/CH") and net.split("/")[2] in G.CH_ISO_LOCALS:
        return int(net[3])
    return 0


def locked(net: str) -> bool:
    """Short channel-local nets (optocoupler, relay and transceiver pins) are routed first and then stay:
    a long net must go around them instead of tearing them up."""
    return net.startswith("/CH") and net.split("/")[2] != "ISOGND"


def clearance(ga: int, gb: int) -> float:
    if ga == gb:
        return CLR
    if ga == 0 or gb == 0:
        return 3.0
    return 1.5


def half_width(net: str) -> float:
    # ISO_IN (one port, <= 0.25 A) runs at signal width: 0.25 mm drops about 40 mV over the board
    # ground links are thin as well: the pours on both layers carry the current, the tracks only tie them together
    # the island supply VISO carries about 20 mA: signal width
    if net in ("+5V", "VBAT", "BATP", "SW"):
        return HW_PWR
    return HW_SIG


# ---------------------------------------------------------------------------- rasterisation
XS = np.arange(NX) * GRID
YS = np.arange(NY) * GRID


def _box(cx, cy, r):
    i0 = max(0, int((cx - r) / GRID) - 1)
    i1 = min(NX - 1, int((cx + r) / GRID) + 2)
    j0 = max(0, int((cy - r) / GRID) - 1)
    j1 = min(NY - 1, int((cy + r) / GRID) + 2)
    return i0, i1, j0, j1


def disc_mask(cx, cy, r):
    i0, i1, j0, j1 = _box(cx, cy, r)
    gx, gy = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1], indexing="ij")
    return (slice(i0, i1 + 1), slice(j0, j1 + 1)), ((gx - cx) ** 2 + (gy - cy) ** 2) <= r * r


def rect_mask(cx, cy, w, h, rot, r):
    """Cells within distance r of a rotated rectangle (w x h, rotation in degrees, KiCad sense)."""
    rad = max(w, h) / 2 + r + GRID
    i0, i1, j0, j1 = _box(cx, cy, rad)
    gx, gy = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1], indexing="ij")
    t = math.radians(rot)
    c, s = math.cos(t), math.sin(t)
    dx, dy = gx - cx, gy - cy
    # into local coordinates (inverse of world = (px*c + py*s, -px*s + py*c))
    u = dx * c - dy * s
    v = dx * s + dy * c
    if r < 0:
        return (slice(i0, i1 + 1), slice(j0, j1 + 1)), (np.abs(u) <= w / 2 + r) & (np.abs(v) <= h / 2 + r)
    du = np.maximum(np.abs(u) - w / 2, 0)
    dv = np.maximum(np.abs(v) - h / 2, 0)
    return (slice(i0, i1 + 1), slice(j0, j1 + 1)), (du * du + dv * dv) <= r * r


def seg_mask(x1, y1, x2, y2, r):
    xmin, xmax = min(x1, x2), max(x1, x2)
    ymin, ymax = min(y1, y2), max(y1, y2)
    i0 = max(0, int((xmin - r) / GRID) - 1)
    i1 = min(NX - 1, int((xmax + r) / GRID) + 2)
    j0 = max(0, int((ymin - r) / GRID) - 1)
    j1 = min(NY - 1, int((ymax + r) / GRID) + 2)
    gx, gy = np.meshgrid(XS[i0:i1 + 1], YS[j0:j1 + 1], indexing="ij")
    vx, vy = x2 - x1, y2 - y1
    L2 = vx * vx + vy * vy
    if L2 < 1e-9:
        d2 = (gx - x1) ** 2 + (gy - y1) ** 2
    else:
        t = np.clip(((gx - x1) * vx + (gy - y1) * vy) / L2, 0, 1)
        d2 = (gx - (x1 + t * vx)) ** 2 + (gy - (y1 + t * vy)) ** 2
    return (slice(i0, i1 + 1), slice(j0, j1 + 1)), d2 <= r * r


# ---------------------------------------------------------------------------- model
class Item:
    """A copper item: pad (rect/disc), track segment or via."""

    def __init__(self, kind, net, layers, geom, hw=0.0, routed=False):
        self.kind, self.net, self.layers, self.geom, self.hw, self.routed = kind, net, layers, geom, hw, routed

    def masks(self, extra):
        """Yield (layer, slices, mask) for the item inflated by `extra`."""
        if self.kind == "rect":
            cx, cy, w, h, rot = self.geom
            sl, m = rect_mask(cx, cy, w, h, rot, extra)
        elif self.kind == "disc":
            cx, cy, r = self.geom
            sl, m = disc_mask(cx, cy, r + extra)
        else:
            x1, y1, x2, y2 = self.geom
            sl, m = seg_mask(x1, y1, x2, y2, self.hw + extra)
        for layer in self.layers:
            yield layer, sl, m


class Router:
    def __init__(self, placed):
        self.counts = {(g, c): np.zeros((2, NX, NY), np.int16) for g in range(5) for c in (0, 1)}
        self.static = np.zeros((2, NX, NY), bool)
        self.novia = np.zeros((NX, NY), bool)      # vias never go into pads or near holes
        self.pads = {}        # net -> list of (item, layer, cells(list of (i,j)), centre)
        self.comps = {}       # plane net -> list of connected components (cell sets)
        self.own_items = {}   # net -> list of Items (pads + tracks)
        self.tracks = []      # (net, layer, x1,y1,x2,y2, hw)
        self.vias = []        # (net, x, y)
        self.max_hw = HW_PWR
        self.build_static(placed)

    # ----- static obstacles
    def build_static(self, placed):
        W, H = G.BOARD_W, G.BOARD_H
        self.static[:, XS < EDGE, :] = True
        self.static[:, XS > W - EDGE, :] = True
        self.static[:, :, YS < EDGE] = True
        self.static[:, :, YS > H - EDGE] = True
        R = G.BOARD_R                # rounded board corners
        gx, gy = np.meshgrid(XS, YS, indexing="ij")
        for cx, cy in ((R, R), (W - R, R), (W - R, H - R), (R, H - R)):
            corner = ((gx < R) if cx == R else (gx > W - R)) & ((gy < R) if cy == R else (gy > H - R))
            outside = corner & (((gx - cx) ** 2 + (gy - cy) ** 2) > (R - EDGE) ** 2)
            self.static[0] |= outside
            self.static[1] |= outside
        b1, b2 = G.BARRIER
        ko = HW_PWR + SLACK          # keep the track copper out of the rule areas
        sl, m = rect_mask(W / 2, (b1 + b2) / 2, W, b2 - b1, 0, ko)
        self.static[0][sl] |= m
        self.static[1][sl] |= m
        for i in range(3):
            gx = G.CH_CENTERS[i] + G.CH_PITCH / 2
            sl, m = rect_mask(gx, (b1 + H) / 2, 1.8, H - b1, 0, ko)
            self.static[0][sl] |= m
            self.static[1][sl] |= m
        for hx, hy, hr in G.hole_keepouts():
            sl, m = disc_mask(hx, hy, hr + ko)
            self.static[0][sl] |= m
            self.static[1][sl] |= m
        # pads
        for p, pin_nets, _ in placed:
            fp = G.FPS[p.fp]
            for pad in fp.pads:
                px, py = G.pad_xy(p, pad)      # bottom-side parts are mirrored
                if pad.kind == "npth":
                    sl, m = disc_mask(px, py, pad.drill / 2 + 0.4 + self.max_hw)
                    self.static[0][sl] |= m
                    self.static[1][sl] |= m
                    continue
                sl, m = disc_mask(px, py, max(pad.w, pad.h) / 2 + VIA_D / 2 + CLR + SLACK)
                self.novia[sl] |= m
                net = pin_nets.get(pad.number, "NC")
                layers = [B if p.side == "B" else F] if pad.kind == "smd" else [F, B]
                if pad.shape == "circle":
                    item = Item("disc", net, layers, (px, py, pad.w / 2))
                else:
                    item = Item("rect", net, layers, (px, py, pad.w, pad.h, p.rot + pad.rot))
                if net == "NC":
                    for layer, sl, m in item.masks(CLR + self.max_hw):
                        self.static[layer][sl] |= m
                    continue
                self.add_item(item)
                # cells inside the pad (targets)
                for layer, sl, m in item.masks(-(HW_SIG + 0.05)):
                    cells = list(zip(*np.nonzero(m)))
                    cells = [(sl[0].start + a, sl[1].start + b) for a, b in cells]
                    ci, cj = int(round(px / GRID)), int(round(py / GRID))
                    if (ci, cj) not in cells:
                        cells.append((ci, cj))
                    self.pads.setdefault(net, []).append((item, layer, cells, (px, py)))

    def add_item(self, item: Item):
        gN = group(item.net)
        self.own_items.setdefault(item.net, []).append(item)
        for g in range(5):
            for c, hw in ((0, HW_SIG), (1, HW_PWR)):
                extra = clearance(g, gN) + hw + SLACK
                for layer, sl, m in item.masks(extra):
                    self.counts[(g, c)][layer][sl] += m

    def remove_item(self, item: Item):
        gN = group(item.net)
        for g in range(5):
            for c, hw in ((0, HW_SIG), (1, HW_PWR)):
                extra = clearance(g, gN) + hw + SLACK
                for layer, sl, m in item.masks(extra):
                    self.counts[(g, c)][layer][sl] -= m

    def snapshot(self):
        import copy
        return ({k: v.copy() for k, v in self.counts.items()}, self.novia.copy(),
                {k: list(v) for k, v in self.own_items.items()}, list(self.tracks), list(self.vias),
                copy.deepcopy(self.comps))

    def restore(self, snap):
        self.counts, self.novia, self.own_items, self.tracks, self.vias, self.comps = snap

    def rip_up(self, net):
        """Remove all routed copper of a net."""
        for item in self.own_items.get(net, []):
            if item.routed:
                self.remove_item(item)
        self.own_items[net] = [it for it in self.own_items.get(net, []) if not it.routed]
        self.tracks = [t for t in self.tracks if t[0] != net]
        self.vias = [v for v in self.vias if v[0] != net]

    def nets_near(self, net, x, y, r=2.5):
        found = set()
        for n, layer, x1, y1, x2, y2, hw in self.tracks:
            if n == net:
                continue
            vx, vy = x2 - x1, y2 - y1
            L2 = vx * vx + vy * vy
            t = 0 if L2 < 1e-9 else max(0, min(1, ((x - x1) * vx + (y - y1) * vy) / L2))
            if math.hypot(x - (x1 + t * vx), y - (y1 + t * vy)) < r:
                found.add(n)
        for n, vx, vy in self.vias:
            if n != net and math.hypot(x - vx, y - vy) < r:
                found.add(n)
        return found

    def own_counts(self, net):
        own = np.zeros((2, NX, NY), np.int16)
        hw = half_width(net)
        for item in self.own_items.get(net, []):
            for layer, sl, m in item.masks(CLR + hw + SLACK):
                own[layer][sl] += m
        return own

    # ----- routing
    def obstacles(self, net):
        g = group(net)
        c = 1 if half_width(net) > HW_SIG else 0
        blocked = (self.counts[(g, c)] - self.own_counts(net)) > 0
        return blocked | self.static

    def plane_polygon(self, net):
        W, H = G.BOARD_W, G.BOARD_H
        b1, b2 = G.BARRIER
        if net == "GND":
            return (0.5, 0.5, W - 0.5, b1)
        if net.startswith("/CH") and net.endswith("/ISOGND"):
            xc = G.CH_CENTERS[int(net[3]) - 1]
            hw = G.CH_PITCH / 2 - 0.9
            return (xc - hw, b2, xc + hw, H - 0.5)
        return None

    def fanout(self, net, poly):
        """Every SMD pad of a plane net gets a short stub and a via into the B.Cu plane (done before
        anything else is routed, so nothing can wall a ground pad in).  Each stub becomes one component."""
        x1, y1, x2, y2 = poly
        m = 0.8
        inzone = np.zeros((NX, NY), bool)
        inzone[(XS >= x1 + m) & (XS <= x2 - m), :] = True
        inzone[:, (YS < y1 + m) | (YS > y2 - m)] = False
        ok = True
        comps = []
        done_centres = set()
        for item, layer, cells, centre in self.pads.get(net, []):
            if len(item.layers) > 1:
                comps.append(set((l, i, j) for l in (F, B) for i, j in cells))   # through hole
                continue
            if centre in done_centres:
                continue
            done_centres.add(centre)
            sources = {(layer, i, j) for i, j in cells}
            path = self.astar(net, sources, set(), None, target_fn=lambda l, i, j: l == B and inzone[i, j])
            comp = set(sources)
            if path is None:
                print(f"  !! no plane via for {net} pad at {centre}")
                if "--debug" in sys.argv:
                    self.debug_image(net, sources, set())
                ok = False
            else:
                self.commit(net, path, None, comp)
            comps.append(comp)
        self.comps[net] = comps
        return ok

    def plane_tree(self, net, allow_ripup=True):
        """Connect all components of a plane net (vias, through-hole pads) with tracks."""
        comps = [c for c in self.comps.get(net, []) if c]
        if len(comps) < 2:
            return True

        def centroid(c):
            xs = [i * GRID for _, i, _ in c]
            ys = [j * GRID for _, _, j in c]
            return (sum(xs) / len(xs), sum(ys) / len(ys))
        connected = set(comps[0])
        cents = [centroid(comps[0])]
        remaining = [(c, centroid(c)) for c in comps[1:]]
        ok = True
        self.ripped = set()
        while remaining:
            remaining.sort(key=lambda t: min(math.hypot(t[1][0] - a, t[1][1] - b) for a, b in cents))
            target, tc = remaining.pop(0)
            if target & connected:
                connected |= target
                cents.append(tc)
                continue
            path = self.astar(net, connected, target, tc)
            if path is None and allow_ripup:
                # transactional rip-up: tear up the nets around the target, link, re-route them without
                # further rip-up; if any of them fails, roll everything back and try a wider circle
                linked = False
                for radius in (2.5, 4.0, 6.0):
                    snap = self.snapshot()
                    conn_snap = set(connected)
                    victims = sorted((v for v in self.nets_near(net, tc[0], tc[1], radius) if not self.plane_polygon(v)), key=lambda n: (half_width(n), tie(n)))[:10]
                    for v in victims:
                        self.rip_up(v)
                    path = self.astar(net, connected, target, tc) if victims else None
                    if path is not None:
                        self.commit(net, path, None, connected)
                        saved_ripped = self.ripped
                        good = all(self.route_net(v, allow_ripup=False) for v in victims)
                        self.ripped = saved_ripped
                        if good:
                            print(f"  rip-up for {net}: {victims} (re-routed)")
                            linked = True
                            break
                    self.restore(snap)
                    connected.clear()
                    connected |= conn_snap
                    path = None
                if linked:
                    connected |= target
                    cents.append(tc)
                    continue
            if path is None:
                # the pour on both layers normally reaches it anyway; the DRC's unconnected-items
                # check is the arbiter, so do not tear up signal nets for a redundant link
                print(f"  -- no track for {net} to component at ({tc[0]:.1f}, {tc[1]:.1f}), left to the pour")
                if "--debug" in sys.argv:
                    self.debug_image(net, connected, target)
            else:
                self.commit(net, path, None, connected)
            connected |= target
            cents.append(tc)
        return ok

    def route_net(self, net, allow_ripup=True):
        terms = self.pads.get(net, [])
        if len(terms) < 2:
            return True
        self.ripped = set()
        # merge terminals that share a pad centre (THT pads appear on both layers)
        centres = {}
        for t in terms:
            centres.setdefault(t[3], []).append(t)
        groups = list(centres.values())
        # connected component: list of (layer, i, j) cells
        connected = set()
        for t in groups[0]:
            connected.update((t[1], i, j) for i, j in t[2])
        connected_centres = [groups[0][0][3]]
        remaining = groups[1:]
        ok = True
        while remaining:
            # nearest remaining terminal to the connected set
            def dist(gr):
                cx, cy = gr[0][3]
                return min(math.hypot(cx - a, cy - b) for a, b in connected_centres)
            remaining.sort(key=dist)
            target = remaining.pop(0)
            tcells = set()
            for t in target:
                tcells.update((t[1], i, j) for i, j in t[2])
            path = self.astar(net, connected, tcells, target[0][3])
            if path is None and allow_ripup:
                # rip up what runs near the target (and near the sources) and try again
                tx, ty = target[0][3]
                victims = self.nets_near(net, tx, ty)
                for cx, cy in connected_centres[-2:]:
                    victims |= self.nets_near(net, cx, cy)
                victims = sorted((v for v in victims if not self.plane_polygon(v) and not (locked(v) and not locked(net))), key=lambda n: (half_width(n), tie(n)))[:6]
                for v in victims:
                    self.rip_up(v)
                    self.ripped.add(v)
                if victims:
                    print(f"  rip-up for {net}: {victims}")
                    path = self.astar(net, connected, tcells, target[0][3])
            if path is None:
                print(f"  !! no route for {net} to pad at {target[0][3]}")
                if "--debug" in sys.argv:
                    self.debug_image(net, connected, tcells)
                ok = False
                connected_centres.append(target[0][3])
                continue
            self.commit(net, path, target[0][3], connected)
            connected_centres.append(target[0][3])
        return ok

    def astar(self, net, sources, targets, tcentre, target_fn=None):
        obs = self.obstacles(net)
        for (layer, i, j) in sources:
            obs[layer, i, j] = False
        for (layer, i, j) in targets:
            obs[layer, i, j] = False
        tx, ty = tcentre if tcentre else (0.0, 0.0)
        if target_fn is None:
            target_fn = lambda l, i, j: (l, i, j) in targets
        hscale = 1.0 if tcentre else 0.0
        hw = half_width(net)
        INF = float("inf")
        gcost = np.full((2, NX, NY), INF, np.float32)
        parent = {}
        heap = []
        tset = targets
        for (layer, i, j) in sources:
            gcost[layer, i, j] = 0.0
            h = hscale * math.hypot(i * GRID - tx, j * GRID - ty)
            heap.append((h, 0.0, layer, i, j))
        heapq.heapify(heap)
        dirs = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, 1.4142), (-1, 1, 1.4142), (1, -1, 1.4142), (-1, -1, 1.4142)]
        expanded = 0
        while heap:
            fcost, gc, layer, i, j = heapq.heappop(heap)
            if gc > gcost[layer, i, j]:
                continue
            if target_fn(layer, i, j):
                # reconstruct
                path = [(layer, i, j)]
                while (layer, i, j) in parent:
                    layer, i, j = parent[(layer, i, j)]
                    path.append((layer, i, j))
                path.reverse()
                return path
            expanded += 1
            if expanded > 400000:
                return None
            for di, dj, dl in dirs:
                ni, nj = i + di, j + dj
                if ni < 0 or nj < 0 or ni >= NX or nj >= NY or obs[layer, ni, nj]:
                    continue
                # direction preference: F horizontal, B vertical
                pref = 1.0
                if di and dj:
                    pref = 1.15
                elif layer == F and dj:
                    pref = 1.5
                elif layer == B and di:
                    pref = 1.5
                ng = gc + dl * GRID * pref
                if ng < gcost[layer, ni, nj]:
                    gcost[layer, ni, nj] = ng
                    parent[(layer, ni, nj)] = (layer, i, j)
                    h = hscale * math.hypot(ni * GRID - tx, nj * GRID - ty)
                    heapq.heappush(heap, (ng + h, ng, layer, ni, nj))
            # via
            ol = 1 - layer
            if not obs[ol, i, j] and self.via_ok(obs, i, j):
                ng = gc + VIA_COST
                if ng < gcost[ol, i, j]:
                    gcost[ol, i, j] = ng
                    parent[(ol, i, j)] = (layer, i, j)
                    h = hscale * math.hypot(i * GRID - tx, j * GRID - ty)
                    heapq.heappush(heap, (ng + h, ng, ol, i, j))
        return None

    def via_ok(self, obs, i, j):
        # the via is bigger than a track: need the 3x3 neighbourhood free on both layers
        if i < 3 or j < 3 or i > NX - 4 or j > NY - 4 or self.novia[i, j]:
            return False
        return not (obs[:, i - 2:i + 3, j - 2:j + 3] & VIA_KERNEL).any()

    def commit(self, net, path, tcentre, connected):
        hw = half_width(net)
        pts = [(l, i * GRID, j * GRID) for l, i, j in path]
        # start/end stubs to the pad centres when the path starts or ends inside a pad
        segs = []
        cur_layer = pts[0][0]
        run_start = pts[0]
        last = pts[0]
        last_dir = None
        for k in range(1, len(pts)):
            l, x, y = pts[k]
            if l != cur_layer:
                if (run_start[1], run_start[2]) != (last[1], last[2]):
                    segs.append((cur_layer, run_start[1], run_start[2], last[1], last[2]))
                self.vias.append((net, x, y))
                self.add_item(Item("disc", net, [F, B], (x, y, VIA_D / 2), routed=True))
                sl, m = disc_mask(x, y, VIA_D + 0.3)
                self.novia[sl] |= m
                connected.add((F, int(round(x / GRID)), int(round(y / GRID))))
                connected.add((B, int(round(x / GRID)), int(round(y / GRID))))
                cur_layer = l
                run_start = (l, x, y)
                last = run_start
                last_dir = None
                continue
            d = (round(x - last[1], 6), round(y - last[2], 6))
            if last_dir is not None and d != last_dir:
                segs.append((cur_layer, run_start[1], run_start[2], last[1], last[2]))
                run_start = last
            last_dir = d
            last = (l, x, y)
        if (run_start[1], run_start[2]) != (last[1], last[2]):
            segs.append((cur_layer, run_start[1], run_start[2], last[1], last[2]))
        # stub into the target pad centre
        tl, tx, ty = pts[-1]
        if tcentre is None:
            for item, layer, cells, centre in self.pads.get(net, []):
                if layer == tl and (int(round(tx / GRID)), int(round(ty / GRID))) in cells:
                    tcentre = centre
                    break
        if tcentre and (round(tx, 6), round(ty, 6)) != (round(tcentre[0], 6), round(tcentre[1], 6)):
            segs.append((tl, tx, ty, tcentre[0], tcentre[1]))
        # stub from the source pad centre if the path started inside a pad
        sl, sx, sy = pts[0]
        for item, layer, cells, centre in self.pads.get(net, []):
            if layer == sl and (int(round(sx / GRID)), int(round(sy / GRID))) in cells and (round(sx, 6), round(sy, 6)) != (round(centre[0], 6), round(centre[1], 6)):
                segs.append((sl, centre[0], centre[1], sx, sy))
                break
        for layer, x1, y1, x2, y2 in segs:
            self.tracks.append((net, layer, x1, y1, x2, y2, hw))
            self.add_item(Item("seg", net, [layer], (x1, y1, x2, y2), hw, routed=True))
        for l, i, j in path:
            connected.add((l, i, j))

    def stitch(self, net, poly, pitch=2.4):
        """Tie the two pours of a plane net together.  The pours are modelled as the cells that are free for
        this net (eroded by one cell, so only solid copper counts); their connected pieces are found per
        layer and joined through the net's own vias and through-hole pads.  Then (1) a coarse stitching
        grid and (2) targeted vias wherever a piece that is still separate overlaps another piece on the
        other layer.  Returns (vias added, separate pieces left that carry a pad or via)."""
        from collections import deque
        x1, y1, x2, y2 = poly
        obs = self.obstacles(net)
        inz = np.zeros((NX, NY), bool)
        inz[(XS >= x1 + 0.4) & (XS <= x2 - 0.4), :] = True
        inz[:, (YS < y1 + 0.4) | (YS > y2 - 0.4)] = False
        free = (~obs) & inz[None, :, :]
        solid = free.copy()                       # erode: a cell counts if its 4 neighbours are free too
        solid[:, 1:, :] &= free[:, :-1, :]
        solid[:, :-1, :] &= free[:, 1:, :]
        solid[:, :, 1:] &= free[:, :, :-1]
        solid[:, :, :-1] &= free[:, :, 1:]

        def add_via(i, j):
            vx, vy = i * GRID, j * GRID
            self.vias.append((net, vx, vy))
            self.add_item(Item("disc", net, [F, B], (vx, vy, VIA_D / 2), routed=True))
            sl, m = disc_mask(vx, vy, VIA_D + 0.3)
            self.novia[sl] |= m

        added = 0
        lab = np.zeros((2, NX, NY), np.int32)     # (2) pieces
        n = 0
        for l in (F, B):
            for i0, j0 in zip(*np.nonzero(solid[l])):
                if lab[l, i0, j0]:
                    continue
                n += 1
                lab[l, i0, j0] = n
                q = deque([(i0, j0)])
                while q:
                    i, j = q.popleft()
                    for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        a, b = i + di, j + dj
                        if 0 <= a < NX and 0 <= b < NY and solid[l, a, b] and not lab[l, a, b]:
                            lab[l, a, b] = n
                            q.append((a, b))
        parent = list(range(n + 1))

        def find(k):
            while parent[k] != k:
                parent[k] = parent[parent[k]]
                k = parent[k]
            return k

        def near(l, i, j, r=2):                   # piece label next to a point (a via barrel sits in a gap of its own)
            best = 0
            for a in range(max(0, i - r), min(NX, i + r + 1)):
                for b in range(max(0, j - r), min(NY, j + r + 1)):
                    if lab[l, a, b]:
                        best = lab[l, a, b]
            return best
        anchors = [(int(round(vx / GRID)), int(round(vy / GRID))) for nn, vx, vy in self.vias if nn == net]
        anchors += [(int(round(c[0] / GRID)), int(round(c[1] / GRID))) for item, layer, cells, c in self.pads.get(net, []) if len(item.layers) > 1]
        has_item = set()
        for i, j in anchors:
            a, b = near(F, i, j), near(B, i, j)
            if a and b:
                parent[find(a)] = find(b)
            has_item.update(k for k in (a, b) if k)
        for item, layer, cells, c in self.pads.get(net, []):
            k = near(layer, int(round(c[0] / GRID)), int(round(c[1] / GRID)), 4)
            if k:
                has_item.add(k)
        sizes = np.bincount(lab.ravel(), minlength=n + 1)
        for _ in range(60):                       # join the pieces that are still separate
            roots = {}
            for k in range(1, n + 1):
                roots.setdefault(find(k), 0)
                roots[find(k)] += sizes[k]
            main = max(roots, key=roots.get)
            both = (lab[F] > 0) & (lab[B] > 0)
            done = False
            for i, j in zip(*np.nonzero(both)):
                a, b = find(lab[F, i, j]), find(lab[B, i, j])
                if a != b and self.via_ok(obs, i, j) and not self.novia[i, j]:
                    add_via(i, j)
                    added += 1
                    parent[a] = b
                    has_item.update((lab[F, i, j], lab[B, i, j]))
                    done = True
                    break
            if not done:
                break
        roots = {}
        for k in range(1, n + 1):
            roots.setdefault(find(k), 0)
            roots[find(k)] += sizes[k]
        main = max(roots, key=roots.get)
        row = 0
        y = y1 + 1.0
        while y <= y2 - 1.0:                      # coarse grid, only where both layers belong to the main piece
            x = x1 + 1.0 + (pitch / 2 if row % 2 else 0.0)
            while x <= x2 - 1.0:
                i, j = int(round(x / GRID)), int(round(y / GRID))
                if lab[F, i, j] and lab[B, i, j] and find(lab[F, i, j]) == main and find(lab[B, i, j]) == main and self.via_ok(obs, i, j):
                    add_via(i, j)
                    added += 1
                x += pitch
            y += pitch * 0.866
            row += 1
        roots = {}
        for k in range(1, n + 1):
            roots.setdefault(find(k), [0, False])
            roots[find(k)][0] += sizes[k]
            roots[find(k)][1] |= k in has_item
        main = max(roots, key=lambda k: roots[k][0])
        left = [(k, v[0]) for k, v in roots.items() if k != main and v[1]]
        for k, size in left:
            cells = np.argwhere(np.isin(lab, [m for m in range(1, n + 1) if find(m) == k]))
            l0, i0, j0 = cells.min(axis=0)
            l1, i1, j1 = cells.max(axis=0)
            print(f"     piece on {'F' if l0 == 0 else 'B'}{'+B' if l1 != l0 else ''}: {size * GRID * GRID:6.1f} mm2  x[{i0 * GRID:.1f},{i1 * GRID:.1f}] y[{j0 * GRID:.1f},{j1 * GRID:.1f}]")
        return added, len(left)

    def debug_image(self, net, sources, targets):
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        obs = self.obstacles(net)
        fig, axs = plt.subplots(1, 2, figsize=(14, 9))
        for layer, ax in enumerate(axs):
            img = np.zeros((NY, NX, 3))
            img[..., 0] = obs[layer].T
            img[..., 1] = self.static[layer].T * 0.5
            for (l, i, j) in sources:
                if l == layer:
                    img[j, i] = (0, 0, 1)
            for (l, i, j) in targets:
                if l == layer:
                    img[j, i] = (0, 1, 0)
            ax.imshow(img, origin="upper", extent=(0, NX * GRID, NY * GRID, 0))
            if targets:
                tx = sum(i for _, i, _ in targets) / len(targets) * GRID
                ty = sum(j for _, _, j in targets) / len(targets) * GRID
                ax.set_xlim(tx - 8, tx + 8)
                ax.set_ylim(ty + 8, ty - 8)
            ax.set_title(f"{net} layer {'F' if layer == 0 else 'B'} (red=blocked, blue=src, green=target)")
        fig.tight_layout()
        self._dbg = getattr(self, "_dbg", 0) + 1
        fig.savefig(os.path.join(HERE, f"debug_{net.replace('/', '_')}_{self._dbg}.png"), dpi=110)
        plt.close(fig)

    # ----- output
    def prune_stubs(self, max_len=0.6):
        """Drop short track ends that lead nowhere (left behind by rip-up and re-route)."""
        pads = [it for lst in self.own_items.values() for it in lst if not it.routed]

        def anchored(net, layer, x, y, me):
            for k, (n, l, x1, y1, x2, y2, hw) in enumerate(self.tracks):
                if k == me or n != net or l != layer:
                    continue
                dx, dy = x2 - x1, y2 - y1
                L2 = dx * dx + dy * dy
                t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / L2))
                if math.hypot(x - (x1 + t * dx), y - (y1 + t * dy)) < 0.02:
                    return True
            if any(n == net and math.hypot(x - vx, y - vy) < VIA_D / 2 for n, vx, vy in self.vias):
                return True
            for it in pads:
                if it.net != net or layer not in it.layers:
                    continue
                if it.kind == "disc":
                    cx, cy, r = it.geom
                    if math.hypot(x - cx, y - cy) <= r + 0.05:
                        return True
                else:
                    cx, cy, w, h, rot = it.geom
                    a = math.radians(rot)
                    u = (x - cx) * math.cos(a) - (y - cy) * math.sin(a)
                    v = (x - cx) * math.sin(a) + (y - cy) * math.cos(a)
                    # a rotated rectangle: test both senses of rotation, pads are symmetric
                    u2 = (x - cx) * math.cos(a) + (y - cy) * math.sin(a)
                    v2 = -(x - cx) * math.sin(a) + (y - cy) * math.cos(a)
                    if (abs(u) <= w / 2 + 0.05 and abs(v) <= h / 2 + 0.05) or (abs(u2) <= w / 2 + 0.05 and abs(v2) <= h / 2 + 0.05):
                        return True
            return False

        removed = 0
        for _ in range(6):
            drop = set()
            for k, (net, layer, x1, y1, x2, y2, hw) in enumerate(self.tracks):
                if math.hypot(x2 - x1, y2 - y1) > max_len:
                    continue
                if not anchored(net, layer, x1, y1, k) or not anchored(net, layer, x2, y2, k):
                    drop.add(k)
            if not drop:
                break
            self.tracks = [t for k, t in enumerate(self.tracks) if k not in drop]
            removed += len(drop)
        return removed

    def sexp(self, net_idx):
        o = []
        for k, (net, layer, x1, y1, x2, y2, hw) in enumerate(self.tracks):
            ln = "F.Cu" if layer == F else "B.Cu"
            o.append(f'  (segment (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (width {f(2 * hw)}) (layer "{ln}") (net {net_idx[net]}) (tstamp {U("seg" + str(k))}))')
        for k, (net, x, y) in enumerate(self.vias):
            o.append(f'  (via (at {f(x)} {f(y)}) (size {f(VIA_D)}) (drill {f(VIA_DRILL)}) (layers "F.Cu" "B.Cu") (net {net_idx[net]}) (tstamp {U("via" + str(k))}))')
        return "\n".join(o)


def main():
    t0 = time.time()
    placed, nets = G.build()
    net_idx = {n: i + 1 for i, n in enumerate(nets)}
    r = Router(placed)
    print(f"static model built in {time.time() - t0:.1f}s, {len(nets)} nets")
    # order: short nets first, power last
    def est(net):
        terms = r.pads.get(net, [])
        cs = sorted({t[3] for t in terms})
        if len(cs) < 2:
            return 0
        return sum(math.hypot(cs[k][0] - cs[k - 1][0], cs[k][1] - cs[k - 1][1]) for k in range(1, len(cs)))
    IC_NETS = {"T1IN", "BUF_OUT", "RXA", "TXA", "RX_O", "R1OUT", "R1IN", "T1OUT", "RJ3", "RJ6", "SWA1", "SWA2", "SWB1", "SWB2", "SWK"}

    def prio(net):
        if net.startswith("/CH") and net.split("/")[2] in IC_NETS:
            return 0            # pins of the optocouplers / MAX3232 / buffer first
        if net.startswith("/CH") and net.endswith("VISO"):
            return 0.6          # island supply, right after the other island nets
        if net.startswith("/CH") and not net.endswith("ISOGND"):
            return 0.5          # charge pump caps, LED, jack
        if net.startswith("ISO_IN") or net in ("SW", "VBAT", "BATP", "+5V", "VBUS"):
            return 1
        if net.startswith("ESP_") or net in ("SDA", "SCL", "FB", "PWR_EN", "TXD0") or net.startswith("PG") or net.startswith("IO") or net.startswith("SWAP"):
            return 2
        if net.startswith("EN"):
            return 3
        if net == "+3V3":
            return 5
        return 6                # GND and the ISOGND islands (also covered by pours)
    order = sorted(nets, key=lambda n: (prio(n), est(n)))
    if "--debug" in sys.argv:
        dbg = sys.argv[sys.argv.index("--debug") + 1]
        order = [n for n in order if prio(n) <= prio(dbg) and n != dbg] + [dbg]
    failed = []
    plane = [n for n in order if r.plane_polygon(n)]
    for net in plane:
        t1 = time.time()
        ok = r.fanout(net, r.plane_polygon(net))
        print(f"fanout {net:22s} {'ok' if ok else 'FAILED'}  {time.time() - t1:5.1f}s  vias={len(r.vias)}")
        if not ok:
            failed.append(net)
    queue = [n for n in order if n not in plane]
    done = set()
    rerouted = {}

    def run(queue):
        while queue:
            net = queue.pop(0)
            t1 = time.time()
            ok = r.route_net(net, allow_ripup=rerouted.get(net, 0) < 2)
            done.add(net)
            print(f"[{len(done):3d}/{len(order)}] {net:22s} {'ok' if ok else 'FAILED'}  {time.time() - t1:5.1f}s  tracks={len(r.tracks)} vias={len(r.vias)}")
            if not ok:
                failed.append(net)
            for v in sorted(r.ripped):
                rerouted[v] = rerouted.get(v, 0) + 1
                if v in failed:
                    failed.remove(v)
                queue.insert(0, v)   # re-route the victims right away

    def tree(net):
        t1 = time.time()
        ok = r.plane_tree(net)
        print(f"tree   {net:22s} {'ok' if ok else 'FAILED'}  {time.time() - t1:5.1f}s  tracks={len(r.tracks)} vias={len(r.vias)}")
        if not ok:
            failed.append(net)

    # 1. the channel-local nets (islands and the short ESP-side nets), 2. the island ground trees - an island
    # shares no routing space with the long ESP-side nets, so its ground can be tied up before those exist,
    # 3. the long nets, 4. the ESP-side ground tree, 5. stitching.
    run([n for n in queue if prio(n) < 1])
    for net in plane:
        if net != "GND":
            tree(net)
    run([n for n in queue if prio(n) >= 1])
    for net in plane:
        if net != "GND":
            continue
        t1 = time.time()
        ok = r.plane_tree(net)
        print(f"tree   {net:22s} {'ok' if ok else 'FAILED'}  {time.time() - t1:5.1f}s  tracks={len(r.tracks)} vias={len(r.vias)}")
        if not ok:
            failed.append(net)
    for net in plane:
        nv, left = r.stitch(net, r.plane_polygon(net))
        print(f"stitch {net:22s} {nv:3d} vias, {left} separate piece(s) left")
    failed = sorted(set(failed))
    print(f"pruned {r.prune_stubs()} dangling stub(s)")
    pcb = os.path.join(G.OUT, f"{G.PROJECT}.kicad_pcb")
    txt = open(pcb).read()
    idx = txt.rstrip().rfind(")")
    txt = txt[:idx] + r.sexp(net_idx) + "\n)\n"
    open(pcb, "w").write(txt)
    print(f"routing done in {time.time() - t0:.0f}s (seed {SEED}): {len(r.tracks)} segments, {len(r.vias)} vias, failed nets: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
