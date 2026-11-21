"""The Antarctica Santa (pedestrian 516 #2 of the PC version) for the PS1 level.

    py tools/art/santa.py [--pc "references/TM2-PC-Version/Game Files"] --out build/port/ui

Builds, from the PC version's LEVELDB\\GLACIER.DPC and GLACIER.TPC (your own copy),
the two files the port adds to Antarctica (src/vplus/fixes.c, santa_*), following
the decomp's docs/SANTA.md (way A, "a relocatable asset, like a car"):

- santa.tms: a texture container of his three 64x64 8-bit images (PC textures
  296-298, their palettes from RGB555 to the PS1's BGR555), parsed as a car's;
- santa.dmd: a one-root model image of 592 bytes: the select node 516 #2, its
  empty step, the 2-frame standing loop and the 3-frame hit animation, five
  meshes of one camera-facing billboard each (the PC primitives rewritten in
  the PS1's 36-byte form), and three vertices; installed as a car's model, the
  game relocates it and points its primitives at the images.

The output is the game's art: it stays in the build folder, not the repository.
"""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = 0x80019C40  # the address the model's pointers are built for (relocated at install)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def tpc_image(tpc, index):
    """The index-th TIM of a .TPC: (palette words, width, height, pixel bytes)."""
    o = 0x10
    for _ in range(index):
        o += 4 + u32(tpc, o)
    tim = tpc[o + 4:o + 4 + u32(tpc, o)]
    assert u32(tim, 0) == 0x10, "not a TIM"
    clut_len = u32(tim, 8)
    pal = list(struct.unpack_from("<256H", tim, 8 + 12))
    p = 8 + clut_len
    w16, h = struct.unpack_from("<HH", tim, p + 8)
    pix = tim[p + 12:p + 12 + w16 * 2 * h]
    return pal, w16 * 2, h, pix


def ps1_color(c):  # PC 0RRRRRGGGGGBBBBB to the PS1's SBBBBBGGGGGRRRRR
    return (c & 0x8000) | ((c & 0x1F) << 10) | (c & 0x3E0) | ((c >> 10) & 0x1F)


def tim(pal, w, h, pix):
    clut = struct.pack("<IHHHH", 12 + 512, 0, 0, 256, 1) + struct.pack("<256H", *[ps1_color(c) for c in pal])
    block = struct.pack("<IHHHH", 12 + len(pix), 0, 0, w // 2, h) + pix
    return struct.pack("<II", 0x10, 0x09) + clut + block


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pc", type=Path, default=ROOT / "references/TM2-PC-Version/Game Files")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    dpc = (a.pc / "LEVELDB/GLACIER.DPC").read_bytes()
    tpc = (a.pc / "LEVELDB/GLACIER.TPC").read_bytes()

    # the texture container: "TXSP", version, stamp, count, a key per image, then each image's size and TIM
    tims = [tim(*tpc_image(tpc, i)) for i in (296, 297, 298)]
    stamp = 0x53414E54  # any
    tms = struct.pack("<IIII", 0x50535854, 0x43, stamp, 3) + struct.pack("<3I", 0x5A71, 0x5A72, 0x5A73)
    for t in tims:
        tms += struct.pack("<I", len(t)) + t

    # the model image (docs/SANTA.md 1.6): nodes copied from the PC file, pointers rebuilt
    img = bytearray(0x250)
    P = lambda off: BASE + off  # noqa: E731
    struct.pack_into("<8I", img, 0, 0x50535844, 0x43, stamp, BASE, 3, P(0x18), 1, P(0x20))
    sel = dpc[0x03C41C:0x03C41C + 0x10]  # select 516 #2: 09 05 04 02 02 00 03 00 ...
    img[0x20:0x30] = sel
    struct.pack_into("<I", img, 0x2C, 0)  # +0x0C chained: none
    struct.pack_into("<3I", img, 0x30, P(0x3C), P(0x54), P(0x7C))
    img[0x3C] = 1  # step 0: an empty group
    stand, hit = dpc[0x03C2B4:0x03C2B4 + 0x20], dpc[0x03C3F0:0x03C3F0 + 0x20]
    img[0x54:0x74] = stand
    struct.pack_into("<I", img, 0x54 + 0x1C, 0)
    struct.pack_into("<2I", img, 0x74, P(0xA8), P(0xD4))
    img[0x7C:0x9C] = hit
    struct.pack_into("<I", img, 0x7C + 0x1C, P(0x20))  # its end: the select node, step 0
    struct.pack_into("<3I", img, 0x9C, P(0x100), P(0x12C), P(0x158))
    pc_meshes = [0x03C1FC, 0x03C258, 0x03C2DC, 0x03C338, 0x03C394]
    verts = [0, 0, 0, 1, 2]           # his three vertices: feet z 34, 53, 72
    images = [0, 1, 2, 2, 2]
    for k, m in enumerate(pc_meshes):
        o = 0xA8 + 44 * k
        img[o:o + 44] = dpc[m:m + 44]
        img[o + 1] = 0xFF
        struct.pack_into("<H", img, o + 2, 0)
        struct.pack_into("<3II", img, o + 4, P(0x238), P(0x238), P(0x184 + 36 * k), 1)
        pc_prim = u32(dpc, m + 0x0C) - BASE
        half_w, half_l, extra = struct.unpack_from("<hhh", dpc, pc_prim + 6)
        q = 0x184 + 36 * k
        struct.pack_into("<BBBBhhhh", img, q, 4, 1, 9, 9, verts[k], half_w, half_l, extra)
        struct.pack_into("<6I", img, q + 12, 0x95810000, 0x2C7F7F7F, 0x00000000,
                         0x0080003F | images[k] << 16, 0x00003F00, 0x00003F3F)
    for i, z in enumerate((34, 53, 72)):
        struct.pack_into("<hhhh", img, 0x238 + 8 * i, 0, 0, z, 0)

    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "santa.tms").write_bytes(tms)
    (a.out / "santa.dmd").write_bytes(bytes(img))
    print("santa: %d + %d bytes -> %s" % (len(tms), len(img), a.out))


if __name__ == "__main__":
    main()
