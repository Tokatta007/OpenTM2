"""Car Info pictures with Vanilla Plus's ratings: the balls of the ARMOR,
SPECIAL WEAPON and SPEED rows added or taken away, nothing else changed.

Used by ui_assets.py (carinfo_vp_NN.bmp); on its own, for a look:

    py tools/art/carinfo_stats.py IN.bmp OUT.bmp ARMOR SPECIAL SPEED

The PC's BIOS/<xx>INFO.TIM pictures (640x480) share the stats panel: five
ball slots per row at x 203, 231, 261, 289 and 317 (centers; the rows' y
differ by a few pixels from picture to picture, so each row's is measured
from its first ball, which every row has). A ball is added by copying the
row's first ball into the slot through a soft round mask; one is taken away
by copying an empty slot of the same row over it the same way (the
parchment's grain stays the picture's own).

Standard library only. The output is the game's art: keep it out of the
repository.
"""
import struct
import sys

SLOTS_X = (203, 231, 261, 289, 317)
ROWS_Y = (373, 402, 431)  # ARMOR, SPECIAL WEAPON, SPEED: where to look
DARK = 120                # r + g + b below this: a ball's pixel

# Vanilla Plus's ratings (armor, special weapon, speed) by car-select index,
# for the cars whose picture differs from them (TM2 - Vanilla Plus.xlsx,
# Balancing: "Driver Info Screen (PROPOSED)"). The others keep the PC's.
VP_STATS = {
    0: (4, 4, 1),   # Hammerhead
    1: (3, 3, 4),   # Outlaw 2
    2: (5, 3, 2),   # Warthog
    4: (2, 3, 4),   # Grasshopper
    5: (3, 5, 3),   # Thumper
    6: (2, 4, 4),   # Spectre (special 4: it nearly always hits, the user's)
    7: (3, 4, 3),   # Roadkill
    9: (4, 3, 2),   # Axel
    10: (5, 5, 1),  # Mr. Slam
    11: (3, 4, 3),  # Shadow
    12: (5, 3, 1),  # Sweet Tooth
}


def _dark(px, x, y):
    return sum(px[y][x][:3]) < DARK


def _row_y(px, guess):
    pts = [y for y in range(guess - 12, guess + 13) for x in range(SLOTS_X[0] - 8, SLOTS_X[0] + 9) if _dark(px, x, y)]
    return round(sum(pts) / len(pts)) if pts else guess


def _has_ball(px, cx, cy):
    n = sum(1 for y in range(cy - 9, cy + 10) for x in range(cx - 7, cx + 8) if _dark(px, x, y))
    return n >= 90


def _paste(px, src, sx, sy, dx, dy, rx, ry):
    """src's pixels around (sx, sy) onto px around (dx, dy), through a soft ellipse."""
    for oy in range(-ry - 3, ry + 4):
        for ox in range(-rx - 3, rx + 4):
            r = ((ox / rx) ** 2 + (oy / ry) ** 2) ** 0.5
            a = 1.0 if r <= 0.85 else 0.0 if r >= 1.2 else (1.2 - r) / 0.35
            if a <= 0:
                continue
            s = src[sy + oy][sx + ox]
            d = px[dy + oy][dx + ox]
            px[dy + oy][dx + ox] = tuple(round(d[c] + (s[c] - d[c]) * a) for c in range(3))


def counts(px):
    """(armor, special, speed) as the picture shows them."""
    out = []
    for guess in ROWS_Y:
        y = _row_y(px, guess)
        out.append(sum(1 for x in SLOTS_X if _has_ball(px, x, y)))
    return tuple(out)


def restat(px, stats):
    """A copy of the picture with stats = (armor, special, speed) balls."""
    src = [list(row) for row in px]
    out = [list(row) for row in px]
    for guess, want in zip(ROWS_Y, stats):
        y = _row_y(src, guess)
        has = [_has_ball(src, x, y) for x in SLOTS_X]
        empty = [x for x, h in zip(SLOTS_X, has) if not h]
        for k, x in enumerate(SLOTS_X):
            if k < want and not has[k]:
                _paste(out, src, SLOTS_X[0], y, x, y, 9, 11)
            elif k >= want and has[k]:
                if not empty:
                    raise ValueError("no empty slot to take the parchment from")
                _paste(out, src, empty[-1], y, x, y, 11, 12)
    return out


def _load_bmp(path):
    d = open(path, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        base = off + ((abs(h) - 1 - y) if h > 0 else y) * stride
        rows.append([(d[base + 3 * x + 2], d[base + 3 * x + 1], d[base + 3 * x]) for x in range(w)])
    return rows


def _save_bmp(path, px):
    h, w = len(px), len(px[0])
    stride = (w * 3 + 3) & ~3
    out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54) +
                    struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0))
    for row in reversed(px):
        out += b"".join(bytes((b, g, r)) for r, g, b in row) + b"\0" * (stride - w * 3)
    open(path, "wb").write(out)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    img = _load_bmp(sys.argv[1])
    print("before", counts(img))
    img = restat(img, tuple(int(v) for v in sys.argv[3:6]))
    print("after", counts(img))
    _save_bmp(sys.argv[2], img)
