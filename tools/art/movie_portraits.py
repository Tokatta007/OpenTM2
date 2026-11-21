"""Profile portraits from the movies: stills cropped to 1:2.

    py tools/art/movie_portraits.py --out build/port/ui

Reads the picks in tools/art/movie_portraits.json (each: id, movie, frame,
x, who, and optionally gain: a brightness multiplier), decodes those frames from the disc's STR movies with ffmpeg (its
psxstr demuxer reads the raw 2352-byte sectors), crops 120x240 (the movie's
full height, 1:2 as the portraits) from x, and writes:

  movie_portraits.bmp  the crops side by side, in the list's order
  movie_portraits.txt  one line per crop: id, who, movie

who is the person in the crop: a driver (hammerhead ... minion), calypso or
other. The movie says when it is earned (the intro is free; an ending with
that driver's ending). ui_assets.py runs this when ffmpeg is found (PATH,
FFMPEG, or C:/ffmpeg/bin). The output is the game's art: keep it out of the
repository.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
sys.path.insert(0, str(ROOT / "tools" / "art"))
from cdimage import CdImage  # noqa: E402

SPEC = ROOT / "tools/art/movie_portraits.json"
W, H = 120, 240
SECTOR = 2352


def find_ffmpeg():
    for c in (os.environ.get("FFMPEG"), shutil.which("ffmpeg"), "C:/ffmpeg/bin/ffmpeg.exe"):
        if c and Path(c).exists():
            return c
    return None


def raw_str(cd, name, path):
    """A movie's raw sectors, as ffmpeg's psxstr reads them. Version-1
    frame headers (two of the short movies) are marked version 2, which
    ffmpeg decodes the same way."""
    f = next(f for f in cd.files if f.path.upper() == f"/MOVIES/{name}.STR")
    n = -(-f.size // 2048)
    with open(cd.bin_path, "rb") as b, open(path, "wb") as o:
        b.seek(f.lba * SECTOR)
        data = bytearray(b.read(n * SECTOR))
        for s in range(n):
            p = s * SECTOR + 24  # the user data of a Mode 2 sector
            # STR header: 0x0160, 0x8001, chunk, chunks, frame ... (32 bytes),
            # then the frame's own header: run length, 0x3800, scale, version
            if data[p + 2:p + 4] == b"\x01\x80" and data[p + 4:p + 6] == b"\x00\x00" and data[p + 38] == 1:
                data[p + 38] = 2
        o.write(data)


def load_bmp(path):
    d = path.read_bytes()
    off = int.from_bytes(d[10:14], "little")
    w = int.from_bytes(d[18:22], "little", signed=True)
    h = int.from_bytes(d[22:26], "little", signed=True)
    bpp = int.from_bytes(d[28:30], "little") // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        r = d[off + y * stride:off + y * stride + w * bpp]
        rows.append([(r[x * bpp + 2], r[x * bpp + 1], r[x * bpp]) for x in range(w)])
    return rows[::-1] if h > 0 else rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--disc", type=Path, default=ROOT / "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue")
    a = ap.parse_args()
    return 0 if build(a.out, a.disc) else 1


def build(out, disc):
    """Writes the two files to out; returns how many portraits (0: no ffmpeg)."""
    from ui_assets import save_bmp
    ff = find_ffmpeg()
    if not ff:
        print("movie_portraits: ffmpeg not found (PATH, FFMPEG or C:/ffmpeg/bin): skipped")
        return 0
    spec_text = SPEC.read_text(encoding="utf-8")
    picks = json.loads(spec_text)
    # the picks' hash heads the index: the same picks are not decoded again
    extra = "".join(str((ROOT / p["image"]).stat().st_mtime) for p in picks if p.get("image") and (ROOT / p["image"]).exists())
    stamp = "# " + hashlib.sha256((spec_text + extra + Path(__file__).read_text(encoding="utf-8")).encode()).hexdigest()
    index = out / "movie_portraits.txt"
    if (out / "movie_portraits.bmp").exists() and index.exists() and \
            index.read_text(encoding="utf-8").splitlines()[:1] == [stamp]:
        print(f"{out}: movie_portraits.bmp up to date ({len(picks)} portraits)")
        return len(picks)
    cd = CdImage(str(disc))
    crops = {}
    with tempfile.TemporaryDirectory() as tmp:
        for p in picks:  # pictures that are not movie stills: an image file, fitted on black
            if p.get("image"):
                from ui_assets import credits_portrait
                big = credits_portrait(ROOT / p["image"])
                if big is None:
                    print(f"movie_portraits: {p['image']} missing: {p['id']} left black")
                    crops[p["id"]] = [[(0, 0, 0)] * W for _ in range(H)]
                else:  # 240x480 -> 120x240, each pixel the average of four
                    crops[p["id"]] = [[tuple((big[2 * y][2 * x][k] + big[2 * y][2 * x + 1][k] + big[2 * y + 1][2 * x][k] +
                                             big[2 * y + 1][2 * x + 1][k]) // 4 for k in range(3)) for x in range(W)]
                                      for y in range(H)]
        for movie in sorted({p["movie"] for p in picks if not p.get("image")}):
            src = Path(tmp) / f"{movie}.str"
            raw_str(cd, movie, src)
            frames = sorted({p["frame"] for p in picks if p["movie"] == movie})
            sel = "+".join(f"eq(n\\,{n})" for n in frames)
            subprocess.run([ff, "-v", "fatal", "-f", "psxstr", "-i", str(src), "-an", "-vf", f"select='{sel}'",
                            "-fps_mode", "passthrough", str(Path(tmp) / f"{movie}_%03d.bmp")], check=True)
            for k, n in enumerate(frames):
                img = load_bmp(Path(tmp) / f"{movie}_{k + 1:03d}.bmp")
                for p in picks:
                    if p["movie"] == movie and p["frame"] == n:
                        x = max(0, min(len(img[0]) - W, p["x"]))
                        g = p.get("gain", 1.0)  # brighter, for the darkest shots
                        crops[p["id"]] = [[tuple(min(255, int(c * g + 0.5)) for c in px) for px in row[x:x + W]]
                                          for row in img[:H]]
    strip = [[px for p in picks for px in crops[p["id"]][y]] for y in range(H)]
    out.mkdir(parents=True, exist_ok=True)
    save_bmp(out / "movie_portraits.bmp", strip)
    index.write_text(stamp + "\n" + "".join(f"{p['id']} {p['who']} {p['movie']}\n" for p in picks), encoding="utf-8")
    print(f"{out}: movie_portraits.bmp ({len(picks)} portraits), movie_portraits.txt")
    return len(picks)


if __name__ == "__main__":
    sys.exit(main())
