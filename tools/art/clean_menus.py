"""Clean menu backgrounds: remove baked-in text without inventing pixels.

    py tools/art/clean_menus.py [--src extracted/images-pc] [--out extracted/menus-clean]

Many TM2 menu screens are drawn on the same shattered-glass picture: the one-
and two-player menus, Options, Audio, History and the eight level previews
(SHOWOPS1-8). Each has its own artwork on it (a title, a picture, a button
legend), in different places. Where at least MIN_VOTES of them have exactly
the same pixel, that pixel is bare glass; together they give a clean "plate".
A screen is cleaned by replacing its text areas (the button legend at the
bottom, the title at the top left) with the plate. Pixels no group of
screens agrees on are left marked in magenta in the report (never guessed).

Writes <out>/<screen>.png (cleaned, unmarked pixels kept from the original)
and <out>/index.html, a before/after report.
"""
import argparse
import struct
import zlib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIN_VOTES = 3     # screens that must agree on a pixel
MIN_VOTES_DARK = 2  # enough for a dark pixel (glass, not text)

# screens drawn on the shared glass, and which of their areas hold baked-in text
NO_LEGEND = [f"SHELLDB1/SHOWOPS{i}_TIM" for i in range(1, 9)]   # level previews: no button legend
GLASS = ["SHELLDB1/SINGLE_TIM", "SHELLDB1/MULTI_TIM", "SHELLDB2/OPTS_TIM", "SHELLDB2/AUDIO_TIM",
         "SHELLDB2/HISTORY_TIM"] + NO_LEGEND
FAMILY = {n: "glass" for n in GLASS}
CLEAN = {  # screen: text areas to replace (x0, y0, x1, y1) in 640x480; "legend" is found
    "SHELLDB1/SINGLE_TIM": ["legend"],
    "SHELLDB1/MULTI_TIM": ["legend"],
    "SHELLDB2/OPTS_TIM": ["legend", (0, 0, 330, 140)],
    "SHELLDB2/AUDIO_TIM": ["legend", (0, 0, 330, 140)],
    "SHELLDB2/HISTORY_TIM": ["legend", (0, 0, 330, 140)],
}
# screens with the same glass, saved with slightly different colors (within a few
# levels, so not voted with the others): their text areas are filled from the clean
# plate, shifted to their colors (measured on the glass around each area)
PATCH = {
    "SHELLDB1/LEVELSEL_TIM": ["legend", (0, 20, 640, 75)],
    "SHELLDB3/LOAD_BG_PCX": [(0, 0, 330, 40)],
    "SHELLDB3/SAVE_BG_PCX": [(0, 0, 330, 40)],
    "SHELLDB3/LOAD_PCX": [(0, 0, 330, 40)],
    "SHELLDB3/SAVE_PCX": [(0, 0, 330, 40)],
    "SHELLDB3/AUDIO_BG_PCX": ["legend", (0, 50, 210, 115)],
}
# screens no other screen shares a picture with: layering cannot remove their text
SINGLETONS = {
    "SHELLDB1/CARSEL_TIM": "Button legend over the road; its box is solid black in every version, so "
                           "no picture of the road under it exists. Options: our own legend bar over it, "
                           "or a manual edit continuing the asphalt.",
    "SHELLDB2/GLASS2_TIM": "Already clean: plain glass (a different picture from the menus').",
    "SHELLDB1/TITLE_TIM": "Only the copyright line, which stays.",
    "SHELLDB1/WORLD_TIM": "No baked-in text.",
}


def load_png(path):
    d = path.read_bytes()
    pos, idat, plte = 8, b"", None
    while pos < len(d):
        n = struct.unpack(">I", d[pos:pos + 4])[0]
        t, c = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"PLTE":
            plte = c
        elif t == b"IDAT":
            idat += c
    raw = zlib.decompress(idat)
    bpp = {2: 3, 6: 4, 3: 1, 0: 1}[ct]
    stride, out, prev, i = w * bpp, [], bytearray(w * bpp), 0
    for _ in range(h):
        f = raw[i]
        row = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                row[x] = (row[x] + a) & 255
            elif f == 2:
                row[x] = (row[x] + b) & 255
            elif f == 3:
                row[x] = (row[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = row
        if ct == 3:
            out.append([tuple(plte[3 * row[k]:3 * row[k] + 3]) for k in range(w)])
        else:
            out.append([tuple(row[k * bpp:k * bpp + 3]) for k in range(w)])
    return out


def save_png(path, px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in px)
    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def legend_box(px):
    """The button legend: bright text and border rows near the bottom."""
    w, h = len(px[0]), len(px)
    bright = lambda p: sum(p) > 600
    rows = [y for y in range(h * 5 // 6, h) if sum(1 for x in range(w) if bright(px[y][x])) > 30]
    if not rows:
        return None
    y0, y1 = rows[0] - 3, min(h, rows[-1] + 4)
    cols = [x for x in range(w) if any(bright(px[y][x]) for y in range(y0, y1))]
    return max(0, min(cols) - 3), y0, min(w, max(cols) + 4), y1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=ROOT / "extracted/images-pc")
    ap.add_argument("--out", type=Path, default=ROOT / "extracted/menus-clean")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    images = {n: load_png(args.src / f"{n}.png") for n in GLASS}
    h, w = 480, 640

    def make_plate(sources):
        plate, votes = [[None] * w for _ in range(h)], [[0] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                v, n = Counter(images[s][y][x] for s in sources).most_common(1)[0]
                plate[y][x], votes[y][x] = v, n
        return plate, votes

    # the legend is the same on every menu, so only screens without one may vote there
    plates = {"legend": make_plate(NO_LEGEND), "glass": make_plate(GLASS)}
    report = []
    for name, areas in CLEAN.items():
        src = images[name]
        out = [row[:] for row in src]
        marked = [row[:] for row in src]
        boxes = [(legend_box(src), "legend") if a == "legend" else (a, FAMILY[name]) for a in areas]
        replaced = flagged = 0
        for box, kind in boxes:
            if not box:
                continue
            plate, votes = plates[kind]
            x0, y0, x1, y1 = box
            for y in range(y0, y1):
                for x in range(x0, x1):
                    ok = votes[y][x] >= MIN_VOTES or (votes[y][x] >= MIN_VOTES_DARK and sum(plate[y][x]) < 300)
                    if ok:
                        if out[y][x] != plate[y][x]:
                            replaced += 1
                        out[y][x] = marked[y][x] = plate[y][x]
                    elif out[y][x] != plate[y][x]:
                        flagged += 1
                        marked[y][x] = (255, 0, 255)
        stem = name.replace("/", "_")
        save_png(args.out / f"{stem}.png", out)
        save_png(args.out / f"{stem}_marked.png", marked)
        report.append((name, stem, [b for b, _ in boxes], replaced, flagged))
        print(f"{name}: {replaced} pixels replaced from the plate, {flagged} without {MIN_VOTES} agreeing screens")

    # the glass plate itself: a clean full background for new pages (at the
    # bottom only the screens without a legend vote)
    plate, votes = plates["glass"]
    lplate, lvotes = plates["legend"]
    x0, y0, x1, y1 = legend_box(images["SHELLDB1/SINGLE_TIM"])
    for y in range(y0, y1):
        for x in range(x0, x1):
            plate[y][x], votes[y][x] = lplate[y][x], lvotes[y][x]
    for kind in ("glass",):
        plate, votes = plates[kind]
        marked = [[plate[y][x] if votes[y][x] >= MIN_VOTES else (255, 0, 255) for x in range(w)] for y in range(h)]
        flagged = sum(1 for y in range(h) for x in range(w) if votes[y][x] < MIN_VOTES)
        save_png(args.out / f"plate_{kind}.png", plate)
        save_png(args.out / f"plate_{kind}_marked.png", marked)
        print(f"plate {kind}: {flagged} pixels without {MIN_VOTES} agreeing screens")
        report.append((f"plate: {kind}", f"plate_{kind}", [], 0, flagged))

    # same glass, other colors: fill from the plate with a color offset
    lum = lambda p: (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
    for name, areas in PATCH.items():
        src = load_png(args.src / f"{name}.png")
        out, marked = [row[:] for row in src], [row[:] for row in src]
        replaced = flagged = 0
        boxes = [legend_box(src) if a == "legend" else a for a in areas]
        for box in filter(None, boxes):
            x0, y0, x1, y1 = box
            ring = [(x, y) for y in range(max(0, y0 - 8), min(h, y1 + 8)) for x in range(max(0, x0 - 8), min(w, x1 + 8))
                    if not (x0 <= x < x1 and y0 <= y < y1) and votes[y][x] >= MIN_VOTES
                    and abs(lum(src[y][x]) - lum(plate[y][x])) <= 24]
            off = [round(sum(src[y][x][k] - plate[y][x][k] for x, y in ring) / max(1, len(ring))) for k in range(3)]
            for y in range(y0, y1):
                for x in range(x0, x1):
                    if votes[y][x] >= MIN_VOTES or (votes[y][x] >= MIN_VOTES_DARK and sum(plate[y][x]) < 300):
                        out[y][x] = marked[y][x] = tuple(max(0, min(255, plate[y][x][k] + off[k])) for k in range(3))
                        replaced += 1
                    else:
                        flagged += 1
                        marked[y][x] = (255, 0, 255)
        stem = name.replace("/", "_")
        save_png(args.out / f"{stem}.png", out)
        save_png(args.out / f"{stem}_marked.png", marked)
        report.insert(len(CLEAN), (name, stem, boxes, replaced, flagged))
        print(f"{name}: {replaced} pixels filled from the plate (color offset), {flagged} without a clean source")

    def cell(n, s, r, f):
        bad = f"<span class=bad>{f} px flagged</span>" if f else "0 px flagged"
        if n.startswith("plate"):
            return f"<tr><td><b>{n}</b><br>{bad}</td><td></td><td><img src='{s}_marked.png'></td><td><img src='{s}.png'></td></tr>"
        return (f"<tr><td><b>{n}</b><br>{r} px replaced<br>{bad}</td><td><img src='../images-pc/{n}.png'></td>"
                f"<td><img src='{s}_marked.png'></td><td><img src='{s}.png'></td></tr>")

    rows = "\n".join(cell(n, s, r, f) for n, s, _, r, f in report)
    rows += "\n".join(f"<tr><td><b>{n}</b><br><span class=bad>not cleaned</span></td>"
                      f"<td><img src='../images-pc/{n}.png'></td><td colspan=2>{note}</td></tr>"
                      for n, note in SINGLETONS.items())
    (args.out / "index.html").write_text(
        "<!doctype html><meta charset=utf-8><title>Menu backgrounds</title><style>"
        "body{background:#111;color:#ddd;font:14px sans-serif}img{width:480px}td{padding:6px;vertical-align:top}"
        ".bad{color:#f6c}</style><h2>Menu backgrounds: original, marked (magenta = no agreement), clean</h2>"
        f"<table><tr><th></th><th>Original (PC)</th><th>Marked</th><th>Clean</th></tr>{rows}</table>",
        encoding="utf-8")


if __name__ == "__main__":
    main()
