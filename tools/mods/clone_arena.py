"""Make an arena mod from one of your own disc's arenas: a start for a new arena, and the test of the mod-arena loader
(the clone should play as the level it came from, without that level's scripted events).

    python tools/mods/clone_arena.py <the PS1 disc's track 1 .bin> <level 1-12> <mods/arenas/<id> folder>

The files come from your disc and stay on your PC: never put disc files in the repository or share them.
mod.ini is made by the game (tm2pc --new-mod arena <id>) or here when missing (a test uid).
"""
import os
import struct
import sys

SECTOR, DATA = 2352, 24  # Mode 2 Form 1: 2048 bytes of user data after a 24-byte header

# level: (file stem, terrain stem, name, sky (r, g, b), route points, start points, GRP sections, RMP surfaces, RMP extra)
# (the decomp's arenaLoad<Name> functions: ai_route_loaders.c, ai_start_nodes.c, terrain_load*.c, arena_load_*.c)
LEVELS = {
    7: ('DEN', 'DEN', 'Holland Clone', (0x8F, 0x6F, 0x50), 58,
        [0, 2, 13, 15, 17, 19, 21, 25, 27, 30, 34, 43, 45, 46, 47], 7, 8, 0),
}
# the rest of each level's arena.ini: its teleporters (worldWarpsSetup), the computer cars' exits (aiWarpSetExitNode:
# side 0, else sides 1 and 2), its hidden health spot (carTick: the square's low corner)
EXTRA = {
    7: ['warp1 = 4800, 4600, 24, 4072, 3960, 520, 2', 'warp2 = 4752, -4960, 24, 4112, -4160, 520, 2',
        'warp1_exit = 46, 9', 'warp2_exit = 47, 38', 'warp3_exit = 47, 38', 'health_spot = -1168, -6200'],
}


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def sector(self, lba):
        self.f.seek(lba * SECTOR + DATA)
        return self.f.read(2048)

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:size])

    def entries(self, lba, size):
        d = self.read(lba, size)
        i = 0
        while i < len(d):
            n = d[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            ext, sz = struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0]
            flags, ln = d[i + 25], d[i + 32]
            name = d[i + 33:i + 33 + ln].decode('ascii', 'replace').split(';')[0]
            yield name, ext, sz, bool(flags & 2)
            i += n

    def find(self, path):
        pvd = self.sector(16)
        assert pvd[1:6] == b'CD001', 'not an ISO 9660 data track'
        lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
        for part in path.split('\\'):
            for name, e, s, is_dir in self.entries(lba, size):
                if name.upper() == part.upper():
                    lba, size = e, s
                    break
            else:
                raise SystemExit(f'{path}: not on the disc')
        return self.read(lba, size)


def write_bmp(path, w, h, rgb):
    row = (w * 3 + 3) & ~3
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', 54 + row * h, 0, 0, 54))
        f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, row * h, 2835, 2835, 0, 0))
        line = bytes((rgb[2], rgb[1], rgb[0])) * w + b'\0' * (row - w * 3)
        f.write(line * h)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    disc, level, out = Disc(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    if level not in LEVELS:
        raise SystemExit(f'level {level}: not in this tool yet (it has {sorted(LEVELS)})')
    stem, tstem, name, sky, points, starts, sections, surfaces, extra = LEVELS[level]
    os.makedirs(out, exist_ok=True)
    files = {'arena.dmd': f'DMD\\{stem}.DMD', 'arena.tms': f'TMS\\{stem}.TMS', 'arena.pts': f'TERRAIN\\{tstem}.PTS',
             'arena.grp': f'TERRAIN\\{tstem}.GRP', 'arena.rmp': f'TERRAIN\\{tstem}.RMP'}
    for mine, theirs in files.items():
        data = disc.find(theirs)
        open(os.path.join(out, mine), 'wb').write(data)
        print(f'{mine}: {theirs}, {len(data)} bytes')
    with open(os.path.join(out, 'arena.ini'), 'w', newline='\n') as f:
        f.write('[arena]\n')
        f.write(f'name = {name}\n')
        f.write(f'sky = {sky[0]}, {sky[1]}, {sky[2]}\n')
        f.write(f'points = {points}\n')
        f.write(f'starts = {", ".join(map(str, starts))}\n')
        f.write(f'sections = {sections}\n')
        f.write(f'surfaces = {surfaces}\n')
        if extra:
            f.write(f'surfaces_extra = {extra}\n')
        for line in EXTRA.get(level, []):
            f.write(line + '\n')
    if not os.path.exists(os.path.join(out, 'card.bmp')):
        write_bmp(os.path.join(out, 'card.bmp'), 344, 260, sky)
    if not os.path.exists(os.path.join(out, 'lineup.bmp')):  # (the line-up's picture: the disc's SCREEN\SHOWOPS<level>.TIM)
        tim = disc.find('SCREEN\\SHOWOPS%d.TIM' % level)
        w, h = struct.unpack_from('<HH', tim, 16)
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                v = struct.unpack_from('<H', tim, 20 + 2 * (y * w + x))[0]
                r, g, b = (v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3
                row += bytes((b, g, r))
            rows.append(bytes(row))
        with open(os.path.join(out, 'lineup.bmp'), 'wb') as f:
            pad = (4 - (w * 3) % 4) % 4
            f.write(b'BM' + struct.pack('<IHHI', 54 + (w * 3 + pad) * h, 0, 0, 54))
            f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, (w * 3 + pad) * h, 2835, 2835, 0, 0))
            for row in reversed(rows):
                f.write(row + b'\0' * pad)
        print(f'lineup.bmp: SCREEN\\SHOWOPS{level}.TIM, {w} x {h}')
    ini = os.path.join(out, 'mod.ini')
    if not os.path.exists(ini):
        mid = os.path.basename(os.path.normpath(out))
        with open(ini, 'w', newline='\n') as f:
            f.write(f'[mod]\nid = {mid}\nname = {name}\nkind = arena\nuid = {os.urandom(16).hex()}\ngame_version = 1\n')
    print(f'{out}: done')


if __name__ == '__main__':
    main()
