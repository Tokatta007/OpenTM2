"""Online, the lockstep milestone (docs/ONLINE_PLAN.md 12 D): two copies of the hooks build on this PC, one hosting
(build/port-dev), one joining (build/port-dev2), over UDP on 127.0.0.1 (TM2PC_NET_LOCAL: no firewall prompt), a 2-player
Challenge Match ('two'); player 1's pad scripted on the host, player 2's on the joiner. The joiner starts from the
host's state (RAM, VRAM, texture banks, sound RAM), so its own menus may go another way (--join KEY=VAL: e.g.
TM2PC_SEED=99, other opponents). Then their state logs compared frame by frame from the battle's start, and the
screenshots.

    python tools/test/net_lockstep.py [--secs 120] [--delay 2] [--join KEY=VAL ...]

Copy build/port-hooks/tm2pc.exe to tm2pc-hooks.exe in build/port-dev and build/port-dev2 first."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
P1 = None  # (pad_x, below: compare_builds imported in each run)
P2 = '1750:cross+left,1950:-,2150:cross+right,2350:-,2550:circle,2570:-,2750:cross+up,2950:-'


def one(secs, env_pairs):
    """one copy (CB_DEV, CB_OUT set by the caller): the 'two' scenario with env_pairs; prints its folder"""
    sys.path.insert(0, str(HERE))
    import compare_builds as cb
    _, shot, env, extra = cb.SCENARIOS['two']
    env = dict(env)
    for kv in env_pairs:
        k, v = kv.split('=', 1)
        env[k] = v.replace('{P1}', cb.pad_x(1700, 3400))
    print(cb.run('tm2pc-hooks.exe', os.environ['NET_NAME'], 'b', secs, shot, env, extra))


def battle(lines):
    """the state log from the frame before the battle's first (the menus' frame counts may differ)"""
    lines = [l for l in lines if len(l.split()) == 3]  # (a run stopped mid-line)
    s = next((i for i, l in enumerate(lines) if l.split()[2] != lines[0].split()[2]), len(lines))
    return lines[max(s - 1, 0):]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secs', type=int, default=120)
    ap.add_argument('--delay', default='2')
    ap.add_argument('--join', nargs='*', default=[])
    ap.add_argument('--one', nargs='*')
    a = ap.parse_args()
    if a.one is not None:
        one(a.secs, a.one)
        return 0
    runs = []
    devs = os.environ.get('NET_DEVS', 'port-dev,port-dev2').split(',')  # (NET_DEVS: two other folders, e.g. while a suite runs)
    for dev, net, extra in ((devs[0], 'host:47011', ['TM2PC_PAD1_SCRIPT={P1}']),
                            (devs[1], 'join:127.0.0.1:47011', ['TM2PC_PAD2_SCRIPT=' + P2] + a.join)):
        e = dict(os.environ, CB_DEV=str(REPO / 'build' / dev), CB_OUT=str(REPO / 'build' / 'test-net'), NET_NAME=dev,
                 SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0')
        args = [sys.executable, __file__, '--secs', str(a.secs), '--one', 'TM2PC_NET=' + net, 'TM2PC_NET_LOCAL=1',
                'TM2PC_NET_DELAY=' + a.delay] + extra
        runs.append(subprocess.Popen(args, env=e, stdout=subprocess.PIPE, text=True))
    outs = [Path(p.communicate()[0].strip().splitlines()[-1]) for p in runs]
    for d in outs:
        print(d)
        for l in (d / 'log.txt').read_text(errors='replace').splitlines():
            if (l.startswith('network:') and 'waiting at' not in l) or l.startswith('runtime:'):
                print('  ', l)
    la, lb = [battle((d / 'state.txt').read_text().splitlines()) if (d / 'state.txt').exists() else [] for d in outs]
    n = min(len(la), len(lb))
    diff = [i for i in range(n) if la[i].split()[2] != lb[i].split()[2]]
    shots = [sorted(d.glob('vblank-*[0-9].bmp')) for d in outs]
    same_shots = bool(shots[0]) and [s.name for s in shots[0]] == [s.name for s in shots[1]] and all(
        x.read_bytes() == y.read_bytes() for x, y in zip(*shots))
    print('battle frames %d/%d, %s; shots %s' % (len(la), len(lb), 'equal' if n and not diff else
          'differ from %d' % diff[0] if diff else 'none', 'equal' if same_shots else 'DIFFER'))
    return 0 if n and not diff and same_shots else 1


if __name__ == '__main__':
    sys.exit(main())
