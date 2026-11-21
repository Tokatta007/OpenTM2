"""A clean network background: NETMAIN's glass, flames and cable without its
title, boxes and labels.

    py tools/art/net_background.py [--out extracted/menus-clean]

The PC version's network screens (SHELLDB3) each put the flame somewhere
else, so they can't fill in for one another; but NETMAIN's glass is the
shared glass picture (SHELLDB2 GLASS2), darker, at the same place. So under
the title, the boxes and the labels the glass is taken from GLASS2, mapped to
NETMAIN's colors (a fitted scale and offset per channel), and what NETMAIN
adds there (the flame's glow, the edges' shading) is carried in from around
each covered area: the difference between NETMAIN and the mapped glass,
smoothed, filled in as smoothly as it can be (a harmonic fill: each covered
pixel the average of its neighbors), so the glow fades out evenly. Near the
edges the flame's texture (the difference less its smoothing) is mirrored in
from just outside, fading over DETAIL_FADE pixels.

Where the two thin boxes cross the flame (QUILT) that would leave a soft,
textureless glow, so there the flame is rebuilt from the flame itself:
6x6 blocks, right to left, each copied from the uncovered picture nearby
(SOURCE, without the cable) where its surroundings best match what is
already in place around the block and its colors the smooth fill's; the
blocks overlap and blend at their margins; a smooth offset (harmonic, as
above) makes them meet the picture at the boxes' edges without a line; and
they fade into the smooth fill at the left.

Writes NETMAIN_clean.png and NETMAIN_clean_marked.png (covered areas tinted).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "art"))
from clean_menus import load_png, save_png  # noqa: E402

# Covered areas on NETMAIN, x0, y0, x1, y1 (exclusive), with a margin for
# the borders' antialiasing.
COVERED = [
    (8, 6, 558, 42),       # MULTIPLAYER OPTIONS
    (13, 96, 282, 115),    # SELECT NETWORK PROVIDER:
    (13, 112, 314, 286),   # its box
    (13, 288, 186, 308),   # DRIVER NAME:
    (13, 306, 314, 334),   # its box
    (13, 338, 180, 358),   # DRIVER ALIAS:
    (13, 355, 314, 383),   # its box
]
# Plain glass on NETMAIN (no flame, no text), for fitting the color map.
FIT = [(0, 386, 260, 480), (0, 120, 12, 380), (0, 44, 300, 94)]
SMOOTH = 4  # radius of the difference's smoothing
DETAIL_FADE = 10.0
SWEEPS = 400
QUILT = [(236, 306, 314, 334), (236, 355, 314, 383)]  # x0, y0, x1, y1
SOURCE = (236, 268, 470, 472)
BLOCK, RING, GUIDE = 6, 4, 0.25  # block size, matched surroundings, weight of the smooth fill
WHITE = 150  # a source pixel this bright in every channel is the cable or a glint
FEATHER = 14  # pixels over which the copied flame fades into the smooth fill  # over-relaxed Gauss-Seidel sweeps for the harmonic fill


def quilt(net, diff, smooth, cov, w, h):
    """Rebuilds the flame's texture in QUILT (see the top); writes smooth."""
    win = BLOCK + 2 * RING
    sx0, sy0, sx1, sy1 = SOURCE
    # source windows: uncovered, without the cable or white glints
    def usable(x, y):
        return not cov[y][x] and min(net[y][x]) < WHITE
    cands = [(x, y) for y in range(sy0, sy1 - win, 2) for x in range(sx0, sx1 - win, 2)
             if all(usable(xx, yy) for yy in range(y, y + win) for xx in range(x, x + win))]
    for qx0, qy0, qx1, qy1 in QUILT:
        acc = {}  # (x, y): [r, g, b, weight] of the blocks copied in
        for bx in range(qx1 - BLOCK, qx0 - BLOCK, -BLOCK):
            for by in range(qy0, qy1, BLOCK):
                ctx = []  # (dx, dy, target color, weight)
                for dy in range(-RING, BLOCK + RING):
                    for dx in range(-RING, BLOCK + RING):
                        x, y = bx + dx, by + dy
                        if not (0 <= x < w and 0 <= y < h):
                            continue
                        if not cov[y][x]:
                            ctx.append((dx, dy, diff[y][x], 1.0))
                        elif (x, y) in acc:
                            a = acc[(x, y)]
                            ctx.append((dx, dy, (a[0] / a[3], a[1] / a[3], a[2] / a[3]), 1.0))
                        elif 0 <= dx < BLOCK and 0 <= dy < BLOCK:
                            ctx.append((dx, dy, smooth[y][x], GUIDE))
                best, pick = None, None
                for cx, cy in cands:
                    c = 0.0
                    for dx, dy, t, wt in ctx:
                        q = diff[cy + RING + dy][cx + RING + dx]
                        c += ((q[0] - t[0]) ** 2 + (q[1] - t[1]) ** 2 + (q[2] - t[2]) ** 2) * wt
                        if best is not None and c >= best:
                            break
                    if best is None or c < best:
                        best, pick = c, (cx + RING, cy + RING)
                # copy the block and its margin, feathered at the margin
                for dy in range(-RING, BLOCK + RING):
                    for dx in range(-RING, BLOCK + RING):
                        x, y = bx + dx, by + dy
                        if not (0 <= x < w and 0 <= y < h) or not cov[y][x]:
                            continue
                        edge = min(dx + RING, dy + RING, BLOCK + RING - 1 - dx, BLOCK + RING - 1 - dy)
                        wt = min(1.0, (edge + 1) / (RING + 1))
                        q = diff[pick[1] + dy][pick[0] + dx]
                        a = acc.setdefault((x, y), [0.0, 0.0, 0.0, 0.0])
                        a[0] += q[0] * wt; a[1] += q[1] * wt; a[2] += q[2] * wt; a[3] += wt
        q = {xy: (a[0] / a[3], a[1] / a[3], a[2] / a[3]) for xy, a in acc.items()}
        # seamless: add the smooth offset (harmonic) that makes the copy meet
        # the picture around it at the top, bottom and right edges
        def edge(pairs):  # (outside, inside) pixels along one edge -> smoothed mismatches
            m = [tuple(diff[o[1]][o[0]][k] - q[i][k] for k in range(3)) for o, i in pairs]
            r = 3
            return [tuple(sum(m[j][k] for j in range(max(0, n - r), min(len(m), n + r + 1))) /
                          (min(len(m), n + r + 1) - max(0, n - r)) for k in range(3)) for n in range(len(m))]
        fix = {}
        for (x, y), v in zip([(x, qy0 - 1) for x in range(qx0, qx1)],
                             edge([((x, qy0 - 1), (x, qy0)) for x in range(qx0, qx1)])):
            fix[(x, y)] = v
        for (x, y), v in zip([(x, qy1) for x in range(qx0, qx1)],
                             edge([((x, qy1), (x, qy1 - 1)) for x in range(qx0, qx1)])):
            fix[(x, y)] = v
        for (x, y), v in zip([(qx1, y) for y in range(qy0, qy1)],
                             edge([((qx1, y), (qx1 - 1, y)) for y in range(qy0, qy1)])):
            fix[(x, y)] = v
        for y in range(qy0 - 1, qy1 + 1):
            fix[(qx0 - 1, y)] = (0.0, 0.0, 0.0)  # the left fades out anyway
        inner = [(x, y) for y in range(qy0, qy1) for x in range(qx0, qx1)]
        for xy in inner:
            fix[xy] = (0.0, 0.0, 0.0)
        for k in range(3):
            f = {xy: v[k] for xy, v in fix.items()}
            f[(qx1, qy0 - 1)] = f[(qx1, qy1)] = 0.0
            for _ in range(300):
                for x, y in inner:
                    t = f[(x - 1, y)] + f[(x + 1, y)] + f[(x, y - 1)] + f[(x, y + 1)]
                    f[(x, y)] += 1.9 * (t / 4 - f[(x, y)])
            for xy in inner:
                v = list(q[xy]); v[k] += f[xy]; q[xy] = tuple(v)
        for (x, y), a in acc.items():
            if not (qx0 <= x < qx1 and qy0 <= y < qy1):
                continue
            t = max(0.0, min(1.0, (x - qx0) / FEATHER))  # fade into the smooth fill
            v = q[(x, y)]
            smooth[y][x] = tuple(smooth[y][x][k] * (1 - t) + v[k] * t for k in range(3))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pc", type=Path, default=ROOT / "extracted/images-pc")
    ap.add_argument("--out", type=Path, default=ROOT / "extracted/menus-clean")
    a = ap.parse_args()
    net = load_png(a.pc / "SHELLDB3/NETMAIN_PCX.png")
    glass = load_png(a.pc / "SHELLDB2/GLASS2_TIM.png")
    h, w = len(net), len(net[0])
    cov = [[False] * w for _ in range(h)]
    for x0, y0, x1, y1 in COVERED:
        for y in range(y0, y1):
            for x in range(x0, x1):
                cov[y][x] = True

    # NETMAIN ~ scale * GLASS2 + offset, per channel (least squares)
    fit = []
    for k in range(3):
        n = sx = sy = sxx = sxy = 0
        for x0, y0, x1, y1 in FIT:
            for y in range(y0, y1):
                for x in range(x0, x1):
                    g, m = glass[y][x][k], net[y][x][k]
                    n += 1; sx += g; sy += m; sxx += g * g; sxy += g * m
        scale = (n * sxy - sx * sy) / (n * sxx - sx * sx)
        fit.append((scale, (sy - scale * sx) / n))
    print("color map:", ", ".join(f"{s:.3f}x{o:+.1f}" for s, o in fit))
    mapped = [[tuple(glass[y][x][k] * fit[k][0] + fit[k][1] for k in range(3)) for x in range(w)] for y in range(h)]

    # the difference, smoothed over uncovered pixels only
    diff = [[tuple(net[y][x][k] - mapped[y][x][k] for k in range(3)) for x in range(w)] for y in range(h)]
    smooth = [[None] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if cov[y][x]:
                continue
            acc, n = [0.0, 0.0, 0.0], 0
            for yy in range(max(0, y - SMOOTH), min(h, y + SMOOTH + 1)):
                for xx in range(max(0, x - SMOOTH), min(w, x + SMOOTH + 1)):
                    if not cov[yy][xx]:
                        d = diff[yy][xx]
                        acc[0] += d[0]; acc[1] += d[1]; acc[2] += d[2]; n += 1
            smooth[y][x] = (acc[0] / n, acc[1] / n, acc[2] / n)

    smooth_out = [row[:] for row in smooth]  # the smoothed difference, uncovered pixels
    out = [row[:] for row in net]
    marked = [row[:] for row in net]
    for y in range(h):
        for x in range(w):
            if not cov[y][x]:
                continue
            acc, wsum = [0.0, 0.0, 0.0], 0.0
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                xx, yy, d = x, y, 0
                while 0 <= xx < w and 0 <= yy < h and cov[yy][xx]:
                    xx += dx; yy += dy; d += 1
                if 0 <= xx < w and 0 <= yy < h:
                    wt = 1.0 / (d * d)
                    s = smooth[yy][xx]
                    acc[0] += s[0] * wt; acc[1] += s[1] * wt; acc[2] += s[2] * wt
                    wsum += wt
            smooth[y][x] = (acc[0] / wsum, acc[1] / wsum, acc[2] / wsum)  # a first guess
    # the harmonic fill, per channel, over the covered pixels
    cells = [(y, x) for y in range(h) for x in range(w) if cov[y][x]]
    for k in range(3):
        f = [[(smooth[y][x][k] if smooth[y][x] else 0.0) for x in range(w)] for y in range(h)]
        for _ in range(SWEEPS):
            for y, x in cells:
                n = t = 0
                if x > 0: t += f[y][x - 1]; n += 1
                if x < w - 1: t += f[y][x + 1]; n += 1
                if y > 0: t += f[y - 1][x]; n += 1
                if y < h - 1: t += f[y + 1][x]; n += 1
                f[y][x] += 1.9 * (t / n - f[y][x])
        for y, x in cells:
            s = list(smooth[y][x]); s[k] = f[y][x]; smooth[y][x] = tuple(s)
    # the texture, mirrored in across the nearest edges
    import math
    for y, x in cells:
        acc, wsum, near = [0.0, 0.0, 0.0], 0.0, None
        for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            xx, yy, d = x, y, 0
            while 0 <= xx < w and 0 <= yy < h and cov[yy][xx]:
                xx += dx; yy += dy; d += 1
            sx, sy = xx + dx * (d - 1), yy + dy * (d - 1)  # as far outside as inside
            if not (0 <= sx < w and 0 <= sy < h) or cov[sy][sx]:
                continue
            near = d if near is None else min(near, d)
            wt = 1.0 / (d * d)
            for k in range(3):
                acc[k] += (diff[sy][sx][k] - smooth_out[sy][sx][k]) * wt
            wsum += wt
        if wsum:
            fade = math.exp(-(near - 1) / DETAIL_FADE)
            smooth[y][x] = tuple(smooth[y][x][k] + acc[k] / wsum * fade for k in range(3))
    quilt(net, diff, smooth, cov, w, h)
    for y, x in cells:
        p = tuple(max(0, min(255, round(mapped[y][x][k] + smooth[y][x][k]))) for k in range(3))
        out[y][x] = p
        marked[y][x] = (min(255, p[0] + 60), p[1], min(255, p[2] + 60))
    a.out.mkdir(parents=True, exist_ok=True)
    save_png(a.out / "NETMAIN_clean.png", out)
    save_png(a.out / "NETMAIN_clean_marked.png", marked)
    print(f"{a.out}: NETMAIN_clean.png, NETMAIN_clean_marked.png")


if __name__ == "__main__":
    main()
