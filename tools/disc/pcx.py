"""Decode ZSoft PCX images (8-bit palette or 24-bit, RLE) to RGBA.

The 1997 PC version of TM2 keeps its PC-only screens in SHELLDB3 as 24-bit
PCX (8 bits per plane, 3 planes).
"""
import struct


class Pcx:
    """Same fields the gallery reads from tim.Tim."""
    def __init__(self, width, height, bpp, rgba):
        self.width = width
        self.height = height
        self.bpp = bpp
        self.palettes = []
        self.vram = (0, 0)
        self._rgba = rgba

    def rgba(self, palette=0):
        return self._rgba


def parse(data):
    if len(data) < 128 or data[0] != 0x0A or data[2] != 1:
        raise ValueError("not an RLE PCX")
    bits, planes = data[3], data[65]
    x0, y0, x1, y1 = struct.unpack_from("<4H", data, 4)
    stride = struct.unpack_from("<H", data, 66)[0]
    w, h = x1 - x0 + 1, y1 - y0 + 1
    if bits != 8 or planes not in (1, 3):
        raise ValueError(f"unsupported PCX: {bits} bits x {planes} planes")
    need = stride * planes * h
    raw = bytearray()
    i = 128
    while len(raw) < need and i < len(data):
        b = data[i]
        i += 1
        if b >= 0xC0:
            raw += bytes([data[i]]) * (b & 0x3F)
            i += 1
        else:
            raw.append(b)
    out = bytearray()
    if planes == 3:
        for y in range(h):
            row = raw[y * stride * 3:(y + 1) * stride * 3]
            r, g, b = row[:w], row[stride:stride + w], row[2 * stride:2 * stride + w]
            for k in range(w):
                out += bytes((r[k], g[k], b[k], 255))
    else:
        pal = data[-768:] if len(data) > 769 and data[-769] == 0x0C else bytes(range(256)) * 3
        for y in range(h):
            for v in raw[y * stride:y * stride + w]:
                out += pal[v * 3:v * 3 + 3] + b"\xff"
    return Pcx(w, h, 8 * planes, bytes(out))
