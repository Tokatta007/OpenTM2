"""Decode PlayStation TIM images to RGBA and write PNGs (standard library only).

TIM layout: u32 magic 0x10, u32 flags (bits 0-1 pixel mode: 0 = 4 bpp,
1 = 8 bpp, 2 = 16 bpp, 3 = 24 bpp; bit 3 = CLUT present; bit 4, set on
many images in the 1997 PC version's .TPC packs, meaning is unknown), then an optional
CLUT block and the image block. Each block is u32 length (including its
12-byte header), u16 x, y, w, h (VRAM position; w in 16-bit units), data.

Colors are 15-bit BGR with the STP bit 15, except in the 1997 PC version's
.TPC packs, where every image is RGB (rgb=True: red and blue swapped; the
car cards, Sweet Tooth's hair and the bars come out blue without it, while
the PC's .TIM files are BGR as on the PS1). A color word of 0x0000 is drawn
as fully transparent by the GPU; everything else is opaque here (STP only
matters for semi-transparent primitives, which a still image cannot show).
"""
import struct
import zlib

BPP = {0: 4, 1: 8, 2: 16, 3: 24}


class Tim:
    def __init__(self, bpp, width, height, pixels, palettes, vram, clut_vram, swap_rb=False):
        self.bpp = bpp            # 4, 8, 16 or 24
        self.width = width        # in pixels
        self.height = height
        self.pixels = pixels      # indices (4/8 bpp), u16 colors (16), RGB tuples (24)
        self.palettes = palettes  # list of lists of u16 colors (4/8 bpp only)
        self.vram = vram          # (x, y) of the image in VRAM
        self.clut_vram = clut_vram
        self.swap_rb = swap_rb    # PC .TPC: the 15-bit colors are RGB

    def rgba(self, palette=0):
        """Return RGBA bytes, row-major."""
        out = bytearray()
        if self.bpp == 24:
            for r, g, b in self.pixels:
                out += bytes((r, g, b, 255))
            return bytes(out)
        pal = self.palettes[palette] if self.bpp in (4, 8) else None
        for p in self.pixels:
            c = pal[p] if pal is not None and p < len(pal) else p
            rgba = color(c)
            out += bytes((rgba[2], rgba[1], rgba[0], rgba[3])) if self.swap_rb else rgba
        return bytes(out)


def color(c):
    if c == 0:
        return b"\0\0\0\0"
    r = (c & 31) << 3
    g = ((c >> 5) & 31) << 3
    b = ((c >> 10) & 31) << 3
    return bytes((r | r >> 5, g | g >> 5, b | b >> 5, 255))


def looks_like_tim(data, off):
    if len(data) - off < 20 or struct.unpack_from("<I", data, off)[0] != 0x10:
        return False
    flags = struct.unpack_from("<I", data, off + 4)[0]
    return flags & ~0x1B == 0


def parse(data, off=0, rgb=False):
    """Parse a TIM at data[off:]. Returns (Tim, size in bytes) or raises ValueError."""
    if not looks_like_tim(data, off):
        raise ValueError("not a TIM")
    flags = struct.unpack_from("<I", data, off + 4)[0]
    mode = flags & 3
    bpp = BPP[mode]
    p = off + 8
    palettes = []
    clut_vram = None
    if flags & 8:
        blen, cx, cy, cw, ch = struct.unpack_from("<I4H", data, p)
        if blen < 12 or p + blen > len(data) or cw * ch * 2 + 12 > blen:
            raise ValueError("bad CLUT block")
        words = struct.unpack_from(f"<{cw * ch}H", data, p + 12)
        per = 16 if bpp == 4 else 256 if bpp == 8 else cw
        flat = list(words)
        palettes = [flat[i:i + per] for i in range(0, len(flat), per)] or [flat]
        clut_vram = (cx, cy)
        p += blen
    elif bpp in (4, 8):
        # No CLUT: show the indices as grays so the image is still visible.
        n = 16 if bpp == 4 else 256
        palettes = [[(v * 31 // (n - 1)) * 0x421 | (1 if v == 0 else 0) for v in range(n)]]
    blen, ix, iy, iw, ih = struct.unpack_from("<I4H", data, p)
    nbytes = iw * ih * 2
    if blen < 12 or nbytes == 0 or p + 12 + nbytes > len(data):
        raise ValueError("bad image block")
    raw = data[p + 12:p + 12 + nbytes]
    if bpp == 4:
        width = iw * 4
        pixels = [v for b in raw for v in (b & 15, b >> 4)]
    elif bpp == 8:
        width = iw * 2
        pixels = list(raw)
    elif bpp == 16:
        width = iw
        pixels = list(struct.unpack(f"<{iw * ih}H", raw))
    else:
        width = iw * 2 // 3
        stride = iw * 2
        pixels = []
        for y in range(ih):
            row = raw[y * stride:y * stride + width * 3]
            pixels += [tuple(row[i:i + 3]) for i in range(0, width * 3, 3)]
    end = p + max(blen, 12 + nbytes)
    return Tim(bpp, width, ih, pixels, palettes, (ix, iy), clut_vram, rgb), end - off


def scan(data, rgb=False):
    """Yield (offset, Tim) for every TIM found in a blob (TMS packs, etc.)."""
    off = 0
    while off <= len(data) - 20:
        if looks_like_tim(data, off):
            try:
                tim, size = parse(data, off, rgb)
            except (ValueError, struct.error):
                off += 4
                continue
            yield off, tim
            off += (size + 3) & ~3
        else:
            off += 4


def write_png(path, width, height, rgba):
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    stride = width * 4
    raw = b"".join(b"\0" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
