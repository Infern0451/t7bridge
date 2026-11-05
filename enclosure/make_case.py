#!/usr/bin/env python3
"""
Sealed enclosure for the T7 Bridge (ESP32-S3 DevKitC-style board, no headers).

Generates three STL files:
  t7bridge_base.stl    - tray holding the board and the USB-C plug
  t7bridge_lid.stl     - lid (print top-face down; the locating lip faces up)
  t7bridge_gasket.stl  - optional TPU gasket for the groove (or use RTV silicone)

Design:
  * Board pocket + a bay for a STRAIGHT USB-C plug, so the plug is inside the
    box. The cable leaves through a notch in the end wall; the lid has a tab
    that closes the notch down to the cable. Seal the notch with a dab of RTV.
  * A groove around the rim takes the gasket or a thin bead of RTV silicone.
  * No screws: two zip ties (up to 5 mm wide) around the box clamp the lid,
    sitting in shallow channels on the top and bottom.
  * All outside edges filleted (the zip-tie channels stay crisp).
  * Flat bottom between the zip-tie channels for 3M VHB tape.
  * All plastic around the antenna end (keep metal away from it).

How it works: each part is described as a signed distance field (negative
inside) built from boxes and a rounded box, then its surface is traced on a
fine grid (surface nets). Only needs Python 3 and numpy:

    python3 make_case.py

Every dimension below is in millimetres and can be changed.
"""
import struct
import sys
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------- parameters
BOARD_L = 57.0 + 6.0     # board length incl. antenna tab
BOARD_W = 28.1
BOARD_H = 4.8            # bottom of PCB to top of tallest part (USB ports)
FIT = 0.5                # clearance around the board, each side

PRESS_FIT = 0.2          # clearance for parts that slot together (printer-tuned)

PLUG_BAY = 20.0          # room for a straight USB-C plug + strain relief
CABLE_D = 4.4            # cable diameter + clearance (cable OD up to ~4.2 mm)
RIB_H = 1.0              # board sits on ribs this high
PORT_ABOVE_PCB = 3.2     # USB-C centre above the PCB bottom

FLOOR = 2.5
WALL_IN, GROOVE_W, WALL_OUT = 1.2, 1.2, 1.2      # wall = inner + groove + outer
GROOVE_D = 1.2
CAVITY_H = 8.5           # inside height (fits board on ribs + plug overmold)
LID_T = 3.0
LIP_T, LIP_H = 1.2, 1.8
FILLET = 1.5             # radius on all outside edges

TIE_W, TIE_D = 5.4, 0.8  # zip-tie channels (width, depth)
TIE_X = (19.0, 72.0)     # channel centres along the length

GASKET_W = GROOVE_W - 2 * 0.1    # TPU gasket, 0.1 mm side clearance in groove
GASKET_H = GROOVE_D + 0.5        # 0.5 mm squeeze when the lid is clamped

VOXEL = 0.25             # surface tracing resolution (smaller = smoother, bigger files)

# ---------------------------------------------------------------- derived
WALL = WALL_IN + GROOVE_W + WALL_OUT
POCKET_L = BOARD_L + 2 * FIT
CAV_L = POCKET_L + PLUG_BAY
CAV_W = BOARD_W + 2 * FIT
OUT_L = CAV_L + 2 * WALL
OUT_W = CAV_W + 2 * WALL
BASE_H = FLOOR + CAVITY_H
TOTAL_H = BASE_H + LID_T
CABLE_Z = FLOOR + RIB_H + PORT_ABOVE_PCB          # cable centre height
NOTCH_Z0 = CABLE_Z - CABLE_D / 2                  # notch bottom
NOTCH_Y0 = OUT_W / 2 - CABLE_D / 2
NOTCH_Y1 = OUT_W / 2 + CABLE_D / 2
END_X0 = OUT_L - WALL                             # end wall (cable side)
BIG = 1000.0

# ---------------------------------------------------------------- SDF helpers
def sdf_box(P, lo, hi):
    """Exact signed distance to an axis-aligned box (negative inside)."""
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    c, h = (lo + hi) / 2, (hi - lo) / 2
    q = np.abs(P - c) - h
    outside = np.linalg.norm(np.maximum(q, 0), axis=-1)
    inside = np.minimum(q.max(axis=-1), 0)
    return outside + inside


def sdf_round_box(P, lo, hi, r):
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    return sdf_box(P, lo + r, hi - r) - r


def union(*d):
    return np.minimum.reduce(d)


def cut(a, *b):
    return np.maximum.reduce([a] + [-x for x in b])


def frame(P, x0, x1, y0, y1, t, z0, z1):
    return cut(sdf_box(P, (x0, y0, z0), (x1, y1, z1)),
               sdf_box(P, (x0 + t, y0 + t, z0 - 1), (x1 - t, y1 - t, z1 + 1)))


def tie_channels(P, z0, z1):
    return union(*[sdf_box(P, (c - TIE_W / 2, -1, z0), (c + TIE_W / 2, OUT_W + 1, z1))
                   for c in TIE_X])


def outer_shell(P):
    return sdf_round_box(P, (0, 0, 0), (OUT_L, OUT_W, TOTAL_H), FILLET)


# ---------------------------------------------------------------- parts
def base_sdf(P):
    z = P[..., 2]
    d = np.maximum(outer_shell(P), z - BASE_H)               # below the parting line
    cavity = sdf_box(P, (WALL, WALL, FLOOR), (OUT_L - WALL, OUT_W - WALL, BIG))
    groove = cut(sdf_box(P, (WALL_OUT, WALL_OUT, BASE_H - GROOVE_D),
                         (OUT_L - WALL_OUT, OUT_W - WALL_OUT, BIG)),
                 sdf_box(P, (WALL_OUT + GROOVE_W, WALL_OUT + GROOVE_W, -BIG),
                         (OUT_L - WALL_OUT - GROOVE_W, OUT_W - WALL_OUT - GROOVE_W, BIG)))
    notch = sdf_box(P, (END_X0 - 0.5, NOTCH_Y0, NOTCH_Z0), (BIG, NOTCH_Y1, BIG))
    d = cut(d, cavity, groove, notch, tie_channels(P, -BIG, TIE_D))
    # ribs under the board
    ribs = [sdf_box(P, (x - 0.75, WALL - 0.1, FLOOR - 0.1), (x + 0.75, OUT_W - WALL + 0.1, FLOOR + RIB_H))
            for x in (WALL + 7, WALL + POCKET_L / 2, WALL + POCKET_L - 7)]
    # stops at the USB end of the board (corners only, the plug passes between)
    px = WALL + POCKET_L
    stops = [sdf_box(P, (px, y0, FLOOR - 0.1), (px + 1.5, y1, FLOOR + RIB_H + 3.0))
             for y0, y1 in ((WALL - 0.1, WALL + 1.6), (OUT_W - WALL - 1.6, OUT_W - WALL + 0.1))]
    return union(d, *ribs, *stops)


def lid_sdf(P):
    z = P[..., 2]
    plate = np.maximum(outer_shell(P), BASE_H - z)          # above the parting line
    plate = cut(plate, tie_channels(P, TOTAL_H - TIE_D, BIG))
    f = PRESS_FIT
    lip = frame(P, WALL + f, OUT_L - WALL - f, WALL + f, OUT_W - WALL - f, LIP_T,
                BASE_H - LIP_H, BASE_H + 0.1)
    tab = sdf_box(P, (END_X0, NOTCH_Y0 + f, NOTCH_Z0 + CABLE_D),
                  (OUT_L - FILLET * 0.3, NOTCH_Y1 - f, BASE_H + 0.1))
    return union(plate, lip, tab)


# ---------------------------------------------------------------- surface nets
def surface_nets(sdf, lo, hi, h):
    """Trace the zero level of sdf on a grid of spacing h -> (verts, quads)."""
    lo = np.asarray(lo, float) - 2 * h
    hi = np.asarray(hi, float) + 2 * h
    axes = [np.arange(lo[i], hi[i] + h, h) for i in range(3)]
    X, Y, Z = np.meshgrid(*axes, indexing="ij")
    F = sdf(np.stack([X, Y, Z], axis=-1)).astype(np.float64)
    F[F == 0] = 1e-9
    nx, ny, nz = F.shape
    inside = F < 0

    # corners of each cell
    offs = [(i, j, k) for i in (0, 1) for j in (0, 1) for k in (0, 1)]
    edges = [(a, b) for a in range(8) for b in range(a + 1, 8)
             if sum(abs(np.subtract(offs[a], offs[b]))) == 1]
    cs = [F[i:nx - 1 + i, j:ny - 1 + j, k:nz - 1 + k] for i, j, k in offs]
    cmin = np.minimum.reduce(cs)
    cmax = np.maximum.reduce(cs)
    active = (cmin < 0) & (cmax > 0)
    idx = np.argwhere(active)
    vals = np.stack([c[active] for c in cs], axis=1)            # (n, 8)
    acc = np.zeros((len(idx), 3))
    cnt = np.zeros(len(idx))
    for a, b in edges:
        va, vb = vals[:, a], vals[:, b]
        m = (va < 0) != (vb < 0)
        t = va[m] / (va[m] - vb[m])
        pa = np.array(offs[a], float)
        pb = np.array(offs[b], float)
        acc[m] += pa + t[:, None] * (pb - pa)
        cnt[m] += 1
    verts = lo + (idx + acc / cnt[:, None]) * h
    vid = -np.ones(active.shape, dtype=np.int64)
    vid[active] = np.arange(len(idx))

    quads = []
    # x-edges: point (i,j,k)-(i+1,j,k), cells (i, j-1..j, k-1..k)
    s = inside[:-1, 1:-1, 1:-1] != inside[1:, 1:-1, 1:-1]
    i, j, k = np.nonzero(s); j += 1; k += 1
    flip = inside[i, j, k]
    q = np.stack([vid[i, j - 1, k - 1], vid[i, j, k - 1], vid[i, j, k], vid[i, j - 1, k]], 1)
    quads.append(np.where(flip[:, None], q[:, ::-1], q))
    # y-edges: cells (i-1..i, j, k-1..k)
    s = inside[1:-1, :-1, 1:-1] != inside[1:-1, 1:, 1:-1]
    i, j, k = np.nonzero(s); i += 1; k += 1
    flip = inside[i, j, k]
    q = np.stack([vid[i - 1, j, k - 1], vid[i - 1, j, k], vid[i, j, k], vid[i, j, k - 1]], 1)
    quads.append(np.where(flip[:, None], q[:, ::-1], q))
    # z-edges: cells (i-1..i, j-1..j, k)
    s = inside[1:-1, 1:-1, :-1] != inside[1:-1, 1:-1, 1:]
    i, j, k = np.nonzero(s); i += 1; j += 1
    flip = inside[i, j, k]
    q = np.stack([vid[i - 1, j - 1, k], vid[i, j - 1, k], vid[i, j, k], vid[i - 1, j, k]], 1)
    quads.append(np.where(flip[:, None], q[:, ::-1], q))
    quads = np.concatenate(quads)
    # Orientation convention check: make the enclosed volume positive.
    tris = np.concatenate([quads[:, [0, 1, 2]], quads[:, [0, 2, 3]]])
    v = verts[tris]
    vol = np.einsum("ij,ij->i", v[:, 0], np.cross(v[:, 1], v[:, 2])).sum() / 6
    if vol < 0:
        tris = tris[:, ::-1]
    return verts, tris


# ---------------------------------------------------------------- box mesher (gasket)
def union_mesh(boxes):
    """Exact union of axis-aligned boxes -> (verts, tris)."""
    xs = sorted({v for b in boxes for v in (b[0], b[1])})
    ys = sorted({v for b in boxes for v in (b[2], b[3])})
    zs = sorted({v for b in boxes for v in (b[4], b[5])})
    filled = np.zeros((len(xs) - 1, len(ys) - 1, len(zs) - 1), dtype=bool)
    for x0, x1, y0, y1, z0, z1 in boxes:
        filled[xs.index(x0):xs.index(x1), ys.index(y0):ys.index(y1), zs.index(z0):zs.index(z1)] = True
    pad = np.pad(filled, 1)
    X, Y, Z = np.array(xs), np.array(ys), np.array(zs)
    out = []
    for axis in range(3):
        sl_hi = [slice(1, -1)] * 3; sl_hi[axis] = slice(1, None)
        sl_lo = [slice(1, -1)] * 3; sl_lo[axis] = slice(None, -1)
        diff = pad[tuple(sl_hi)].astype(int) - pad[tuple(sl_lo)].astype(int)
        for c in zip(*np.nonzero(diff)):
            c = list(c)
            u, w = [a for a in range(3) if a != axis]
            G = (X, Y, Z)
            base = [0, 0, 0]
            base[axis] = G[axis][c[axis]]
            p = []
            for du, dw in ((0, 0), (1, 0), (1, 1), (0, 1)):
                v = list(base)
                v[u] = G[u][c[u] + du]
                v[w] = G[w][c[w] + dw]
                p.append(v)
            # outward normal is -axis when solid is on the + side
            outward_neg = diff[tuple(c)] > 0
            right_handed = (u, w) in ((0, 1), (1, 2), (2, 0))
            if outward_neg == right_handed:
                p = p[::-1]
            out.append((p[0], p[1], p[2]))
            out.append((p[0], p[2], p[3]))
    tris = np.array(out, float)
    verts, inv = np.unique(tris.reshape(-1, 3).round(6), axis=0, return_inverse=True)
    return verts, inv.reshape(-1, 3)


# ---------------------------------------------------------------- output / checks
def write_stl(path, verts, tris, flip_z_at=None):
    v = verts.copy()
    t = tris.copy()
    if flip_z_at is not None:                 # print the part upside down
        v[:, 2] = flip_z_at - v[:, 2]
        t = t[:, [0, 2, 1]]
    p = v[t]
    n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"] = n
    rec["v"] = p
    with open(path, "wb") as f:
        f.write(b"t7bridge enclosure".ljust(80, b" "))
        f.write(struct.pack("<I", len(t)))
        f.write(rec.tobytes())


def check(verts, tris, name):
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    und = np.sort(e, axis=1)
    _, counts = np.unique(und, axis=0, return_counts=True)
    open_or_nonmanifold = int(np.sum(counts != 2))
    # every directed edge must appear once (consistent orientation)
    _, dcounts = np.unique(e, axis=0, return_counts=True)
    bad_dir = int(np.sum(dcounts != 1))
    p = verts[tris]
    vol = np.einsum("ij,ij->i", p[:, 0], np.cross(p[:, 1], p[:, 2])).sum() / 6
    ok = open_or_nonmanifold == 0 and bad_dir == 0 and vol > 0
    print(f"  {name}: {len(tris)} triangles, volume {vol / 1000:.2f} cm3, "
          f"{'watertight' if ok else f'PROBLEM ({open_or_nonmanifold} bad edges, {bad_dir} misoriented)'}")
    return ok


def gasket_boxes():
    g0 = WALL_OUT + (GROOVE_W - GASKET_W) / 2
    x0, x1, y0, y1 = g0, OUT_L - g0, g0, OUT_W - g0
    t = GASKET_W
    ny0, ny1 = NOTCH_Y0 - 0.5, NOTCH_Y1 + 0.5       # open at the cable notch
    return [
        (x0, x1, y0, y0 + t, 0, GASKET_H), (x0, x1, y1 - t, y1, 0, GASKET_H),
        (x0, x0 + t, y0, y1, 0, GASKET_H),
        (x1 - t, x1, y0, ny0, 0, GASKET_H), (x1 - t, x1, ny1, y1, 0, GASKET_H),
    ]


def main(outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    print(f"Outside: {OUT_L:.1f} x {OUT_W:.1f} x {TOTAL_H:.1f} mm "
          f"(base {BASE_H:.1f} + lid {LID_T:.1f}), fillet {FILLET} mm")
    ok = True
    v, t = surface_nets(base_sdf, (0, 0, 0), (OUT_L, OUT_W, BASE_H), VOXEL)
    ok &= check(v, t, "base")
    write_stl(outdir / "t7bridge_base.stl", v, t)
    v, t = surface_nets(lid_sdf, (0, 0, BASE_H - LIP_H), (OUT_L, OUT_W, TOTAL_H), VOXEL)
    ok &= check(v, t, "lid")
    write_stl(outdir / "t7bridge_lid.stl", v, t, flip_z_at=TOTAL_H)
    v, t = union_mesh(gasket_boxes())
    ok &= check(v, t, "gasket")
    write_stl(outdir / "t7bridge_gasket.stl", v, t)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "stl"))
