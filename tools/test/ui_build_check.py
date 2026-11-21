"""The first-time setup's builder (src/assets/uibuild.c) against tools/art/ui_assets.py: every picture it writes the
same bytes as the Python's (build/ui-py, made by `python tools/art/ui_assets.py --out build/ui-py`). Not byte for byte:
the movie portraits (the game's own movie decoder, not ffmpeg's: each still close) and the Credits picture (new: the
credits' background, not the advertising art).

    python tools/test/ui_build_check.py [--pc iso|folder]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PS1 = ROOT / 'references/TM2-Decomp/cdimage/Twisted Metal 2 (USA).cue'
PCS = {'iso': ROOT / 'references/TM2-PC-Version/TWISTEDMETAL2.iso', 'folder': ROOT / 'references/TM2-PC-Version/Game Files'}


NEW = {'credits_portrait.bmp', 'setup.txt', 'hd_arena.bin'}
RETIRED = {'mode_icons.bmp', 'hud_hud.bmp', 'hud_list.bmp', 'hud_nohud.bmp'}  # (the port's own: art/mode_icons.png; the HUD preview: painted)


def movies_close(ref, out):
    """movie_portraits: every still within an average of 3 per color of the Python's (the image picks are new)."""
    sys.path.insert(0, str(ROOT / 'tools/art'))
    from movie_portraits import load_bmp
    a, b = load_bmp(ref / 'movie_portraits.bmp'), load_bmp(out / 'movie_portraits.bmp')
    picks = json.loads((ROOT / 'tools/art/movie_portraits.json').read_text(encoding='utf-8'))
    if len(a[0]) != len(b[0]) or len(a) != len(b):
        return False
    worst = 0.0
    for i, p in enumerate(picks):
        if p.get('image'):
            continue
        s = sum(abs(a[y][x][k] - b[y][x][k]) for y in range(0, 240, 2) for x in range(i * 120, i * 120 + 120, 2) for k in range(3))
        worst = max(worst, s / (120 * 60 * 3))
    print('movie stills: the worst average difference %.2f' % worst)
    index = lambda f: (f / 'movie_portraits.txt').read_text(encoding='utf-8').splitlines()[1:]
    return worst < 3 and index(ref) == index(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pc', default='iso', choices=sorted(PCS))
    ap.add_argument('--exe', default=str(ROOT / 'build' / 'net-sim' / 'ui_build_check.exe'))
    a = ap.parse_args()
    ref, out = ROOT / 'build' / 'ui-py', ROOT / 'build' / 'ui-c'
    if not ref.exists():
        subprocess.run([sys.executable, str(ROOT / 'tools/art/ui_assets.py'), '--out', str(ref)], check=True)
    for f in out.glob('*'):
        f.unlink()
    r = subprocess.run([a.exe, str(PS1), str(PCS[a.pc]), str(out)], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
    made = sorted(p.name for p in out.glob('*'))
    same = [n for n in made if (ref / n).exists() and (ref / n).read_bytes() == (out / n).read_bytes()]
    if 'movie_portraits.bmp' in made and movies_close(ref, out):
        same += ['movie_portraits.bmp', 'movie_portraits.txt']
    differ = [n for n in made if n not in same and n not in NEW]
    missing = sorted(p.name for p in ref.glob('*') if p.name not in made and p.name not in RETIRED)
    print('written %d: %d the same as the Python\'s (the movie stills close), %d new, %d differ %s' %
          (len(made), len(same), len(NEW & set(made)), len(differ), differ[:10]))
    print('not written yet (%d): %s' % (len(missing), ' '.join(missing)))
    return 0 if not differ and not r.returncode else 1


if __name__ == '__main__':
    sys.exit(main())
