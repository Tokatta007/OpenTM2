"""Fixture files: CPU state on entry to a function and on return.

Layout (little-endian):
    "RCFX" u32 version=1, u32 func, u32 frame, u32 flags (bit 0: interrupted)
    entry: 34 u32 (r0..r31, hi, lo), 64 u32 GTE (32 data, 32 control),
           1024 bytes scratchpad, u32 n + n bytes zlib(2 MB RAM)
    exit:  same as entry, except that its RAM is zlib(entry RAM XOR exit RAM)
"""
import struct
import zlib

MAGIC = b"RCFX"


def _pack_state(state, base=None):
    cpu, gte, scratch, ram = state
    if base is not None:
        ram = (int.from_bytes(ram, "little") ^ int.from_bytes(base, "little")).to_bytes(len(ram), "little")
    z = zlib.compress(ram, 1)
    return (struct.pack("<34I", *cpu) + struct.pack("<64I", *gte) + scratch
            + struct.pack("<I", len(z)) + z)


def write(path, func, frame, interrupted, entry, exit_):
    with open(path, "wb") as f:
        f.write(MAGIC + struct.pack("<4I", 1, func, frame, 1 if interrupted else 0))
        f.write(_pack_state(entry))
        f.write(_pack_state(exit_, base=entry[3]))


def _unpack_state(data, o):
    cpu = list(struct.unpack_from("<34I", data, o)); o += 136
    gte = list(struct.unpack_from("<64I", data, o)); o += 256
    scratch = data[o:o + 1024]; o += 1024
    n = struct.unpack_from("<I", data, o)[0]; o += 4
    ram = zlib.decompress(data[o:o + n]); o += n
    return (cpu, gte, scratch, ram), o


def read(path):
    data = open(path, "rb").read()
    if data[:4] != MAGIC:
        raise ValueError(f"{path}: not a fixture")
    version, func, frame, flags = struct.unpack_from("<4I", data, 4)
    entry, o = _unpack_state(data, 20)
    exit_, o = _unpack_state(data, o)
    cpu, gte, scratch, x = exit_
    ram = (int.from_bytes(x, "little") ^ int.from_bytes(entry[3], "little")).to_bytes(len(x), "little")
    exit_ = (cpu, gte, scratch, ram)
    return {"func": func, "frame": frame, "interrupted": bool(flags & 1), "entry": entry, "exit": exit_}
