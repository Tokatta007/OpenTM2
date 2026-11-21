"""New text screens in the style of the co-op ending screen (COWIN), from its own letters.

    py tools/art/duel_text.py --replace-from 5 --text "Be the last one standing|to earn your reward..." --out DUEL.png

SHELLDB2/COWIN.TIM (the PC version's 640x480 copy) is the screen shown after a won co-op
tournament: italic Flexure lettering with a drop shadow on the game's emblem, colored
red at the center of the screen and through orange to yellow towards the edges. This
cuts its letters out (text shape, shading and shadow), and sets new lines with them:
- background: the emblem without text, voted from the credits screens that share it
  (CREDIT01-07, at least 3 agreeing), shifted to COWIN's colors;
- color: a radial gradient fitted to COWIN's letters (distance from the screen center,
  vertical distance scaled), so a letter takes the color of its new place and keeps
  its own shading;
- spacing: letter gaps, word spaces, line pitch and baseline as measured in COWIN.
Only letters that occur in COWIN can be used (it says which are missing).
"""
import argparse
import math
from collections import Counter
from pathlib import Path

from clean_menus import load_png, save_png

ROOT = Path(__file__).resolve().parents[2]
W, H = 640, 480
LINES = ["DARK TOOTH is dead.", "You are the victors in the", "11th TWISTED METAL™ contest.",
         "But, there can only be", "ONE winner of the ultimate prize.", "Try again in single player mode",
         "to find out what your", "Twisted reward will be..."]
USABLE = {0: None, 1: None, 2: None, 3: None, 4: None, 5: 22, 6: None}  # line: characters that split cleanly


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def warm(p):
    r, g, b = p
    return r > 120 and r > b + 60 and r >= g


def components(mask, y0, y1):
    seen, out = set(), []
    for y in range(y0, y1):
        for x in range(W):
            if mask[y][x] and (x, y) not in seen:
                stack, px = [(x, y)], []
                seen.add((x, y))
                while stack:
                    cx, cy = stack.pop()
                    px.append((cx, cy))
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            nx, ny = cx + dx, cy + dy
                            if 0 <= nx < W and y0 <= ny < y1 and mask[ny][nx] and (nx, ny) not in seen:
                                seen.add((nx, ny))
                                stack.append((nx, ny))
                if len(px) >= 6:
                    out.append(px)
    return sorted(out, key=lambda p: min(q[0] for q in p))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--text", required=True, help="lines separated by |")
    ap.add_argument("--replace-from", type=int, default=0,
                    help="first of COWIN's 8 lines to replace (they and the rest are cleared; the new "
                         "lines go on their baselines). 0: replace all, centered on the screen")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--src", type=Path, default=ROOT / "extracted/images-pc/SHELLDB2")
    args = ap.parse_args()

    co = load_png(args.src / "COWIN_TIM.png")
    credits = [load_png(args.src / f"CREDIT0{i}_TIM.png") for i in (1, 2, 4, 5, 6, 7)]
    # the emblem is gray; the credits' text is near-white with soft edges: leave the text and
    # a 2-pixel margin around it out of the vote
    def text_mask(c):
        m = [[False] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                if lum(c[y][x]) >= 175:
                    for yy in range(max(0, y - 2), min(H, y + 3)):
                        for xx in range(max(0, x - 2), min(W, x + 3)):
                            m[yy][xx] = True
        return m
    masks = [text_mask(c) for c in credits]

    def vote(y, x):
        vals = [c[y][x] for c, m in zip(credits, masks) if not m[y][x]] or [c[y][x] for c in credits]
        v, n = Counter(vals).most_common(1)[0]
        return v if n >= 2 else sorted(vals, key=lum)[len(vals) // 2]
    plate = [[vote(y, x) for x in range(W)] for y in range(H)]
    text = [[warm(co[y][x]) for x in range(W)] for y in range(H)]

    # COWIN was saved with slightly different colors than the credits: measure the shift
    diffs = [[co[y][x][k] - plate[y][x][k] for k in range(3)] for y in range(0, H, 2) for x in range(0, W, 2)
             if not text[y][x] and abs(lum(co[y][x]) - lum(plate[y][x])) < 16]
    shift = [sorted(d[k] for d in diffs)[len(diffs) // 2] for k in range(3)]

    # text lines (row bands) and their baselines
    rows = [sum(r) for r in text]
    bands, y = [], 0
    while y < H:
        if rows[y]:
            y0 = y
            while y < H and rows[y]:
                y += 1
            if y - y0 > 8:
                bands.append((y0, y))
        y += 1
    baselines = [Counter(max(q[1] for q in c) for c in components(text, y0, y1)).most_common(1)[0][0]
                 for y0, y1 in bands]

    # COWIN as it is, except around the letters of the lines being replaced
    first = bands[args.replace_from][0] - 3 if args.replace_from < len(bands) else H
    near = [[False] * W for _ in range(H)]
    for y in range(first, H):
        for x in range(W):
            if text[y][x]:  # its letters, with their edge dots (left/up) and shadows (right/down)
                for yy in range(max(0, y - 3), min(H, y + 10)):
                    for xx in range(max(0, x - 3), min(W, x + 10)):
                        near[yy][xx] = True
    # COWIN's tint drifts against the credits' across the screen: measure the color shift
    # locally, on 16-pixel cells, from clean background (COWIN away from any letters)
    anytext = [[False] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if text[y][x]:
                for yy in range(max(0, y - 4), min(H, y + 11)):
                    for xx in range(max(0, x - 4), min(W, x + 11)):
                        anytext[yy][xx] = True
    cell = {}
    for cy in range(0, H, 16):
        for cx in range(0, W, 16):
            for r in (24, 40, 64, 96):
                d = [[co[y][x][k] - plate[y][x][k] for k in range(3)]
                     for y in range(max(0, cy - r), min(H, cy + 16 + r), 2)
                     for x in range(max(0, cx - r), min(W, cx + 16 + r), 2) if not anytext[y][x]]
                if len(d) >= 40:
                    cell[cy // 16, cx // 16] = [sorted(v[k] for v in d)[len(d) // 2] for k in range(3)]
                    break
    def local(y, x):
        return cell.get((y // 16, x // 16), shift)
    def clean(y, x):  # the emblem without any text, in COWIN's colors
        return tuple(max(0, min(255, plate[y][x][k] + local(y, x)[k])) for k in range(3))
    bg = [[clean(y, x) if near[y][x] else co[y][x]
           for x in range(W)] for y in range(H)]

    # color model: radial from the screen center, vertical distance scaled by s
    samples = [(x - 320, y - 240, co[y][x]) for y in range(H) for x in range(W) if text[y][x]]
    def fit(s):
        bins = {}
        for dx, dy, p in samples:
            bins.setdefault(int(math.hypot(dx, dy * s)) // 4, []).append(p[1])
        return sum(sum((g - sum(v) / len(v)) ** 2 for g in v) for v in bins.values()) / len(samples)
    s = min((i / 20 for i in range(6, 33)), key=fit)
    bins = {}
    for dx, dy, p in samples:
        bins.setdefault(int(math.hypot(dx, dy * s)) // 4, []).append(p)
    model = {b: tuple(sum(p[k] for p in v) / len(v) for k in range(3)) for b, v in bins.items() if len(v) >= 8}
    keys = sorted(model)

    def color(x, y):
        d = math.hypot(x - 320, (y - 240) * s) / 4
        lo = max([k for k in keys if k <= d], default=keys[0])
        hi = min([k for k in keys if k >= d], default=keys[-1])
        t = 0 if hi == lo else (d - lo) / (hi - lo)
        return tuple(model[lo][k] * (1 - t) + model[hi][k] * t for k in range(3))

    # cut the letters
    glyphs, gaps, spaces = {}, [], []

    def profile(c, x_ref, base):  # per row (relative to the baseline): leftmost and rightmost pixel
        prof = {}
        for qx, qy in c:
            lo, hi = prof.get(qy - base, (qx - x_ref, qx - x_ref))
            prof[qy - base] = (min(lo, qx - x_ref), max(hi, qx - x_ref))
        return prof

    def distance(pa, xa, pb, xb):  # closest horizontal distance between two outlines
        rows = set(pa) & set(pb)
        return min((xb + pb[r][0]) - (xa + pa[r][1]) for r in rows) if rows else None
    for li, (y0, y1) in enumerate(bands):
        if li not in USABLE:
            continue
        chars = LINES[li].replace(" ", "")
        comps = components(text, y0, y1)
        n = USABLE[li] or len(chars)
        if USABLE[li] is None and len(comps) != len(chars):
            continue
        bottoms = Counter(max(q[1] for q in c) for c in comps)
        baseline = bottoms.most_common(1)[0][0]
        prev, word_break = None, set()
        k = 0
        for i, ch in enumerate(LINES[li]):
            if ch == " ":
                word_break.add(k)
            else:
                k += 1
        for i, (ch, c) in enumerate(zip(chars[:n], comps[:n])):
            left = min(q[0] for q in c)
            prof = profile(c, left, baseline)
            if prev is not None:
                d = distance(prev[0], prev[1], prof, left)
                if d is not None:
                    (spaces if i in word_break else gaps).append(d)
            prev = (prof, left)
            if ch in glyphs:
                continue
            cs = set(c)
            xs, ys = [q[0] for q in c], [q[1] for q in c]
            px = []
            for yy in range(min(ys) - 1, max(ys) + 9):
                for xx in range(min(xs) - 1, max(xs) + 9):
                    if not (0 <= xx < W and 0 <= yy < H):
                        continue
                    if (xx, yy) in cs:
                        m = color(xx, yy)
                        px.append((xx - left, yy - baseline, "t", tuple(co[yy][xx][k] / max(1, m[k]) for k in range(3))))
                    elif (not text[yy][xx] and lum(co[yy][xx]) < lum(clean(yy, xx)) - 20
                          and any((xx - sx, yy - sy) in cs for sx in range(0, 7) for sy in range(0, 7))):
                        # a shadow pixel of this letter: just below-right of its own pixels
                        px.append((xx - left, yy - baseline, "s",
                                   tuple(co[yy][xx][k] / max(1, clean(yy, xx)[k]) for k in range(3))))
            glyphs[ch] = (px, prof)
    gap = sorted(gaps)[len(gaps) // 2]
    space = sorted(spaces)[len(spaces) // 2]
    pitch = sorted(b[0] - a[0] for a, b in zip(bands, bands[1:]))[len(bands) // 2 - 1]

    lines = args.text.split("|")
    missing = sorted({ch for line in lines for ch in line if ch != " " and ch not in glyphs})
    if missing:
        raise SystemExit(f"letters not in COWIN: {' '.join(missing)}")
    out = [row[:] for row in bg]
    top = 240 - pitch * len(lines) // 2 + pitch // 2
    for li, line in enumerate(lines):
        # place each letter so its outline is the measured gap (or a word space) from the last one
        layers, prev, want = [], None, gap
        for ch in line:
            if ch == " ":
                want = space
                continue
            px, prof = glyphs[ch]
            if prev is None:
                x = 0
            else:
                d = distance(prev[0], prev[1], prof, 0)
                x = want - d if d is not None else prev[1] + max(r[1] for r in prev[0].values()) + want
            layers.append((x, px))
            prev, want = (prof, x), gap
        right = max(x + max(r[1] for r in glyphs[ch][1].values()) for (x, _), ch in zip(layers, line.replace(" ", "")))
        shift_x = 320 - right // 2
        layers = [(x + shift_x, px) for x, px in layers]
        if args.replace_from:  # on the baselines of the replaced lines
            i = args.replace_from + li
            base = baselines[i] if i < len(baselines) else baselines[-1] + (i - len(baselines) + 1) * pitch
        else:
            base = top + li * pitch
        for kind in ("s", "t"):  # all shadows first, then the letters over them
            for gx, px in layers:
                for dx, dy, k, f in px:
                    if k != kind:
                        continue
                    xx, yy = gx + dx, base + dy
                    if 0 <= xx < W and 0 <= yy < H:
                        ref = color(xx, yy) if k == "t" else bg[yy][xx]
                        out[yy][xx] = tuple(max(0, min(255, round(ref[c] * f[c]))) for c in range(3))
    save_png(args.out, out)
    print(f"{args.out}: gradient scale {s:.2f}, gap {gap}, space {space}, pitch {pitch}, color shift {shift}")


if __name__ == "__main__":
    main()
