"""Car select without its button legend: continue the road under the legend box.

    py tools/art/carsel_road.py

The legend box of SHELLDB1/CARSEL.TIM (car select: flames over a road) is solid black
inside in every version, so no picture of the road under it exists. This fills it from
the screen's own pixels, nothing painted from scratch:
- asphalt (texture synthesis, "image quilting"): the box is covered with overlapping
  14x14 patches of real road from rows at the same distance (within about 30 rows above
  and below the box: further up the grain is finer), anywhere across the road, never
  touching the center line, optionally mirrored. Each patch is picked, among a few hundred
  random ones, as the best match for the pixels already around it, including the original
  road at the box's edges, after matching its brightness and grain contrast to the road
  there (blended from the road just above and just below the box, column by column).
  Each patch replaces the overlap, with a 1-pixel seam blended (averaging more would
  soften the grain);
- center line: the dash running into the box is extended along its slant (its edges fitted
  over the rows above the box) to a length that fits the dashes above (each about 1.5
  times the one before: 8, 13, 20, then about 30 rows), colored from its own clean
  interior (asphalt specks left out), with soft edges.
Writes extracted/menus-clean/SHELLDB1_CARSEL_TIM.png and _marked.png (edited area outlined).
"""
import random
from pathlib import Path

from clean_menus import legend_box, load_png, save_png

ROOT = Path(__file__).resolve().parents[2]
DASH_END = 436     # last row of the dash entering the box
PATCH, STEP = 14, 10
CANDIDATES = 400
SEED = 7


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000


def yellow(p):
    r, g, b = p
    return r > 120 and g > 70 and b < 90 and r > b + 60


def main():
    random.seed(SEED)
    src = load_png(ROOT / "extracted/images-pc/SHELLDB1/CARSEL_TIM.png")
    h, w = len(src), len(src[0])
    x0, y0, x1, y1 = legend_box(src)
    out = [row[:] for row in src]
    known = [[not (x0 <= x < x1 and y0 <= y < y1) for x in range(w)] for y in range(h)]
    weight = [[1.0 if known[y][x] else 0.0 for x in range(w)] for y in range(h)]
    acc = [[[0.0, 0.0, 0.0] for _ in range(w)] for _ in range(h)]

    # the road's brightness and grain contrast inside the box: per column, blended from the
    # road just above to just below it (the rows next to the legend's border left out)
    def band(ys, x):
        vals = [lum(src[y][xx]) for y in ys for xx in range(max(0, x - 8), min(w, x + 9))]
        m = sum(vals) / len(vals)
        return m, (sum((v - m) ** 2 for v in vals) / len(vals)) ** 0.5
    above = [band(range(y0 - 9, y0 - 2), x) for x in range(w)]
    below = [band(range(y1 + 1, min(h, y1 + 8)), x) for x in range(w)]

    def target(y, x):
        t = min(1.0, max(0.0, (y - y0 + 0.5) / (y1 - y0)))
        return tuple(above[x][i] * (1 - t) + below[x][i] * t for i in range(2))

    # source patches: real road at the same distance (rows within ~30 of the box: the road's
    # grain gets finer further up), anywhere across, never touching the center line
    isy = [[1 if yellow(src[y][x]) else 0 for x in range(w)] for y in range(h)]
    def has_yellow(sx, sy):
        return any(isy[y][x] for y in range(sy, sy + PATCH) for x in range(sx, sx + PATCH))
    def stats(sx, sy):
        ls = [lum(src[y][x]) for y in range(sy, sy + PATCH) for x in range(sx, sx + PATCH)]
        m = sum(ls) / len(ls)
        return m, (sum((v - m) ** 2 for v in ls) / len(ls)) ** 0.5
    sources = [(sx, sy) + stats(sx, sy) for sy in list(range(y0 - 32, y0 - PATCH - 1)) + list(range(y1 + 1, h - PATCH + 1))
               for sx in range(0, w - PATCH + 1, 2) if not has_yellow(sx, sy)]

    def take(sx, sy, flip, x, y, bx, by):
        dx = x - bx
        return src[sy + y - by][sx + (PATCH - 1 - dx if flip else dx)]

    for by in range(y0 - 4, y1, STEP):
        for bx in range(x0 - 4, x1, STEP):
            cells = [(x, y) for y in range(by, by + PATCH) for x in range(bx, bx + PATCH) if 0 <= x < w and 0 <= y < h]
            if not any(not known[y][x] and weight[y][x] < 1 for x, y in cells):
                continue
            ref = [(x, y) for x, y in cells if known[y][x] or weight[y][x] > 0]
            tm = sum(target(y, x)[0] for x, y in cells) / len(cells)
            ts = sum(target(y, x)[1] for x, y in cells) / len(cells)
            best = None
            # only pieces of road with about the same grain (contrast) and lighting as here
            pool = [s for s in sources if 0.8 * ts <= s[3] <= 1.25 * ts and 0.75 * tm <= s[2] <= 1.33 * tm]
            if len(pool) < 50:
                pool = sorted(sources, key=lambda s: abs(s[3] - ts) / max(1, ts) + abs(s[2] - tm) / max(1, tm))[:200]
            for sx, sy, _, _ in random.sample(pool, min(CANDIDATES, len(pool))):
                flip = random.random() < 0.5
                q_all = [take(sx, sy, flip, x, y, bx, by) for x, y in cells]
                ls = [lum(q) for q in q_all]
                pm = sum(ls) / len(ls)
                ps = max(1.0, (sum((v - pm) ** 2 for v in ls) / len(ls)) ** 0.5)
                cm = [sum(q[k] for q in q_all) / len(q_all) for k in range(3)]
                gain = min(1.6, max(0.6, ts / ps))
                lift = tm / max(1.0, pm)
                def adj(q, cm=cm, lift=lift, gain=gain):  # brightness to the road there, grain to its contrast
                    return [cm[k] * lift + (q[k] - cm[k]) * gain for k in range(3)]
                err = 0.0
                for x, y in ref:
                    q = adj(take(sx, sy, flip, x, y, bx, by))
                    have = out[y][x] if known[y][x] else [acc[y][x][k] / weight[y][x] for k in range(3)]
                    err += sum((q[k] - have[k]) ** 2 for k in range(3))
                if best is None or err < best[0]:
                    best = (err, sx, sy, flip, adj)
            _, sx, sy, flip, adj = best
            for x, y in cells:
                if known[y][x]:
                    continue
                # the new patch replaces what was there, except a 1-pixel seam blended
                # half and half (averaging more would soften the asphalt's grain)
                e = min(x - bx, bx + PATCH - 1 - x, y - by, by + PATCH - 1 - y) + 1
                q = adj(take(sx, sy, flip, x, y, bx, by))
                if e > 1 or weight[y][x] == 0:
                    acc[y][x] = q
                else:
                    have = [acc[y][x][k] / weight[y][x] for k in range(3)]
                    acc[y][x] = [(have[k] + q[k]) / 2 for k in range(3)]
                weight[y][x] = 1.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            out[y][x] = tuple(max(0, min(255, round(acc[y][x][k] / max(1e-6, weight[y][x])))) for k in range(3))

    # the dash: fit its edges over the rows above the box, extend, color from its interior
    rows = []
    for y in range(y0 - 22, y0):
        xs = [x for x in range(280, 360) if yellow(src[y][x])]
        if xs:
            rows.append((y, min(xs), max(xs)))
    def fit(pts):
        n = len(pts); my = sum(p[0] for p in pts) / n; mv = sum(p[1] for p in pts) / n
        k = sum((p[0] - my) * (p[1] - mv) for p in pts) / sum((p[0] - my) ** 2 for p in pts)
        return lambda y: mv + k * (y - my)
    rows = rows[:-2]                        # the last two rows are darkened by the legend's border
    fl, fr = fit([(y, l) for y, l, _ in rows]), fit([(y, r) for y, _, r in rows])
    last_y, last_l, last_r = rows[-1]       # anchor the fit to the real edge at the box
    left = lambda y: fl(y) + (last_l - fl(last_y))
    right = lambda y: fr(y) + (last_r - fr(last_y))
    interior = [src[y][x] for y, l, r in rows for x in range(l + 1, r) if yellow(src[y][x])]
    start = last_y + 1
    for y in range(start, DASH_END + 1):
        l, r = left(y), right(y)
        # texture: the dash rows mirrored upward from where the continuation starts
        sy, sl, sr = rows[max(0, len(rows) - 1 - (y - start))]
        for x in range(int(l) - 1, int(r) + 2):
            cover = min(1.0, max(0.0, x + 1 - l), max(0.0, r + 1 - x))  # soft left/right edge
            if y == DASH_END:
                cover *= 0.5                                         # soft end
            if cover <= 0:
                continue
            # texture from the same relative place in a dash row above (yellow pixels only)
            u = (x - l) / max(1.0, r - l)
            q = src[sy][min(sr, max(sl, round(sl + u * (sr - sl))))]
            if not yellow(q):
                q = interior[(x * 31 + y * 17) % len(interior)]
            base = out[y][x] if y >= y0 else (src[y - 3][x] if not yellow(src[y - 3][x]) else out[y][x])
            out[y][x] = tuple(round(q[k] * cover + base[k] * (1 - cover)) for k in range(3))

    marked = [row[:] for row in out]
    for x in range(x0 - 1, x1 + 1):
        marked[y0 - 1][x] = marked[y1][x] = (0, 255, 255)
    for y in range(y0, y1):
        marked[y][x0 - 1] = marked[y][x1] = (0, 255, 255)
    dst = ROOT / "extracted/menus-clean"
    save_png(dst / "SHELLDB1_CARSEL_TIM.png", out)
    save_png(dst / "SHELLDB1_CARSEL_TIM_marked.png", marked)
    print(f"car select: rows {y0}-{y1 - 1}, columns {x0}-{x1 - 1} synthesized; dash to row {DASH_END}")


if __name__ == "__main__":
    main()
