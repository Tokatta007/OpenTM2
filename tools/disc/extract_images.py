"""Extract the 2D images from Twisted Metal 2 to PNG.

    python tools/disc/extract_images.py ["<disc>.cue"] [--out extracted/images] [--all]
    python tools/disc/extract_images.py --pc "references/TM2-PC-Version/Game Files" [--out extracted/images-pc]

PlayStation disc (default): stand-alone .TIM files and the TIMs packed in
.TMS files. By default only the front end is extracted (SCREEN, SHELLDB1,
SHELLDB2, BIOS car info screens), the gameplay images every level pack
shares (HUD, weapons, pickups, explosions) and, per level, the sprites only
that level has (pedestrians, statues, trees, landmarks: LEVELS/<pack>/).
--all instead writes every folder whole (car and level textures).

PC version (--pc, an installed game folder): the same for .TIM and the .TPC
packs (TIMs at twice the PlayStation resolution for many images), plus the
PC-only .PCX screens in SHELLDB3.

Writes one PNG per image (and per extra palette, up to --max-palettes) plus
index.html, a gallery grouped by source file. The output is game content:
keep it out of git (extracted/ is ignored) and do not publish it.
"""
import argparse
import html
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdimage import CdImage  # noqa: E402
import pcx  # noqa: E402
import tim  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DISC = ROOT / "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue"
FRONT_END = ("/SCREEN/", "/SHELLDB1/", "/SHELLDB2/", "/SHELLDB3/", "/BIOS/")
PACKS = (".TIM", ".TMS", ".TPC")


def disc_files(disc):
    """(path, read function) for every data file on a PlayStation disc."""
    cd = CdImage(disc)
    for f in cd.files:
        if cd.is_data(f) and not f.path.startswith("/SCUS"):
            yield f.path, (lambda f=f: cd.read(f))


def folder_files(folder):
    """(path, read function) for every file under an installed game folder."""
    folder = Path(folder)
    for p in sorted(folder.rglob("*")):
        if p.is_file():
            yield "/" + p.relative_to(folder).as_posix().upper(), p.read_bytes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", nargs="?", default=str(DEFAULT_DISC), help=".cue or track-1 .bin")
    ap.add_argument("--pc", metavar="FOLDER", help="read an installed PC version folder instead of a disc")
    ap.add_argument("--out", help="output folder (default extracted/images, or extracted/images-pc with --pc)")
    ap.add_argument("--all", action="store_true", help="also car and level textures")
    ap.add_argument("--max-palettes", type=int, default=8,
                    help="extra PNGs for images with several palettes (default 8)")
    args = ap.parse_args()

    source = args.pc or args.disc
    files = list(folder_files(args.pc) if args.pc else disc_files(args.disc))
    out = Path(args.out or ("extracted/images-pc" if args.pc else "extracted/images"))
    out.mkdir(parents=True, exist_ok=True)
    groups = []  # (source path, [(png name, image, palette, offset)])
    total = 0
    for path, read in files:
        ext = os.path.splitext(path)[1]
        if ext not in PACKS + (".PCX",):
            continue
        if not args.all and not path.startswith(FRONT_END):
            continue
        data = read()
        if ext == ".PCX":
            try:
                found = [(0, pcx.parse(data))]
            except ValueError as e:
                print(f"{path}: {e}")
                continue
        else:
            found = list(tim.scan(data, rgb=path.upper().endswith(".TPC")))
        if not found:
            continue
        stem = Path(path).name.replace(".", "_")
        entries = []
        digits = max(2, len(str(len(found) - 1)))
        for n, (off, t) in enumerate(found):
            base = stem if len(found) == 1 else f"{stem}_{n:0{digits}d}"
            npal = min(len(t.palettes), args.max_palettes) if t.bpp in (4, 8) and t.palettes else 1
            for pal in range(npal):
                name = base if pal == 0 else f"{base}_pal{pal}"
                rel = Path(path.strip("/")).parent / f"{name}.png"
                (out / rel).parent.mkdir(parents=True, exist_ok=True)
                tim.write_png(out / rel, t.width, t.height, t.rgba(pal))
                entries.append((rel.as_posix(), t, pal, off))
                total += 1
        groups.append((path, entries))
        print(f"{path}: {len(found)} image(s)")
    if not args.all:
        levels = [(p, r) for p, r in files
                  if (p.startswith("/TMS/") and p.endswith(".TMS")) or (p.startswith("/LEVELDB/") and p.endswith(".TPC"))]
        entries, own = extract_shared(levels, out)
        if entries:
            groups.append(("Gameplay images shared by the level packs", entries))
            total += len(entries)
            print(f"gameplay shared: {len(entries)} image(s)")
        for path, level_entries in own:
            groups.append((f"{path}: sprites only this level has (pedestrians, scenery)", level_entries))
            total += len(level_entries)
        if own:
            print(f"level sprites: {sum(len(e) for _, e in own)} image(s)")
    write_gallery(out / "index.html", groups, source)
    print(f"{total} PNG files and index.html in {out}")


SPRITE_MAX = 128  # level images up to this size with transparency count as sprites (not wall textures)


def is_sprite(t):
    if t.width > SPRITE_MAX or t.height > SPRITE_MAX:
        return False
    rgba = t.rgba(0)
    clear = sum(1 for i in range(3, len(rgba), 4) if rgba[i] == 0)
    return clear * 5 > t.width * t.height  # over a fifth transparent


def extract_shared(levels, out, min_share=0.75):
    """The HUD, weapon, pickup and explosion images, and each level's own sprites.

    Every level pack carries the same set of about 140 gameplay images, with
    level-specific palettes. They are found by comparing pixel indices across
    the packs (present in at least `min_share` of them) and written once,
    from the first pack that has them, so colors may differ in-game. The rest
    of a pack is the level's own: its sprites (images with transparency:
    pedestrians, statues, trees) go to LEVELS/<pack>/, its wall and ground
    textures are left out (--all has them).
    Returns (shared entries, [(pack path, its own entries)]).
    """
    packs = [(p, list(tim.scan(read(), rgb=p.upper().endswith(".TPC")))) for p, read in levels]
    packs = [(p, found) for p, found in packs if len(found) > 50]  # level packs, not car packs
    if len(packs) < 2:
        return [], []
    key = lambda t: (t.width, t.height, t.bpp, tuple(t.pixels))  # noqa: E731
    counts = {}
    for _, found in packs:
        for k in {key(t) for _, t in found}:
            counts[k] = counts.get(k, 0) + 1
    need = max(2, int(len(packs) * min_share))
    folder = out / "GAMEPLAY_SHARED"
    folder.mkdir(parents=True, exist_ok=True)
    entries = []
    for n, (off, t) in enumerate(packs[0][1]):
        if counts.get(key(t), 0) < need:
            continue
        rel = f"GAMEPLAY_SHARED/{n:03d}.png"
        tim.write_png(out / rel, t.width, t.height, t.rgba(0))
        entries.append((rel, t, 0, off))
    own = []
    for path, found in packs:
        stem = Path(path).stem
        seen, level_entries = set(), []
        for n, (off, t) in enumerate(found):
            k = key(t)
            if counts.get(k, 0) >= need or k in seen or not is_sprite(t):
                continue
            seen.add(k)
            rel = f"LEVELS/{stem}/{n:03d}.png"
            (out / rel).parent.mkdir(parents=True, exist_ok=True)
            tim.write_png(out / rel, t.width, t.height, t.rgba(0))
            level_entries.append((rel, t, 0, off))
        if level_entries:
            own.append((path, level_entries))
    return entries, own


def write_gallery(path, groups, source):
    parts = [
        "<!doctype html><meta charset=utf-8><title>TM2 images</title>",
        "<style>body{font:14px system-ui,sans-serif;background:#1b1b1f;color:#ddd;margin:16px}"
        "h2{font-size:15px;margin:28px 0 8px;color:#fc6}figure{display:inline-block;margin:6px;"
        "vertical-align:top;background:#2a2a30 url(\"data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg'"
        " width='16' height='16'><rect width='8' height='8' fill='%23333'/><rect x='8' y='8' width='8'"
        " height='8' fill='%23333'/></svg>\");padding:6px;border-radius:4px}"
        "img{image-rendering:pixelated;display:block;max-width:min(640px,90vw);height:auto}"
        "figcaption{font-size:11px;color:#aaa;margin-top:4px;max-width:320px}</style>",
        f"<h1>Twisted Metal 2 images</h1><p>From {html.escape(str(source))}. "
        "Images up to 320 pixels wide are shown at 2&times;, pixelated. Checkerboard = transparent.</p>",
    ]
    stamp = int(time.time())  # in the links, so a browser loads a new extraction rather than its cached copy
    for src, entries in groups:
        parts.append(f"<h2>{html.escape(src)}</h2>")
        for rel, t, pal, off in entries:
            cap = f"{rel.rsplit('/', 1)[-1]}<br>{t.width}&times;{t.height}, {t.bpp} bpp"
            if t.bpp in (4, 8) and t.palettes:
                cap += f", palette {pal + 1}/{len(t.palettes)}"
            if t.vram != (0, 0) or off:
                cap += f"<br>VRAM {t.vram[0]},{t.vram[1]}; offset 0x{off:X}"
            scale = 2 if t.width <= 320 else 1
            parts.append(f"<figure><img src='{html.escape(rel)}?v={stamp}' width={t.width * scale} "
                         f"height={t.height * scale} loading=lazy><figcaption>{cap}</figcaption></figure>")
    path.write_text("\n".join(parts), encoding="utf-8")


if __name__ == "__main__":
    main()
