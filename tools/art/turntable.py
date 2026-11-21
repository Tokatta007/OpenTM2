"""turntable.py: each car turned a whole turn in 1-player car select, a picture per step, then a GIF of each.

    python tools/art/turntable.py OUT [--cars 0-14] [--steps 90] [--size 1920x1080] [--fps 30] [--crop W:H:X:Y]

Runs the dev build (build/port-dev/tm2pc-hooks.exe, compare_builds' scripted menus, silent, unfocused) once, through
every car, with TM2PC_TURNTABLE (ui/options.c: the car held at each heading on the road and flames alone, no portrait, titles, emblem, arrows,
name, portraits' row or legend; the frames saved),
then ffmpeg makes OUT/NN_name.gif (a palette per car; with --mp4 also OUT/NN_name.mp4) from them; the BMPs are deleted after.
--crop takes ffmpeg's crop (W:H:X:Y of the full picture); without it the whole wide picture.
Car numbers: 0 Hammerhead, 1 Outlaw 2, 2 Warthog, 3 Mr. Grimm, 4 Grasshopper, 5 Thumper, 6 Spectre, 7 Roadkill,
8 Twister, 9 Axel, 10 Mr. Slam, 11 Shadow, 12 Sweet Tooth, 13 Minion, 14 Dark Tooth.
"""
import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools' / 'test'))
import compare_builds as cb  # noqa: E402

NAMES = ['hammerhead', 'outlaw2', 'warthog', 'mr_grimm', 'grasshopper', 'thumper', 'spectre', 'roadkill', 'twister', 'axel',
         'mr_slam', 'shadow', 'sweet_tooth', 'minion', 'dark_tooth']


def cars(spec):
    out = []
    for part in spec.split(','):
        a, _, b = part.partition('-')
        out += list(range(int(a), int(b or a) + 1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--cars', default='0-14')
    ap.add_argument('--steps', type=int, default=90)
    ap.add_argument('--size', default='1920x1080')
    ap.add_argument('--fps', type=int, default=30)
    ap.add_argument('--crop', default='')
    ap.add_argument('--keep', action='store_true', help='keep the BMP frames')
    ap.add_argument('--mp4', action='store_true', help='an MP4 of each too (full size)')
    ap.add_argument('--far', default='', help='car:percent,... those cars further back (Minion whole in a wide crop)')
    ap.add_argument('--gif-size', default='', help='WxH: each GIF scaled to this (after --crop), as 920x400')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    want = set(cars(a.cars))
    frames = out / 'frames'
    frames.mkdir(exist_ok=True)
    for f in frames.glob('*.bmp'):
        f.unlink()
    # one run: the carousel from its first car, Right after each, all the way round (Sweet Tooth, Minion and Dark Tooth
    # unlocked); about (220 + steps * 6) VBlanks a car
    env = dict(TM2PC_UI_SCRIPT=cb.UI_ONE, TM2PC_SIGNIN_ONE='1', TM2PC_UNLOCK_ALL='1', TM2PC_TURNTABLE=str(frames),
               TM2PC_TURNTABLE_STEPS=str(a.steps), TM2PC_TEST_WINDOW=a.size,
               TM2PC_NO_PRECISE_C='0')  # (the game's own subpixel sprites, as players see: Mr. Grimm's helmet is one)
    if a.far:
        env['TM2PC_TURNTABLE_FAR'] = a.far
    d = cb.run('tm2pc-hooks.exe', 'turntable', 'all', 90 + 18 * (220 + a.steps * 6) // 60, 999999, env, {})
    for car in sorted(want):
        got = sorted(frames.glob('turn_%02d_*.bmp' % car))
        print('car %d %s: %d pictures' % (car, NAMES[car] if car < len(NAMES) else car, len(got)), flush=True)
        if len(got) < a.steps:
            print('  incomplete: see %s' % (d / 'log.txt'))
            continue
        name = '%02d_%s' % (car, NAMES[car] if car < len(NAMES) else 'car')
        src = str(got[0]).replace('_000.bmp', '_%03d.bmp')
        vf = ('crop=%s,' % a.crop if a.crop else '') + ('scale=%s:flags=lanczos' % a.gif_size.replace('x', ':') if a.gif_size
                                                         else 'scale=iw/2:-1:flags=lanczos')
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', str(a.fps), '-i', src,
                        '-vf', vf + ',split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=sierra2_4a',
                        '-loop', '0', str(out / (name + '.gif'))], check=True)
        if a.mp4:
            subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', str(a.fps), '-i', src,
                            '-vf', ('crop=%s,' % a.crop if a.crop else '') + 'format=yuv420p', '-c:v', 'libx264', '-crf', '16',
                            str(out / (name + '.mp4'))], check=True)
    if not a.keep:
        for f in frames.glob('*.bmp'):
            f.unlink()
        frames.rmdir()

if __name__ == '__main__':
    main()
