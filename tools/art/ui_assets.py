"""Pictures for the port's own menu pages (src/ui), made from the game's art.

    py tools/art/ui_assets.py --out build/port/ui

Writes BMPs (24-bit) next to the executable, in ui/:
  options.bmp  the Options screen without its title and legend
               (extracted/menus-clean/SHELLDB2_OPTS_TIM.png, clean_menus.py)
               options.bmp and audio.bmp are taken to GLASS2's levels (see
               glass_levels), so the pages and their extensions match
  glass.bmp    the shattered glass with nothing on it: the PC version's GLASS2
               (extracted/images-pc, tools/disc/extract_images.py --pc)
  single.bmp   the PS1 1 Player menu's picture (the "1"), for the 1 Player and
               Tournament pages (extracted/menus-clean/SHELLDB1_SINGLE_TIM.png)
  audio.bmp    the PS1 Audio screen without its title and legend (speakers and
               flame in the middle; extracted/menus-clean/SHELLDB2_AUDIO_TIM.png)
  title.bmp    the PC version's title picture (the logo; the menu is drawn over it)
  hair.bmp     Sweet Tooth's flaming hair on the title: the PC version's 19 frames
               (TITLE.TPC images 1-19, 128x128) side by side, magenta where
               transparent; the title cycles them (one per game frame, 30 a second)
  portraits.bmp  profile pictures: each driver's two car-select portraits
               (64x128, CARSEL.TPC), side by side in car-select order
               (Hammerhead 1, Hammerhead 2, Outlaw 2 1 ... Minion 2)
  portraits_hq.bmp  each driver's third portrait: the art of the driver's
               info screen (BIOS/<xx>INFO.TIM), 240x480 from its right side,
               in car-select order
  movie_portraits.bmp/.txt  profile portraits from the movies (movie_portraits.py;
               needs ffmpeg, skipped without it)
  gamefont.bmp/.txt  the game's in-battle font sheet (UAFONT1.TIM, CPFONT.TIM below it; 4 times as
               large: Scale2x then doubled; magenta where transparent) and
               its glyph tables (in the sheet's own 1x pixels), for the pause menu
  carinfo_NN.bmp  each driver's CAR INFO picture (the PC's), by car-select index
  carinfo_vp_NN.bmp  the same with Vanilla Plus's ratings, for the cars whose differ
               (carinfo_stats.py: balls added or taken away, nothing else)
  carsel.bmp, carsel_titles.bmp, emblem1.bmp  car select's clean picture, its
               titles sheet (CARSEL.TPC 1) and the 1 player emblem (CARSEL.TPC 24)
  credits_portrait.bmp  Sweet Tooth from the advertising (references/art/
               sweet_tooth_promo_hq.png) on black, 240x480: the Movies Credits
  network.bmp  the Network background: the PC version's network screen (NETMAIN)
               with only its glass, flame and cable
               (extracted/menus-clean/NETMAIN_clean.png, net_background.py)
  bar.bmp      the PS1 highlight bar (copper), without text
  lineup_N.bmp  the opponent cards' screen of arena N (1-8; the PC's SHOWOPS, 640x480)
  duel.bmp     the co-op duel's screen (extracted/menus-clean/DUEL_TEXT.png, duel_text.py), 640x480
  hud_hud.bmp, hud_nohud.bmp, hud_list.bmp  the Customize HUD preview: one battle frame
               with the game's HUD, without it, with the full weapon list (1280x960)
  arena_N.bmp  arena N's level-select picture (the PC's LEVELSEL.TPC, 512x256; 0 Random: the
               game's "no level" card, SC-NONE, on the bare brick frame; 9-11 the secret arenas: their cards' scenes in the same brick frame)
  arena_card_N.bmp  arena N's level-select card (the PC's SC-*, 344x260, with its name), N 0-11
  icons.bmp    the HUD's weapon icons, the health and turbo pickups (64x64 each, magenta where clear)
  plates.bmp   each driver's opponent card (SCREEN/<code>PLATE.TIM, 128x64), in car-select order

The bar (see clean_bar) is the PS1's, at its resolution: the 1997 PC
version's are the same copper at twice it, but every one of them has
letters over its middle.

Needs the clean menus (clean_menus.py, net_background.py), the PC images and the PS1 disc image. Standard library only. The
output is the game's art: keep it out of the repository.
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "disc"))
sys.path.insert(0, str(ROOT / "tools" / "art"))
from cdimage import CdImage  # noqa: E402
import tim  # noqa: E402
from clean_menus import load_png  # noqa: E402
import carinfo_stats  # noqa: E402

# The PS1 options labels (OPTIONS.TMS image 4): three highlighted labels,
# AUDIO, HISTORY and SCENE DROPOUT, 21 rows each from row 1. A bar is a dark
# strip with copper bands along its top and bottom (rows 1-2 and 18-19 of
# the bar, between dark lines); the game cuts each bar to its label, ending
# a few pixels after it. The bands are never under letters.
BAR_SHEET = 4
BAR_TOP = 43       # SCENE DROPOUT: the longest bar, the sheet's full width
BAR_H = 21
INSIDE = range(4, 17)


def clean_bar(cd):
    """SCENE DROPOUT's bar with the label taken out: the bands are kept,
    the inside rows become the bar's own dark (the median of the inside
    pixels left of the label), so the bar can be stretched to any label."""
    t = list(tim.scan(cd.read("/SHELLDB2/OPTIONS.TMS")))[BAR_SHEET][1]
    b = t.rgba(0)
    px = lambda x, y: tuple(b[(y * t.width + x) * 4:(y * t.width + x) * 4 + 3])
    bar = [[px(x, BAR_TOP + y) for x in range(t.width)] for y in range(BAR_H)]
    for y in INSIDE:
        dark = sorted((px(x, BAR_TOP + y) for x in range(2)), key=sum)[0]
        bar[y] = [dark] * t.width
    return bar


def load_png_rgba(path):
    """A PNG as rows of (r, g, b, a) (8-bit RGBA or RGB)."""
    import zlib
    d = path.read_bytes()
    pos, idat = 8, b""
    while pos < len(d):
        n = struct.unpack(">I", d[pos:pos + 4])[0]
        t, c = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride, out, prev, i = w * bpp, [], bytearray(w * bpp), 0
    for _ in range(h):
        f = raw[i]
        row = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: row[x] = (row[x] + a) & 255
            elif f == 2: row[x] = (row[x] + b) & 255
            elif f == 3: row[x] = (row[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = row
        out.append([tuple(row[k * bpp:k * bpp + 3]) + ((row[k * bpp + 3],) if bpp == 4 else (255,)) for k in range(w)])
    return out


HAIR_FRAMES = range(1, 20)
KEY = (255, 0, 255)  # transparent


def hair_strip(pc_dir):
    frames = [load_png_rgba(pc_dir / f"SHELLDB1/TITLE_TPC_{i:02d}.png") for i in HAIR_FRAMES]
    h = len(frames[0])
    return [[(p[:3] if p[3] else KEY) for f in frames for p in f[y]] for y in range(h)]


def glass_levels(plate, glass):
    """Tone curves (one per channel) taking the PS1 menus' glass to the PC's.

    The PS1 menu pictures (and the clean plate made from them) show the same
    shattered glass as GLASS2, pixel for pixel, but with lifted blacks and
    other levels: for each level of the plate, the mean GLASS2 level at the
    same pixels, made non-decreasing."""
    curves = []
    for k in range(3):
        tot, cnt = [0] * 256, [0] * 256
        for prow, grow in zip(plate, glass):
            for p, g in zip(prow, grow):
                if p != (255, 0, 255):
                    tot[p[k]] += g[k]
                    cnt[p[k]] += 1
        curve, last = [], 0.0
        for v in range(256):
            # levels seldom seen borrow from their neighbors
            lo, hi, t, n = v, v, tot[v], cnt[v]
            while n < 50 and (lo > 0 or hi < 255):
                lo, hi = max(0, lo - 1), min(255, hi + 1)
                t, n = sum(tot[lo:hi + 1]), sum(cnt[lo:hi + 1])
            last = max(last, t / n if n else last)
            curve.append(round(last))
        curves.append(curve)
    return curves


ART = 90  # summed difference from the plate at which a pixel counts as artwork


def apply_levels(px, curves, plate):
    """The curves where px is the plate's glass; on the artwork (the car, the
    speaker, the flames) only the black level is taken down, keeping its
    colors; blended between by how far px is from the plate."""
    black = []
    for k in range(3):  # the plate level the curves take to (about) black
        black.append(max(v for v in range(256) if curves[k][v] <= curves[k][0] + 1))
    out = []
    for row, prow in zip(px, plate):
        line = []
        for p, q in zip(row, prow):
            d = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])
            w = 0.0 if q == (255, 0, 255) else max(0.0, 1.0 - d / ART)
            line.append(tuple(round(w * curves[k][p[k]] +
                                    (1 - w) * max(0, (p[k] - black[k]) * 255 / (255 - black[k])))
                              for k in range(3)))
        out.append(line)
    return out


def card_on(back, card):
    """card scaled to back's height (nearest), centered on it: a 4:3 picture on a 2:1 frame."""
    bh, bw, ch, cw = len(back), len(back[0]), len(card), len(card[0])
    w = cw * bh // ch
    x0 = (bw - w) // 2
    out = [list(row) for row in back]
    for y in range(bh):
        for x in range(w):
            out[y][x0 + x] = card[y * ch // bh][x * cw // w]
    return out


def network_on_glass(px, glass, fade=48):
    """The network screen's artwork (flame, ring, cable) over GLASS2 itself.

    Its glass is GLASS2's, pixel for pixel, but darker; the page's sides are
    GLASS2, so where the screen is plain glass GLASS2 is used, and the
    artwork (far from its glass after glass_levels' curves) keeps its colors,
    fading out over the last `fade` pixels at the left and right edges (it
    ran off the picture there)."""
    curves = glass_levels(px, glass)
    w = len(px[0])
    out = []
    for row, grow in zip(px, glass):
        line = []
        for x, (p, g) in enumerate(zip(row, grow)):
            c = [curves[k][p[k]] for k in range(3)]
            d = abs(c[0] - g[0]) + abs(c[1] - g[1]) + abs(c[2] - g[2])
            art = min(1.0, d / ART) * min(1.0, x / fade, (w - 1 - x) / fade)
            line.append(tuple(round(art * max(p[k], g[k]) + (1 - art) * g[k]) for k in range(3)))
        out.append(line)
    return out


# CARSEL.TPC images: each driver's two portraits, by car-select index (the
# drivers matched by eye with their BIOS info screens)
PORTRAITS = [
    (8, 14),   # Hammerhead
    (5, 0),    # Outlaw 2
    (26, 9),   # Warthog
    (3, 15),   # Mr. Grimm (with his helmet, then the skull)
    (4, 16),   # Grasshopper
    (10, 17),  # Thumper
    (7, 18),   # Spectre
    (6, 19),   # Roadkill
    (11, 20),  # Twister
    (12, 21),  # Axel
    (13, 22),  # Mr. Slam
    (2, 23),   # Shadow
    (27, 28),  # Sweet Tooth
    (29, 30),  # Minion
]


def portrait_strip(pc_dir):
    frames = [load_png_rgba(pc_dir / f"SHELLDB1/CARSEL_TPC_{i:02d}.png") for pair in PORTRAITS for i in pair]
    return [[p[:3] for f in frames for p in f[y]] for y in range(len(frames[0]))]


# Each driver's info screen and where its art is cropped (x of a 240x480
# region; the biography text ends at about x 345), by car-select index
INFO_ART = [("HH", 385), ("OT", 348), ("WH", 370), ("MG", 400), ("PV", 348), ("TH", 370), ("SP", 400),
            ("RK", 400), ("IR", 370), ("AX", 400), ("FL", 380), ("HS", 348), ("ST", 380), ("MN", 400)]
INFO_W = 240


def portrait_hq_strip(pc_dir):
    shots = [(load_png(pc_dir / f"BIOS/{code}INFO_TIM.png"), x0) for code, x0 in INFO_ART]
    return [[p for img, x0 in shots for p in img[y][x0:x0 + INFO_W]] for y in range(len(shots[0][0]))]


# The game's in-battle fonts (decomp font_init.c, font_print.c): UAFONT1.TIM
# and the glyph tables in the executable, 6 bytes a glyph (u, v, width,
# height, y offset s16) for the 77 codes '0'..'|', and each font's extra
# spacing. The port's pause menu draws with fonts 0 and 1 (the items, the
# same table), 5 (the password) and 6 (the title).
GAME_FONTS = [(0, 0x8016297C, -1), (1, 0x8016297C, -1), (5, 0x8016297C, 7), (6, 0x80162B4C, -1)]  # font, table, extra spacing
# The shell's font (CPFONT.TIM, fonts 2 and 3: car select's driver name), below UAFONT1 on the sheet
SHELL_FONTS = [(2, 0x80162D58, 0)]


def scale2x(img):
    """Scale2x (EPX): each pixel becomes four, the corners taking a
    neighbor's color where two neighbors agree, which rounds the steps of
    diagonals and nothing else (deterministic, shapes kept)."""
    h, w = len(img), len(img[0])
    out = [[None] * (2 * w) for _ in range(2 * h)]
    g = lambda x, y: img[min(max(y, 0), h - 1)][min(max(x, 0), w - 1)]  # noqa: E731
    for y in range(h):
        for x in range(w):
            p, a, b, c, d = img[y][x], g(x, y - 1), g(x + 1, y), g(x - 1, y), g(x, y + 1)
            out[2 * y][2 * x] = a if c == a and c != d and a != b else p
            out[2 * y][2 * x + 1] = b if a == b and a != c and b != d else p
            out[2 * y + 1][2 * x] = c if d == c and d != b and c != a else p
            out[2 * y + 1][2 * x + 1] = d if b == d and b != a and d != c else p
    return out


FONT_SCALE = 4  # the sheet is written 4 times as large: Scale2x, then each pixel doubled


def font_sheet(cd, name):
    sheet = list(tim.scan(cd.read(name)))[0][1]
    rgba = sheet.rgba(0)
    return [[(255, 0, 255) if rgba[(y * sheet.width + x) * 4 + 3] == 0 else
             tuple(rgba[(y * sheet.width + x) * 4:(y * sheet.width + x) * 4 + 3])
             for x in range(sheet.width)] for y in range(sheet.height)]


def game_font(cd):
    px = font_sheet(cd, "/SCREEN/UAFONT1.TIM")
    cp = font_sheet(cd, "/SCREEN/CPFONT.TIM")
    shell_v = len(px)  # the shell font's rows start here (game pixels)
    width = max(len(px[0]), len(cp[0]))
    px = [row + [(255, 0, 255)] * (width - len(row)) for row in px + cp]
    # for a window up to 1080 lines (4.5 screen pixels a game pixel): the
    # diagonals' steps smoothed (Scale2x), then doubled, so it stays the
    # game's squared lettering rather than looking redrawn
    px = [[p for p in row for _ in (0, 1)] for row in scale2x(px) for _ in (0, 1)]
    exe = cd.read("/SCUS_943.06")
    base = int.from_bytes(exe[0x18:0x1C], "little")  # where the executable is loaded
    lines = ["# font spacing, then 77 glyphs: u v w h yoff (tools/art/ui_assets.py)"]
    for font, table, spacing in GAME_FONTS + SHELL_FONTS:
        o = table - base + 0x800
        t = exe[o:o + 77 * 6]
        lines.append(f"font {font} {spacing} {shell_v if (font, table, spacing) in SHELL_FONTS else 0}")
        for g in range(77):
            e = t[g * 6:g * 6 + 6]
            lines.append(f"{e[0]} {e[1]} {e[2]} {e[3]} {int.from_bytes(e[4:6], 'little', signed=True)}")
    return px, "\n".join(lines) + "\n"


# The Credits portrait: Sweet Tooth from the game's advertising
# (references/art/sweet_tooth_promo_hq.png, the user's; 512x512 with real
# transparency), cropped to him, centered a little high on black, 240x480
# (twice the portraits' size, for its detail; drawn scaled).


def credits_portrait(promo):
    if not promo.exists():
        return None
    p = load_png_rgba(promo)
    n, m = len(p), len(p[0])
    ys = [y for y in range(n) if any(q[3] > 0 for q in p[y])]  # every visible pixel, faint flame tips too
    xs = [x for x in range(m) if any(p[y][x][3] > 0 for y in range(n))]
    x0, y0, bw, bh = min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
    w, h = 240, 480
    sc = min((w - 24) / bw, (h * 0.8) / bh)  # a margin each side
    dw, dh = int(bw * sc), int(bh * sc)
    ox, oy = (w - dw) // 2, int(h * 0.42 - dh / 2)
    out = [[(0, 0, 0)] * w for _ in range(h)]
    for y in range(dh):
        for x in range(dw):
            # the average of the source pixels this one covers (premultiplied)
            sx0, sx1 = x0 + x / sc, x0 + (x + 1) / sc
            sy0, sy1 = y0 + y / sc, y0 + (y + 1) / sc
            acc, cnt = [0.0, 0.0, 0.0], 0
            for yy in range(int(sy0), max(int(sy0) + 1, int(sy1))):
                for xx in range(int(sx0), max(int(sx0) + 1, int(sx1))):
                    q = p[min(n - 1, yy)][min(m - 1, xx)]
                    a = q[3] / 255
                    for k in range(3):
                        acc[k] += q[k] * a
                    cnt += 1
            out[oy + y][ox + x] = tuple(int(acc[k] / cnt) for k in range(3))
    return out


# The HUD's weapon icons and two pickups' sprites (the PC's GAMEPLAY_SHARED images), for the
# Challenge Match weapon and pickup rows: homing, fire, power, napalm, remote, ricochet,
# lightning, health, turbo, the special
ICONS = ["302", "299", "305", "043", "306", "307", "304", "105", "033", "301"]
ICON_CELL = 64


def icon_strip(pc_dir):
    """The icons side by side, each scaled whole (nearest, a whole factor) into a 64x64 cell, magenta where clear."""
    strip = [[KEY] * (ICON_CELL * len(ICONS)) for _ in range(ICON_CELL)]
    for i, name in enumerate(ICONS):
        img = load_png_rgba(pc_dir / f"GAMEPLAY_SHARED/{name}.png")
        h, w = len(img), len(img[0])
        k = max(1, min(ICON_CELL // w, ICON_CELL // h))
        if w * k > ICON_CELL or h * k > ICON_CELL:
            k = 1
        ox, oy = i * ICON_CELL + (ICON_CELL - w * k) // 2, (ICON_CELL - h * k) // 2
        for y in range(min(h * k, ICON_CELL)):
            for x in range(min(w * k, ICON_CELL)):
                p = img[y // k][x // k]
                if p[3] and p[:3] != (0, 0, 0):
                    strip[oy + y][ox + x] = p[:3]
    return strip


def plate_strip(cd):
    """Each driver's opponent card (SCREEN/<code>PLATE.TIM, 128x64) side by side, in car-select order."""
    strip = [[(0, 0, 0)] * (128 * len(INFO_ART)) for _ in range(64)]
    for idx, (code, _x0) in enumerate(INFO_ART):
        t, _ = tim.parse(cd.read(f"/SCREEN/{code}PLATE.TIM"))
        px = t.rgba()
        for y in range(64):
            for x in range(128):
                o = (y * 128 + x) * 4
                strip[y][idx * 128 + x] = (px[o], px[o + 1], px[o + 2])
    return strip


ARENA_CARDS = ("NONE", "LA", "MOSC", "PARIS", "AMAZ", "NY", "ANT", "HOLL", "HONG", "SWAMP", "CYB", "OROOF")


def arena_cards(pc_dir, out):
    """arena_card_N.bmp: each arena's level-select card (the PC's SC-*, 344x260, its name on it), by the match's arena
    choice (0 Random, 1-8, 9-11 Suicide Swamp, Cyburbia, Roof Tops); arena_9..11.bmp: the secret ones' 2:1 pictures for
    the Survival page, cut from their cards (the scene under the name)"""
    for i, code in enumerate(ARENA_CARDS):
        px = load_png(pc_dir / f"SHELLDB3/SC-{code}_PCX.png")
        save_bmp(out / f"arena_card_{i}.bmp", px)
        if i >= 9:  # in the 2:1 pictures' own brick frame (Los Angeles's), the card's scene under its name inside
            frame = [list(row) for row in load_png(pc_dir / "SHELLDB1/LEVELSEL_TPC_02.png")]
            fx0, fy0, fx1, fy1 = ARENA_WIDE_OPENING         # the frame's opening (measured on the 8 arenas' pictures)
            b = ARENA_CARD_BORDER
            cx0, cx1, cy1 = b, len(px[0]) - b, len(px) - b    # the card inside its own stone border
            cy0 = cy1 - (cx1 - cx0) * (fy1 - fy0) // (fx1 - fx0)  # the scene: the same 2:1, from the bottom up
            for y in range(fy0, fy1):
                for x in range(fx0, fx1):
                    frame[y][x] = px[cy0 + (y - fy0) * (cy1 - cy0) // (fy1 - fy0)][cx0 + (x - fx0) * (cx1 - cx0) // (fx1 - fx0)]
            save_bmp(out / f"arena_{i}.bmp", frame)


ARENA_WIDE_OPENING = (15, 8, 496, 248)  # the 2:1 pictures' brick frame: its opening (x0, y0, x1, y1), LEVELSEL_TPC_02
ARENA_CARD_BORDER = 10                   # a level-select card's stone border (SC-*, 344 x 260)

TEMPLATE_GUIDE = """Arena pictures: templates (made from your disc's art by tools/art/ui_assets.py --templates)

wide_template.png  the 2:1 picture's brick frame (Survival's arena picture, the game's level-select frame), {w} x {h}.
                   The opening (transparent) is x {x0}..{x1}, y {y0}..{y1} ({ow} x {oh}, 2:1): put your arena's
                   scene in a layer under the frame, then save it as wide.bmp (24-bit BMP, the same size).
                   Without wide.bmp the game makes it from card.bmp (the scene under the name, in this frame).
card_template.png  a level-select card, 344 x 260 (4:3), the game's stone border kept ({b} pixels), the inside
                   transparent: your arena's scene and its name inside, saved as card.bmp (24-bit BMP). Required:
                   the server browser, the lobbies and the arena lists show it.
"""


def save_png_rgba(path, px):
    """rows of (r, g, b, a) as an 8-bit RGBA PNG"""
    import zlib
    h, w = len(px), len(px[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(t, c):  # noqa: E306
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def arena_templates(pc_dir, out):
    """mods/arenas/_TEMPLATE's pictures for modders: the brick frame with its opening clear, a card with its inside
    clear, and the guide to them"""
    out.mkdir(parents=True, exist_ok=True)
    frame = [[tuple(p[:3]) + (255,) for p in row] for row in load_png(pc_dir / "SHELLDB1/LEVELSEL_TPC_02.png")]
    x0, y0, x1, y1 = ARENA_WIDE_OPENING
    for y in range(y0, y1):
        for x in range(x0, x1):
            frame[y][x] = (0, 0, 0, 0)
    save_png_rgba(out / "wide_template.png", frame)
    card = [[tuple(p[:3]) + (255,) for p in row] for row in load_png(pc_dir / f"SHELLDB3/SC-{ARENA_CARDS[1]}_PCX.png")]
    b = ARENA_CARD_BORDER
    for y in range(b, len(card) - b):
        for x in range(b, len(card[0]) - b):
            card[y][x] = (0, 0, 0, 0)
    save_png_rgba(out / "card_template.png", card)
    (out / "PICTURES.txt").write_text(TEMPLATE_GUIDE.format(w=len(frame[0]), h=len(frame), x0=x0, x1=x1, y0=y0, y1=y1,
                                                            ow=x1 - x0, oh=y1 - y0, b=b), encoding="utf-8")


def save_bmp(path, px):
    h, w = len(px), len(px[0])
    stride = (w * 3 + 3) & ~3
    data = bytearray()
    for row in reversed(px):
        line = bytearray()
        for r, g, b in (p[:3] for p in row):
            line += bytes((b, g, r))
        data += line + b"\0" * (stride - len(line))
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    path.write_bytes(hdr + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--disc", type=Path, default=ROOT / "references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue")
    ap.add_argument("--pc", type=Path, default=ROOT / "extracted/images-pc")
    ap.add_argument("--clean", type=Path, default=ROOT / "extracted/menus-clean")
    ap.add_argument("--templates", type=Path, help="only write the mod arena picture templates here (mods/arenas/_TEMPLATE)")
    a = ap.parse_args()
    if a.templates:
        arena_templates(a.pc, a.templates)
        return
    a.out.mkdir(parents=True, exist_ok=True)
    glass = load_png(a.pc / "SHELLDB2/GLASS2_TIM.png")
    plate = load_png(a.clean / "plate_glass.png")
    curves = glass_levels(plate, glass)
    save_bmp(a.out / "options.bmp", apply_levels(load_png(a.clean / "SHELLDB2_OPTS_TIM.png"), curves, plate))
    save_bmp(a.out / "audio.bmp", apply_levels(load_png(a.clean / "SHELLDB2_AUDIO_TIM.png"), curves, plate))
    save_bmp(a.out / "single.bmp", apply_levels(load_png(a.clean / "SHELLDB1_SINGLE_TIM.png"), curves, plate))
    save_bmp(a.out / "glass.bmp", glass)
    save_bmp(a.out / "title.bmp", load_png(a.pc / "SHELLDB1/TITLE_TIM.png"))
    save_bmp(a.out / "hair.bmp", hair_strip(a.pc))
    credits = credits_portrait(ROOT / "references/art/sweet_tooth_promo_hq.png")
    if credits:
        save_bmp(a.out / "credits_portrait.bmp", credits)
    # car select, rebuilt by the port: its clean picture, the SELECT CAR /
    # CAR INFO sheet and the 1 player emblem (magenta where transparent)
    save_bmp(a.out / "carsel.bmp", load_png(a.clean / "SHELLDB1_CARSEL_TIM.png"))
    keyed = lambda img: [[(p[:3] if p[3] else KEY) for p in row] for row in img]  # noqa: E731
    save_bmp(a.out / "carsel_titles.bmp", keyed(load_png_rgba(a.pc / "SHELLDB1/CARSEL_TPC_01.png")))
    save_bmp(a.out / "emblem1.bmp", keyed(load_png_rgba(a.pc / "SHELLDB1/CARSEL_TPC_24.png")))
    save_bmp(a.out / "emblem2.bmp", keyed(load_png_rgba(a.pc / "SHELLDB1/CARSEL_TPC_25.png")))  # (split car select: players 2-4)
    save_bmp(a.out / "portraits.bmp", portrait_strip(a.pc))
    save_bmp(a.out / "portraits_hq.bmp", portrait_hq_strip(a.pc))
    # each driver's CAR INFO picture (the PC's, 640x480), for the split-screen car select
    # and with Vanilla Plus's ratings (carinfo_stats.py: only the balls differ), carinfo_vp_NN.bmp
    for idx, (code, _x0) in enumerate(INFO_ART):
        info = load_png(a.pc / f"BIOS/{code}INFO_TIM.png")
        save_bmp(a.out / f"carinfo_{idx:02d}.bmp", info)
        if idx in carinfo_stats.VP_STATS:
            save_bmp(a.out / f"carinfo_vp_{idx:02d}.bmp", carinfo_stats.restat(info, carinfo_stats.VP_STATS[idx]))
    save_bmp(a.out / "network.bmp", network_on_glass(load_png(a.clean / "NETMAIN_clean.png"), glass))
    # the opponent cards' screen of each arena (the PC's, 640x480), for the Challenge Match line-up
    for level in range(1, 9):
        save_bmp(a.out / f"lineup_{level}.bmp", load_png(a.pc / f"SHELLDB1/SHOWOPS{level}_TIM.png"))
    # the co-op duel's screen (COWIN's lettering, tools/art/duel_text.py), shown in place of COWIN
    if (a.clean / "DUEL_TEXT.png").exists():
        save_bmp(a.out / "duel.bmp", load_png(a.clean / "DUEL_TEXT.png"))
    # the Customize HUD page's preview: one battle moment with the HUD, without it, and with the
    # full weapon list (extracted/hud-preview, captured by tools/art/hud_preview.sh); skipped without them
    prev = ROOT / "extracted/hud-preview"
    for name in ("hud", "nohud", "list"):
        if (prev / f"{name}.png").exists():
            save_bmp(a.out / f"hud_{name}.bmp", load_png(prev / f"{name}.png"))
    # each arena's level-select picture (the PC's, 512x256), for the Survival page; 0 the bare frame (Random)
    for level, tpc in enumerate((0, 2, 3, 1, 5, 6, 4, 8, 7)):
        px = load_png(a.pc / f"SHELLDB1/LEVELSEL_TPC_{tpc:02d}.png")
        if level == 0:  # Random: the game's "no level" card (SC-NONE: the logo on bricks) on the bare frame
            px = card_on(px, load_png(a.pc / "SHELLDB3/SC-NONE_PCX.png"))
        save_bmp(a.out / f"arena_{level}.bmp", px)
    arena_cards(a.pc, a.out)
    # the level-select board's back: the bare brick wall ("PARKING / POLICE ORDER"), seen as Survival's picture turns
    save_bmp(a.out / "arena_back.bmp", load_png(a.pc / "SHELLDB1/LEVELSEL_TPC_00.png"))
    cd = CdImage(str(a.disc))
    save_bmp(a.out / "plates.bmp", plate_strip(cd))
    save_bmp(a.out / "icons.bmp", icon_strip(a.pc))
    font_px, font_txt = game_font(cd)
    save_bmp(a.out / "gamefont.bmp", font_px)
    (a.out / "gamefont.txt").write_text(font_txt, encoding="utf-8")
    bar = clean_bar(cd)
    save_bmp(a.out / "bar.bmp", bar)
    import movie_portraits  # the stills from the movies (needs ffmpeg)
    movie_portraits.build(a.out, a.disc)
    import subprocess  # the Antarctica Santa (from the PC version's files)
    subprocess.run([sys.executable, str(ROOT / "tools/art/santa.py"), "--out", str(a.out)], check=False)
    print(f"{a.out}: options.bmp, audio.bmp, single.bmp, glass.bmp, title.bmp, hair.bmp, gamefont.bmp, portraits.bmp, portraits_hq.bmp, network.bmp, bar.bmp ({len(bar[0])}x{len(bar)})")


if __name__ == "__main__":
    main()
