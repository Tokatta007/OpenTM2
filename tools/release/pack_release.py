"""A release's zip (docs/RELEASE.md): the port's own files from build/port, in one folder named for the version
(CMakeLists.txt TM2PC_VERSION), as the updater (src/runtime/update.c) puts them next to the executable.

    python tools/release/pack_release.py        -> build/release/OpenTM2-<version>-win64.zip

Only what the port made: tm2pc.exe, fonts/ (the menu fonts, SIL OFL; flexure.atlas by permission of T26, its notice beside it) and art/ (the mode symbols, CC BY 3.0). Never the game's art or anything made from it
(ui/, extracted/), nor a player's files (tm2pc.ini, profiles/, music/, mods/, screenshots/)."""
import re
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PORT = REPO / 'build' / 'port'
SHIP = ['tm2pc.exe', 'fonts', 'art']


def main():
    m = re.search(r'set\(TM2PC_VERSION "([^"]+)"\)', (REPO / 'CMakeLists.txt').read_text())
    if not m:
        sys.exit('no TM2PC_VERSION in CMakeLists.txt')
    ver = m.group(1)
    out = REPO / 'build' / 'release' / ('OpenTM2-%s-win64.zip' % ver)
    out.parent.mkdir(parents=True, exist_ok=True)
    top = 'OpenTM2-%s/' % ver
    n = 0
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in SHIP:
            p = PORT / name
            if not p.exists():
                sys.exit('missing: %s (build build/port first)' % p)
            for f in [p] if p.is_file() else sorted(q for q in p.rglob('*') if q.is_file()):
                z.write(f, top + f.relative_to(PORT).as_posix())
                n += 1
    print('%s: %d files, %d KB' % (out, n, out.stat().st_size // 1024))


if __name__ == '__main__':
    main()
