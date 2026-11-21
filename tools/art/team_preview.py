"""Team colors, a look before the game: each car's body texture (image 0 of
TMS\\<code>.TMS) as it is, the paint entries marked, and recolored for the
four teams, as src/vplus/fixes.c recolors them (team_paint.h's entries).

    py tools/art/team_preview.py OUT_DIR [--disc CUE] [--car CODE]

Writes OUT_DIR/team_<code>.png (5 pictures side by side, 3 times as large)
and, with --dump, the palette index under each texel as a text grid. The
output is the game's art: keep it out of the repository.
"""
import argparse
import colorsys
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
from cdimage import CdImage  # noqa: E402
import tim  # noqa: E402

CODES = ["HH", "OT", "WH", "MG", "PV", "TH", "SP", "RK", "IR", "AX", "FL", "HS", "ST", "MN"]
HUES = [0, 225, 120, 55]  # red, blue, green, yellow (fixes.c team_hue)
FLOORS = {0: 0.68, 55: 0.72}  # fixes.c team_recolor: red not pink, yellow not pale


def masks():
    """team_paint.h's bit masks, by car, and each car's saturation floor and brightness gain."""
    text = (ROOT / "src/vplus/team_paint.h").read_text()
    rows = re.findall(r"\{(0x[0-9A-F, x]+)\}, /\*", text)
    tune = [re.search(name + r"\[15\] = \{([0-9, ]+)\}", text) for name in ("team_smin", "team_vgain")]
    smin = [int(v) / 100 for v in tune[0].group(1).split(",")] if tune[0] else [0.35] * 15
    gain = [int(v) / 100 for v in tune[1].group(1).split(",")] if tune[1] else [1.0] * 15
    out = []
    for r in rows[:14]:
        b = [int(v, 16) for v in r.split(",")]
        out.append({i for i in range(256) if b[i >> 3] >> (i & 7) & 1})
    acc = []  # team_accent: per car two (mask, hue per team)
    m = re.search(r"team_accent\[15\]\[2\]\[32\] = \{(.*?)\n\};", text, re.S)
    mh = re.search(r"team_accent_hue\[15\]\[2\]\[4\] = \{(.*?)\n\};", text, re.S)
    if m and mh:
        masks_rows = re.findall(r"\{(0x[0-9A-F, x]+|0)\}", m.group(1))
        hue_rows = re.findall(r"\{(-?\d+), (-?\d+), (-?\d+), (-?\d+)\}", mh.group(1))
        for k in range(14):
            pair = []
            for a in range(2):
                r = masks_rows[2 * k + a]
                b = [int(v, 16) for v in r.split(",")] if r != "0" else [0] * 32
                pair.append(({i for i in range(256) if b[i >> 3] >> (i & 7) & 1}, [int(v) for v in hue_rows[2 * k + a]]))
            acc.append(pair)
    return out, smin, gain, acc


def recolor(e, hue, smin=0.35, vgain=1.0):
    if not e & 0x7FFF:
        return e
    smin = max(smin, FLOORS.get(hue, 0))
    r, g, b = (e & 31) / 31, (e >> 5 & 31) / 31, (e >> 10 & 31) / 31
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    s = smin + (1 - smin) * s
    v = min(1.0, v * vgain * (1.3 if hue == 55 else 1.0))
    r, g, b = colorsys.hsv_to_rgb(hue / 360, s, v)
    o = round(r * 31) | round(g * 31) << 5 | round(b * 31) << 10
    return (o or 0x0421) | (e & 0x8000)


def rgb(c):
    if not c & 0x7FFF:
        return (0, 0, 0)
    return ((c & 31) * 255 // 31, (c >> 5 & 31) * 255 // 31, (c >> 10 & 31) * 255 // 31)


def png(path, w, h, px):
    raw = b"".join(b"\0" + bytes(v for p in px[y * w:(y + 1) * w] for v in p) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("--disc", type=Path, default=ROOT / "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue")
    ap.add_argument("--car", default="")
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    m, smin, gain, acc = masks()
    cd = CdImage(str(a.disc))
    for k, code in enumerate(CODES):
        if a.car and a.car != code:
            continue
        data = cd.read(f"/TMS/{code}.TMS")
        tims = list(tim.scan(data))
        body = tims[0][1] if isinstance(tims[0], tuple) else tims[0]
        pal = body.palettes[0]
        w, h = body.width, body.height
        S = 3
        pics = [[rgb(pal[p]) for p in body.pixels],
                [(255, 0, 255) if p in m[k] else rgb(pal[p]) for p in body.pixels]]
        for team, hue in enumerate(HUES):
            def one(i, c):
                if i in m[k]:
                    return recolor(c, hue, smin[k], gain[k])
                for amask, ahues in (acc[k] if acc else []):
                    if i in amask and ahues[team] >= 0:
                        return recolor(c, ahues[team], smin[k], gain[k])
                return c
            tp = [one(i, c) for i, c in enumerate(pal)]
            pics.append([rgb(tp[p]) for p in body.pixels])
        W = (w * S + 4) * len(pics)
        px = [(20, 20, 20)] * (W * h * S)
        for n, pic in enumerate(pics):
            for y in range(h * S):
                for x in range(w * S):
                    px[y * W + n * (w * S + 4) + x] = pic[(y // S) * w + x // S]
        png(a.out / f"team_{code}.png", W, h * S, px)
        if a.dump:
            with open(a.out / f"team_{code}.txt", "w") as f:
                for y in range(h):
                    f.write(" ".join(f"{body.pixels[y * w + x]:3d}" for x in range(w)) + "\n")
        print(code, w, h, "images", len(tims))


if __name__ == "__main__":
    main()
