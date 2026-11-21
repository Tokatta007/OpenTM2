"""Shared readers: GRP (worldSectionAt replica from cybroute/val.py), PTS (4 successors), RMP."""
import struct, os
RET = os.environ.get('TM2_TERRAIN', '')  # the disc's TERRAIN folder (extracted; not in the repository)
def load(name, ext): return open(os.path.join(RET, name + '.' + ext), 'rb').read()
class GRP:
    def __init__(s, d):
        s.d = bytes(d); s.count = d[0x1790]
        s.pos = [struct.unpack_from('<3i', d, 0x0C + 12 * i) for i in range(35)]
        s.w = struct.unpack_from('<35i', d, 0x1B0); s.l = struct.unpack_from('<35i', d, 0x23C)
        s.sn = struct.unpack_from('<35h', d, 0x2C8); s.cs = struct.unpack_from('<35h', d, 0x30E)
        s.fol = d[0x11AF:0x11AF + 35]; s.rot = d[0x11D2:0x11D2 + 35]; s.hasFloor = d[0x11F5:0x11F5 + 35]
        s.hasDrop = d[0x1218:0x1218 + 35]; s.shape = d[0x123B:0x123B + 35]; s.ground = d[0x125E:0x125E + 35]
        s.first = d[0x174A:0x174A + 35]; s.last = d[0x176D:0x176D + 35]
    def floor(s, i): return s.pos[i][2]
    def tab(s, off, a, b, signed=True): return struct.unpack_from('b' if signed else 'B', s.d, off + 35 * a + b)[0]
    def next(s, a, b): return s.tab(0x354, a, b)
    def nsec(s, a, b): return s.tab(0x81D, a, b)
    def link(s, a, b): return s.tab(0xCE6, a, b)
    def speed(s, a, b): return s.tab(0x1281, a, b, False)
    def contains(s, i, x, y):
        return s.pos[i][0] <= x <= s.pos[i][0] + s.w[i] and s.pos[i][1] <= y <= s.pos[i][1] + s.l[i]
    def section_at(s, x, y, z):
        """worldSectionAt 800EFF34 (car_wheel_sway.c:39), line for line (C division truncates)."""
        def cdiv(a, b): q = abs(a) // abs(b); return q if (a >= 0) == (b > 0) else -q
        i = 0; r = s.count - 1
        if s.count:
            z += 80
            while i < s.count:
                dx = x - s.pos[i][0]; dz = y - s.pos[i][1]
                if s.rot[i]:
                    t2 = dx * s.cs[i] - dz * s.sn[i]
                    if t2 < 0: t2 += 4095
                    t = dz * s.cs[i] + dx * s.sn[i]
                    dx = t2 >> 12
                    if t < 0: t += 4095
                    dz = t >> 12
                flag = 0
                if dx >= 0 and dz >= 0 and s.w[i] >= dx and s.l[i] >= dz:
                    sh = s.shape[i]
                    if sh == 0: flag = 1
                    elif sh == 2: flag = 1 if cdiv(dx * s.l[i], s.w[i]) >= dz else 0
                    elif sh == 1:
                        hw = cdiv(s.w[i], 4); hl = cdiv(s.l[i], 2)
                        if dx < hw:
                            if hl < dz: flag = cdiv((dz - hl) * hw, hl) < dx
                            else: flag = cdiv((hl - dz) * hw, hl) < dx
                        elif dx < s.w[i] - hw: flag = 1
                        else:
                            dx -= cdiv(s.w[i] * 3, 4)
                            t = dz * hw if dz < hl else (hl - (dz - hl)) * hw
                            flag = dx < cdiv(t, hl)
                if flag:
                    r = i
                    if s.pos[i][2] < z: break
                i += 1
        return r
def pts(name):
    d = load(name, 'PTS'); out = []
    for i in range(len(d) // 24):
        x, y, z = struct.unpack_from('<iih', d, 24 * i); nx = struct.unpack_from('<4h', d, 24 * i + 10); ln = struct.unpack_from('<4b', d, 24 * i + 0x12)
        c, r = d[24 * i + 0x16], d[24 * i + 0x17]
        out.append(dict(id=i, x=x, y=y, z=z, succ=list(nx[:c]), lane=list(ln[:c]), section=r))
    while out and not (out[-1]['x'] or out[-1]['y'] or out[-1]['succ']): out.pop()
    return out
def rmp(name):
    d = load(name, 'RMP'); out = []
    for k in range(len(d) // 28):
        t, f1, sc, w, l, sn, cs, h0, h1, h2, h3, x, y = struct.unpack_from('<BBhhhhh4hii', d, 28 * k)
        out.append(dict(k=k, type=t, x=x, y=y, w=w, l=l, sn=sn, cs=cs, h=(h0, h1, h2, h3)))
    return out
