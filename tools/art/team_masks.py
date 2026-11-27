"""src/vplus/team_paint.h: each car's paint entries for team colors.

    py tools/art/team_masks.py [--disc CUE]

The entries start from the decomp's lists (references/TM2-Decomp/docs/
TEAM_COLORS.md 4.3) and are tuned per car below, from the body texture
(image 0 of TMS\\<code>.TMS) read from the disc: entries added by color in a
part of the texture, or taken away when they are used in a part that keeps
its own colors. Also per car: how strongly a recolored entry takes the
team's color (its saturation's floor) and a brightness gain. The header
holds indices and numbers only. tools/art/team_preview.py shows the result.

A car can instead have a painted mask, tools/art/team_masks/<code>.pbm (128x128, 1 = takes the
team's color): its paint is the entries those pixels use, and every other pixel keeps its colors
(its own copies of the entries it shares with the paint: the game copies them pixel by pixel).
The masks are made by painting over the texture:

    py tools/art/team_masks.py --export DIR      DIR/paint_<code>.png: the texture, 6x, its paint magenta (every car;
                                                 DIR/texture_<code>.png the texture as it is)
    (paint magenta 255,0,255 where the team's color goes, any other color where the car's stays)
    py tools/art/team_masks.py --import DIR      DIR/paint_<code>.png -> tools/art/team_masks/<code>.pbm
"""
import argparse
import colorsys
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
from cdimage import CdImage  # noqa: E402
import tim  # noqa: E402

ORDER = ["HH", "OT", "WH", "MG", "PV", "TH", "SP", "RK", "IR", "AX", "FL", "HS", "ST", "MN"]
NAMES = ["Hammerhead", "Outlaw 2", "Warthog", "Mr. Grimm", "Grasshopper", "Thumper", "Spectre", "Roadkill",
         "Twister", "Axel", "Mr. Slam", "Shadow", "Sweet Tooth", "Minion"]

RED = (340, 20, 0.45, 0.30)  # hue window (degrees), minimum saturation and value: a strong red
# per car: entries added (hue window, min s, min v, in a texture rectangle x0, y0, x1, y1 or None),
# rectangles whose entries are taken away (and kept anyway when in a 'keep' rectangle), the
# saturation floor (0.35: the decomp's) and the brightness gain
TUNE = {
    # the hubs share their entries with his skin: they keep their colors
    "AX": {"remove": [(0, 0, 128, 52)],                     # his skin and the rest of the top (arms, torso)
           "add": [((340, 20, 0.70, 0.55), (55, 0, 72, 30))]},  # then the shoulder cannons' bright red
    "HS": {"add": [(RED, None)]},                           # the red lights
    # the missiles' tips, the lights, and every color of the hull (its sides, top and underside); the
    # wheels, windows, gun, grille and lights keep theirs (split); a stronger color
    "WH": {"add": [(RED, None)], "paint_rects": [(64, 0, 128, 84), (0, 0, 32, 62), (32, 64, 100, 104), (0, 92, 100, 104)],
           "split": [(0, 100, 32, 128), (60, 100, 94, 128), (35, 86, 73, 99), (30, 82, 86, 89), (100, 84, 128, 128),
                     (0, 64, 32, 82)],
           "smin": 0.70, "vgain": 1.30},
    # the white panels: a solid team color, not a tint; the light bar, the head- and taillights and the
    # lenses keep theirs (split)
    "OT": {"smin": 0.85, "vgain": 0.80,
           "split": [(0, 0, 128, 24), (0, 57, 30, 70), (70, 57, 100, 70), (96, 24, 128, 48), (96, 72, 128, 104),
                     (0, 76, 25, 86), (70, 76, 95, 86), (12, 106, 38, 128), (86, 106, 112, 128)]},
    # every pink and red of the polka dots and the ice cream; the clown sign on the roof, his flaming
    # head and the roof lights keep theirs (split), the nose the team's
    "ST": {"add": [((330, 15, 0.20, 0.35), None)],
           "split": [(14, 3, 40, 24), (2, 8, 9, 15), (35, 8, 43, 15), (78, 94, 128, 128)],
           "split_except": [(89, 104, 97, 113)]},
    # Twister: with the red team her pink stripe goes blue, with the yellow team her yellow strip green
    # (accents: entries outside the paint, a hue for some teams: 0 red, 1 blue, 2 green, 3 yellow)
    "IR": {"accents": [((280, 340, 0.25, 0.20), None, {0: 225}), ((45, 70, 0.40, 0.40), None, {3: 120})]},
}


def lists():
    doc = (ROOT / "references/TM2-Decomp/docs/TEAM_COLORS.md").read_text(encoding="utf-8")
    sec = doc[doc.index("### 4.3"):doc.index("## 5.")]
    rows = {}
    for line in sec.splitlines():
        m = re.match(r"\| (\w\w)(, car select palette \(768,36\))? \| ([0-9,\-]+) \|", line)
        if not m:
            continue
        idx = set()
        for part in m.group(3).split(","):
            a, _, b = part.partition("-")
            idx.update(range(int(a), int(b or a) + 1))
        rows[m.group(1) + ("_SEL" if m.group(2) else "")] = idx
    return rows


def hsv(c):
    return colorsys.rgb_to_hsv((c & 31) / 31, (c >> 5 & 31) / 31, (c >> 10 & 31) / 31)


def in_window(c, win):
    h0, h1, smin, vmin = win[:4]
    vmax = win[4] if len(win) > 4 else 1.0  # (optional: the brightest)
    h, s, v = hsv(c)
    h *= 360
    hue_ok = h0 <= h <= h1 if h0 <= h1 else (h >= h0 or h <= h1)
    return c & 0x7FFF and hue_ok and s >= smin and vmin <= v <= vmax


def used_in(body, rect):
    x0, y0, x1, y1 = rect
    return {body.pixels[y * body.width + x] for y in range(y0, min(y1, body.height)) for x in range(x0, min(x1, body.width))}


def in_rects(x, y, rects):
    return any(r[0] <= x < r[2] and r[1] <= y < r[3] for r in rects)


MASK_DIR = ROOT / "tools/art/team_masks"
EXPORT = ORDER  # (every car can have a painted mask: the community's, 2026-10-27; those without one keep their lists)


def mask_read(code):
    p = MASK_DIR / f"{code}.pbm"
    if not p.exists():
        return None
    words = [w for line in p.read_text().splitlines() if not line.startswith("#") for w in line.split()]
    assert words[0] == "P1"
    w, h = int(words[1]), int(words[2])
    bits = "".join(words[3:])
    return {(i % w, i // w) for i, c in enumerate(bits[:w * h]) if c == "1"}


def mask_write(code, cells, w=128, h=128):
    MASK_DIR.mkdir(parents=True, exist_ok=True)
    lines = ["P1", f"# {code}: 1 = the team's color (team_masks.py --import)", f"{w} {h}"]
    for y in range(h):
        lines.append("".join("1" if (x, y) in cells else "0" for x in range(w)))
    (MASK_DIR / f"{code}.pbm").write_text("\n".join(lines) + "\n")


def png_read(path):
    """An 8-bit PNG (gray, RGB, palette, gray+alpha, RGBA; not interlaced): (w, h, rows of (r, g, b))."""
    import struct
    import zlib
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, plte = 8, b"", []
    while pos < len(data):
        n, t = struct.unpack(">I4s", data[pos:pos + 8])
        d = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", d)
        elif t == b"PLTE":
            plte = [tuple(d[i:i + 3]) for i in range(0, len(d), 3)]
        elif t == b"IDAT":
            idat += d
        pos += 12 + n
    assert depth == 8 and not inter, "save the painting as an 8-bit PNG, not interlaced"
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw, stride, prev, rows = zlib.decompress(idat), w * bpp, bytearray(w * bpp), []
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        px = []
        for x in range(w):
            q = line[x * bpp:(x + 1) * bpp]
            px.append(plte[q[0]] if ctype == 3 else (q[0], q[0], q[0]) if ctype in (0, 4) else tuple(q[:3]))
        rows.append(px)
    return w, h, rows


def do_split(body, idx, t):
    """The parts that keep their colors (t['split'] less t['split_except']) get their own copies of the paint
    entries they share with the rest. Entries used only there leave the paint. The copies go to unused
    entries, and to entries freed by merging near-identical colors (both paint or both not, both not shared).
    idx is changed in place; returns the merges, copies and rects for the game."""
    rects, exc = t.get("split", []), t.get("split_except", [])
    w, h = body.width, body.height
    pix = list(body.pixels)
    pal = body.palettes[0]
    mask = t.get("mask")  # a painted mask: the pixels outside it keep their colors
    def kept(x, y):
        return (x, y) not in mask if mask is not None else in_rects(x, y, rects) and not in_rects(x, y, exc)
    def zones():
        inside, outside = set(), set()
        for y in range(h):
            for x in range(w):
                (inside if kept(x, y) else outside).add(pix[y * w + x])
        return inside, outside
    inside, outside = zones()
    idx -= inside - outside                      # theirs alone: not paint
    shared = sorted(inside & outside & idx)      # paint used there and elsewhere: copies
    used = set(pix)
    free = [i for i in range(256) if i not in used]
    merges = []
    def rgb5(c):
        return (c & 31, c >> 5 & 31, c >> 10 & 31)
    if len(free) < len(shared):                  # merge near-identical colors until there is room
        cand = sorted(used)                      # (two shared ones merged: one copy fewer as well)
        pairs = []
        for i, a in enumerate(cand):
            for b in cand[i + 1:]:
                if (a in idx) != (b in idx) or (pal[a] & 0x8000) != (pal[b] & 0x8000):
                    continue
                d = max(abs(p - q) for p, q in zip(rgb5(pal[a]), rgb5(pal[b])))
                if d <= 3:
                    pairs.append((d, (a in shared) + (b in shared) == 2 and -1 or 0, a, b))
        pairs.sort()
        gone = set()
        for d, _, a, b in pairs:
            if len(free) >= len(shared):
                break
            if a in gone or b in gone:
                continue
            merges.append((b, a))
            gone.add(b)
            free.append(b)
            for k in range(len(pix)):
                if pix[k] == b:
                    pix[k] = a
            idx.discard(b)
            if b in shared:  # its pixels are a's now: a is shared (if it was not)
                shared = [e for e in shared if e != b]
                if a not in shared:
                    shared.append(a)
    if len(free) < len(shared):
        print("  split: room for %d of %d copies" % (len(free), len(shared)))
        shared = shared[:len(free)]
    copies = list(zip(shared, free))
    rr = [tuple(r) + (0,) for r in rects] + [tuple(r) + (1,) for r in exc]
    keep = None
    if mask is not None:  # the game's per-pixel bits: the pixel takes the copies
        keep = bytearray(w * h // 8)
        for y in range(h):
            for x in range(w):
                if kept(x, y):
                    keep[(y * w + x) >> 3] |= 1 << ((y * w + x) & 7)
    return {"merge": merges, "copy": copies, "rects": rr[:8], "keep": keep}


def preview(out_dir, code, body, idx, split, t):
    """The texture as it is, its paint marked, and in the four teams' colors as the game makes them."""
    sys.path.insert(0, str(ROOT / "tools" / "art"))
    import team_preview as tp
    w, h = body.width, body.height
    pal = list(body.palettes[0])
    pix = list(body.pixels)
    if split:
        for b, a2 in split["merge"]:
            pix = [a2 if p == b else p for p in pix]
        rects = [r[:4] for r in split["rects"] if not r[4]]
        exc = [r[:4] for r in split["rects"] if r[4]]
        cp = dict(split["copy"])
        for f_from, f_to in split["copy"]:
            pal[f_to] = pal[f_from]
        keep = split.get("keep")
        for y in range(h):
            for x in range(w):
                k = y * w + x
                kept = keep[k >> 3] >> (k & 7) & 1 if keep else in_rects(x, y, rects) and not in_rects(x, y, exc)
                if pix[k] in cp and kept:
                    pix[k] = cp[pix[k]]
    smin, gain = t.get("smin", 0.35), t.get("vgain", 1.0)
    pics = [[tp.rgb(body.palettes[0][p]) for p in body.pixels], [(255, 0, 255) if p in idx else tp.rgb(pal[p]) for p in pix]]
    for hue in tp.HUES:
        tpal = [tp.recolor(c, hue, smin, gain) if i in idx else c for i, c in enumerate(pal)]
        pics.append([tp.rgb(tpal[p]) for p in pix])
    S = 3
    W = (w * S + 4) * len(pics)
    px = [(20, 20, 20)] * (W * h * S)
    for n, pic in enumerate(pics):
        for y in range(h * S):
            for x in range(w * S):
                px[y * W + n * (w * S + 4) + x] = pic[(y // S) * w + x // S]
    out_dir.mkdir(parents=True, exist_ok=True)
    tp.png(out_dir / f"team_{code}.png", W, h * S, px)
    return pics[1]  # the texture, its paint magenta


def mask_text(idx):
    b = [0] * 32
    for i in idx:
        b[i >> 3] |= 1 << (i & 7)
    return "{" + ", ".join("0x%02X" % x for x in b) + "}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", type=Path, default=ROOT / "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue")
    ap.add_argument("--preview", type=Path, help="also write DIR/team_<code>.png for the tuned cars (the game's art: not for the repository)")
    ap.add_argument("--export", type=Path, help="write DIR/paint_<code>.png to paint on (the game's art: not for the repository)")
    ap.add_argument("--import", dest="imp", type=Path, help="read DIR/paint_<code>.png into tools/art/team_masks/<code>.pbm")
    a = ap.parse_args()
    if a.imp:
        for code in EXPORT:
            p = a.imp / f"paint_{code}.png"
            if not p.exists():
                continue
            w, h, rows_ = png_read(p)
            tw, th = round(w / 6), round(h / 6)  # (the kit's 6x: the texture's own size, 128x128; Minion's 256x128)
            sx, sy = w / tw, h / th
            cells = set()
            for y in range(th):
                for x in range(tw):
                    votes = 0
                    for yy in range(int(y * sy), max(int(y * sy) + 1, int((y + 1) * sy))):
                        for xx in range(int(x * sx), max(int(x * sx) + 1, int((x + 1) * sx))):
                            r, g, b = rows_[yy][xx]
                            votes += 1 if (r > 200 and g < 90 and b > 200) else -1
                    if votes > 0:
                        cells.add((x, y))
            mask_write(code, cells, tw, th)
            print(f"{code}: {len(cells)} pixels take the team's color -> {MASK_DIR / (code + '.pbm')}")
    rows = lists()
    cd = CdImage(str(a.disc))
    out = ["/* The paint entries of each car's body CLUT (image 0 of TMS\\<code>.TMS), one bit per",
           " * entry, and how strongly they take the team's color: made by tools/art/team_masks.py",
           " * (decomp TEAM_COLORS.md 4.3, tuned per car; do not edit). */",
           "#ifndef TEAM_PAINT_H", "#define TEAM_PAINT_H", "#include <stdint.h>",
           "static const uint8_t team_paint[15][32] = {"]
    smins, gains, accents, splits = [], [], [], []
    for code, name in zip(ORDER, NAMES):
        idx = set(rows[code])
        t = dict(TUNE.get(code, {}))
        mask = mask_read(code)
        if mask is not None:  # painted: exactly the entries its pixels use, the rest kept (split pixel by pixel)
            body = next(tim.scan(cd.read(f"/TMS/{code}.TMS")))[1]
            idx = {body.pixels[y * body.width + x] for (x, y) in mask}
            idx = {i for i in idx if body.palettes[0][i] & 0x7FFF}
            pal = body.palettes[0]  # (the accents read it)
            t = {"mask": mask, "split": [], "smin": t.get("smin", 0.35), "vgain": t.get("vgain", 1.0),
                 "accents": t.get("accents", [])}  # (Twister's stripes: another hue for some teams, with a mask too)
        elif t or a.export:
            body = next(tim.scan(cd.read(f"/TMS/{code}.TMS")))[1]
            pal = body.palettes[0]
            keep = set()  # first what goes (a 'keep' part stays), then what is added
            for rect in t.get("keep", []):
                keep |= used_in(body, rect) & idx
            for rect in t.get("remove", []):
                idx -= used_in(body, rect) - keep
            if t.get("remove_own"):  # only the entries used in these parts and nowhere else
                inside = set()
                for rect in t["remove_own"]:
                    inside |= used_in(body, rect)
                outside = set(body.pixels) - inside
                for y in range(body.height):
                    for x in range(body.width):
                        if not any(r[0] <= x < r[2] and r[1] <= y < r[3] for r in t["remove_own"]):
                            outside.add(body.pixels[y * body.width + x])
                idx -= (inside - outside) - keep
            for rect in t.get("paint_rects", []):  # every color of these parts but the darkest
                idx |= {i for i in used_in(body, rect) if hsv(pal[i])[2] >= 0.08}
            for add in t.get("add", []):
                win, rect = add[0], add[1]
                where = used_in(body, rect) if rect else set(range(256))
                for ex in (add[2] if len(add) > 2 else []):  # not the entries these parts use (wheels, windows)
                    where -= used_in(body, ex)
                idx |= {i for i in where if in_window(pal[i], win)}
        split = do_split(body, idx, t) if t.get("split") or "mask" in t else None
        splits.append(split)
        if (a.preview and t) or a.export:  # (exported: every car, tuned or not)
            marked = preview(a.preview or a.export, code, body, idx, split, t)
            if a.export and code in EXPORT:  # to paint on: 6x
                sys.path.insert(0, str(ROOT / "tools" / "art"))
                import team_preview as tp
                S, w = 6, body.width
                big = [marked[(y // S) * w + x // S] for y in range(body.height * S) for x in range(w * S)]
                tp.png(a.export / f"paint_{code}.png", w * S, body.height * S, big)
                plain = [tp.rgb(body.palettes[0][p]) for p in body.pixels]  # (and the texture as it is, to compare)
                big = [plain[(y // S) * w + x // S] for y in range(body.height * S) for x in range(w * S)]
                tp.png(a.export / f"texture_{code}.png", w * S, body.height * S, big)
        out.append(f"    {mask_text(idx)}, /* {name}: {len(idx)} */")
        acc = []
        for win, rect, hues in t.get("accents", []):
            where = used_in(body, rect) if rect else set(range(256))
            acc.append(({i for i in where if in_window(pal[i], win)} - idx, hues))
        accents.append(acc)
        smins.append(t.get("smin", 0.35))
        gains.append(t.get("vgain", 1.0))
    out.append("    {0}, /* Dark Tooth: not analysed */")
    out.append("};")
    out.append("/* accents: other entries recolored for some teams (Twister's stripes), a hue per team or -1 */")
    out.append("static const uint8_t team_accent[15][2][32] = {")
    for k, acc in enumerate(accents + [[]]):
        cells = [mask_text(acc[i][0]) if i < len(acc) else "{0}" for i in range(2)]
        out.append("    {%s, %s}," % (cells[0], cells[1]))
    out.append("};")
    out.append("/* the split (team_masks.py do_split): copies of shared paint colors for the parts that keep theirs.")
    out.append(" * merge: pixels from -> to everywhere (near-identical colors, freeing entries); copy: in the rects (not")
    out.append(" * the except ones), pixels from -> to, the CLUT's 'to' a copy of 'from' */")
    keeps = [sp["keep"] for sp in splits if sp and sp.get("keep")]
    out.append("/* painted masks (tools/art/team_masks): a bit per pixel of the texture (128x128; Minion's 256x128), 1 = takes")
    out.append(" * the copies */")
    out.append("static const uint8_t team_keep[%d][4096] = {" % max(1, len(keeps)))
    for k in keeps or [bytes(4096)]:
        out.append("    {%s}," % ", ".join(str(v) for v in k))
    out.append("};")
    out.append("typedef struct { uint8_t n_merge, n_copy, n_rect; uint8_t merge[128][2], copy[128][2], rect[8][5]; int8_t keep; } TeamSplit;")
    out.append("static const TeamSplit team_split[15] = {")
    keep_no = 0
    for sp in splits + [None]:
        if not sp:
            out.append("    {0},")
            continue
        mg = ", ".join("{%d, %d}" % p for p in sp["merge"]) or "{0}"
        cp = ", ".join("{%d, %d}" % p for p in sp["copy"]) or "{0}"
        rc = ", ".join("{%d, %d, %d, %d, %d}" % r for r in sp["rects"]) or "{0}"
        kn = -1
        if sp.get("keep"):
            kn, keep_no = keep_no, keep_no + 1
        out.append("    {%d, %d, %d, {%s}, {%s}, {%s}, %d}," % (len(sp["merge"]), len(sp["copy"]), len(sp["rects"]), mg, cp, rc, kn))
    out.append("};")
    out.append("static const int16_t team_accent_hue[15][2][4] = {")
    for k, acc in enumerate(accents + [[]]):
        cells = []
        for i in range(2):
            hues = acc[i][1] if i < len(acc) else {}
            cells.append("{%s}" % ", ".join(str(hues.get(t, -1)) for t in range(4)))
        out.append("    {%s}," % ", ".join(cells))
    out.append("};")
    out.append("static const uint8_t team_paint_minion_select[32] = %s; /* Minion's car select palette (768,36) */"
               % mask_text(rows["MN_SEL"]))
    out.append("/* the recolor: saturation = floor + (1 - floor) x the entry's; brightness x gain (in hundredths) */")
    out.append("static const uint8_t team_smin[15] = {%s, 35};" % ", ".join(str(round(v * 100)) for v in smins))
    out.append("static const uint8_t team_vgain[15] = {%s, 100};" % ", ".join(str(round(v * 100)) for v in gains))
    out.append("#endif")
    with open(ROOT / "src/vplus/team_paint.h", "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("team_paint.h written")


if __name__ == "__main__":
    main()
