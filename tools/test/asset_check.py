"""The first-time setup's C readers (src/assets: disc images, folders, TIM / TPC / PCX) against tools/disc's Python:
every image of the PS1 disc image and of the PC version (its folder and its ISO), the same size and the same RGBA.

    python tools/test/asset_check.py [--exe build/net-sim/asset_check.exe]
"""
import argparse
import os
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'disc'))
from cdimage import CdImage  # noqa: E402
import pcx  # noqa: E402
import tim  # noqa: E402

PS1 = ROOT / 'references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue'
PC = ROOT / 'references/TM2-PC-Version/Game Files'
PC_ISO = PC.parent / 'TWISTEDMETAL2.iso'
EXTS = ('.TIM', '.TMS', '.TPC', '.PCX')


def python_lines(files):
    out = []
    for path, read in files:
        ext = os.path.splitext(path)[1].upper()
        if ext not in EXTS:
            continue
        data = read()
        if ext == '.PCX':
            try:
                p = pcx.parse(data)
            except ValueError:
                continue
            out.append('%s 0 %dx%d %08x' % (path, p.width, p.height, zlib.crc32(p.rgba()) & 0xFFFFFFFF))
            continue
        for k, (_, t) in enumerate(tim.scan(data, rgb=ext == '.TPC')):
            out.append('%s %d %dx%d %08x' % (path, k, t.width, t.height, zlib.crc32(t.rgba(0)) & 0xFFFFFFFF))
    return out


def disc_files(image):
    cd = CdImage(str(image))
    return [(f.path.split(';')[0].upper(), (lambda f=f: cd.read(f))) for f in cd.files if cd.is_data(f)]


def folder_files(folder):
    return [('/' + p.relative_to(folder).as_posix().upper(), p.read_bytes) for p in sorted(folder.rglob('*'))
            if p.is_file() and p.suffix.upper() != '.ISO']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=str(ROOT / 'build' / 'net-sim' / 'asset_check.exe'))
    a = ap.parse_args()
    ok = True
    sources = [('the PS1 disc (.cue)', str(PS1), lambda: disc_files(PS1)), ('the PC version (folder)', str(PC), lambda: folder_files(PC))]
    if PC_ISO.exists():
        sources.append(('the PC version (.iso)', str(PC_ISO), None))
    pc_lines = None
    for what, src, files in sources:
        c = subprocess.run([a.exe, src], capture_output=True, text=True).stdout.splitlines()
        c = sorted(l for l in c if not l.startswith('/TM2PC'))
        if files is None:  # (the ISO: each image the same as the installed folder's of that file)
            folder = {' '.join(l.split()[:2]): l for l in (pc_lines or [])}
            shared = [l for l in c if ' '.join(l.split()[:2]) in folder]
            bad = [l for l in shared if folder[' '.join(l.split()[:2])] != l]
            print('%-28s %5d images, %d also in the folder: %s' % (what, len(c), len(shared), 'the same' if shared and not bad else '%d differ' % len(bad)))
            ok &= bool(shared) and not bad
            continue
        py = sorted(python_lines(files()))
        if 'folder' in what:
            pc_lines = c
        diff = [x for x in py if x not in set(c)] + [x for x in c if x not in set(py)]
        print('%-28s %5d images, C %5d: %s' % (what, len(py), len(c), 'the same' if not diff else '%d differ' % len(diff)))
        for x in diff[:8]:
            print('    ', x)
        ok &= not diff and len(py) > 0
    print('all checks passed' if ok else 'SOME CHECKS FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
