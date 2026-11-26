"""Online through Network Play's menus (docs/NETWORK_MENUS.md): two copies of the hooks build on this PC over UDP on
127.0.0.1 (TM2PC_NET_LOCAL: no firewall prompt). build/port-dev hosts (LAN, Host Server, Create server) and presses
Start once the other is in; build/port-dev2 joins by address (TM2PC_NET_JOIN_ADDR) and readies. The match on both
(the joiner from the host's state, then lockstep), each one's pad 1 scripted; player 2 and the computer cars destroyed
(the match ends); X through the results: both back in the lobby. Then their battles compared frame by frame.

    python tools/test/net_lobby.py [--secs 150] [--lan] [--playlist] [--players N]
    (--lan: the joiner finds the server in the LAN browser; --playlist: a playlist of 4, both vote card 2 after the
    first match, the next match the same on both; --players N: N copies, build/port-dev, port-dev2..N (each a copy of
    port-dev2's fonts, profiles, ui, tm2pc.ini and the hooks exe), one player each: every battle against the host's)

Copy build/port-hooks/tm2pc.exe to tm2pc-hooks.exe in build/port-dev and build/port-dev2 first."""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
W = lambda n: ',-' * n
HOST = '-,-,-,down,-,x,-,-,-,-,down,-,x,-,-,-,-,up,-,x' + W(120) + ',start' + W(100) + ',-,-,-,x' * 100
JOIN = '-,-,-,down,-,x,-,-,-,-,down,-,down,-,x' + W(40) + ',start' + W(100) + ',-,-,-,x' * 100
JOIN_LAN = W(30) + ',-,-,-,down,-,x,-,-,-,-,x' + W(40) + ',x' + W(40) + ',start' + W(100) + ',-,-,-,x' * 100  # (the LAN browser)
P1 = ','.join('%d:cross,%d:-' % (v, v + 8) for v in range(3000, 6000, 100))
P2 = '3050:cross+left,3250:-,3450:cross+right,3650:-,3850:circle,3870:-,4050:cross+up,4250:-'
KILLS = '2@2500,' + ','.join('%d@%d' % (50 + k, 2550 + 10 * k) for k in range(9))


def one(secs, script, pad):
    sys.path.insert(0, str(HERE))
    import compare_builds as cb
    _, _, env, extra = cb.SCENARIOS['net_host']
    if os.environ.get('NET_PAD_PRE'):  # (presses before the script's own: --machine-env K:NET_PAD_PRE=2700:start,2708:-)
        pad = os.environ['NET_PAD_PRE'] + ',' + pad
    env = dict(env, TM2PC_NET_SCRIPT=script, TM2PC_PAD1_SCRIPT=pad, TM2PC_NET_LOCAL='1', TM2PC_NET_PORT='47200',
               TM2PC_NET_JOIN_ADDR='127.0.0.1:47200', TM2PC_TEST_NO_OWN_PAGE='1',  # (the scripts' X presses in the lobby: no player page)
               TM2PC_TEST_KILL='' if os.environ.get('NET_KILLS') == 'none' else os.environ.get('NET_KILLS') or KILLS,
               TM2PC_NET_NOTICE_SHOT=os.environ['CB_DEV'])  # (the network's notices: a BMP each, in the copy's folder)
    if os.environ.get('NET_PLAYLIST'):
        env.update(TM2PC_NET_PLAYLIST='4', TM2PC_NET_AUTOVOTE='1')
    extra = dict(extra, CBTest={'battle': ['lives = 1']})
    if os.environ.get('NET_TIMED'):  # (a machine quits or comes in at a set time: the match at once, no countdown)
        env['TM2PC_NET_COUNTDOWN'] = '0'
    if os.environ.get('NET_VIDEO'):  # (other [video] lines: NET_VIDEO="textures = playstation;...")
        extra['ini'] = list(extra.get('ini', ())) + [l.strip() for l in os.environ['NET_VIDEO'].split(';')]
    print(cb.run('tm2pc-hooks.exe', os.environ['NET_NAME'], 'b', secs, os.environ.get('NET_SHOTS', '5000v'), env, extra))  # (NET_SHOTS: other shots)


def battle(lines):
    lines = [l for l in lines[:-1] if len(l.split()) == 3]  # (the last line: a run stopped as it wrote it)
    s = next((i for i, l in enumerate(lines) if l.split()[2] != lines[0].split()[2]), len(lines))
    return lines[max(s - 1, 0):]


def realigned(ha, hb):
    """the first frame of ha (index) whose state hb never reaches, or None: between battles the copies wait for their own
    players (the results, the vote, a tournament's menus) and the frames there differ in number; at a difference, the
    other's log realigned on the same state further on, once per gap"""
    i = j = 0
    while i < len(ha) and j < len(hb):
        if ha[i] == hb[j]:
            i += 1; j += 1; continue
        k = next((k for k in range(j, min(len(hb), j + 3000)) if hb[k] == ha[i]), -1)
        if k >= 0: j = k; continue
        k = next((k for k in range(i, min(len(ha), i + 3000)) if ha[k] == hb[j]), -1)
        if k >= 0: i = k; continue
        # (a gap on both sides: a battle's first steps, which the host ran before the others took its state, they never
        # had, and their own before the state the host never had: on from the first state both have again)
        later = {h: n for n, h in reversed(list(enumerate(hb[j:j + 3000], j)))}
        k = next((k for k in range(i, min(len(ha), i + 3000)) if ha[k] in later), -1)
        if k >= 0 and k - i <= 200: i, j = k, later[ha[k]]; continue
        return i
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--secs', type=int, default=150)
    ap.add_argument('--one', nargs=2)
    ap.add_argument('--lan', action='store_true')
    ap.add_argument('--playlist', action='store_true')
    ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--quit-last', type=int, default=0, help='the last machine quits after this many seconds (a player leaving)')
    ap.add_argument('--late', type=int, default=0, help='the last machine joins this many seconds late (into a match on)')
    ap.add_argument('--dropin', action='store_true', help='with --late: the late machine drops into the match on (a CPU / OPEN '
                    "car's place); without it, it waits for the next match")
    ap.add_argument('--rejoin', type=int, default=0, help='with --quit-last: another copy joins this many seconds in (the same name: back in)')
    ap.add_argument('--machine-env', action='append', default=[], help='K:VAR=VALUE, an environment variable for machine K (1 the host)')
    ap.add_argument('--game', type=int, default=0, help="the server's game (1 Co-op Tournament, 2 Survival)")
    ap.add_argument('--migrate-start', action='store_true', help='with --quit-host: the new host starts the match (tests hook)')
    ap.add_argument('--quit-host', type=int, default=0, help='the host quits after this many seconds (another machine takes over)')
    ap.add_argument('--split', action='append', type=int, default=[], help='machine K a split screen of two players (its player 2 '
                    'signed in and through car select; with --machine-env K:TM2PC_TEST_SPECTATE=2 a spectator)')
    a = ap.parse_args()
    if a.dropin and not os.environ.get('NET_KILLS'):  # (the computer cars out after the newcomer is in; it too, the match ends)
        os.environ['NET_KILLS'] = '2@6000,' + ','.join('%d@%d' % (50 + k, 6100 + 10 * k) for k in range(12)) + ',3@6400'
    if a.late and not a.dropin:  # (the late machine waits for the next match, as in a game no one may drop into)
        os.environ['TM2PC_NET_NO_DROPIN'] = '1'
    if a.game == 2 and not os.environ.get('NET_KILLS'):  # (Survival: both players out later, so the run ends in time)
        os.environ['NET_KILLS'] = KILLS + ',1@4200,2@4200'
    if a.one:
        one(a.secs, *a.one)
        return 0
    runs = []
    pads = [P1, P2, '3100:cross+right,3300:-,3500:square,3520:-,3700:cross+left,3900:-']
    host_script = HOST.replace(W(120), W(120 + 60 * (a.players - 2) + 100 * len(a.split)), 1)  # (the host waits for every
    # machine; a split screen's longer: its two players' car select in a team game)
    for k in range(a.players):
        dev = 'port-dev' if k == 0 else 'port-dev%d' % (k + 1)
        script, pad = (host_script, P1) if k == 0 else (JOIN_LAN if a.lan else JOIN, pads[k % 3])
        if a.game:  # (a tournament's menus between levels, Survival's results: X on and on)
            script += ',-,-,-,x' * 400
        elif k + 1 in a.split:  # (a split screen: the host starts later, its results still to go through)
            script += ',-,-,-,x' * 200
        if a.late and k == a.players - 1:  # (0.2 s a script step: its Join that much later)
            script = W(a.late * 5) + ',' + script
        e = dict(os.environ, CB_DEV=str(REPO / 'build' / dev), CB_OUT=str(REPO / 'build' / 'test-net'), NET_NAME='lobby_' + dev,
                 SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0', NET_PLAYLIST='1' if a.playlist else '',
                 NET_TIMED='1' if a.quit_last or a.quit_host or a.late else '')
        if k + 1 in a.split:  # (a split screen PC: player 2 signs in, and picks a car whenever car select opens: a team
            # game's X on a player's card opens it, and with player 2's choice never made the PC stayed there while the
            # host played the match without it)
            e.update(TM2PC_P2_SCRIPT='-,x', TM2PC_CARSEL_P2_SCRIPT='-,-,-,-,-,-,x',
                     TM2PC_PAD2_SCRIPT=','.join('%d:cross,%d:-' % (v, v + 8) for v in range(1000, 12000, 100)))
        for me in a.machine_env:
            mk, kv = me.split(':', 1)
            if int(mk) == k + 1:
                e[kv.split('=', 1)[0]] = kv.split('=', 1)[1].replace('{DEV}', str(REPO / 'build' / dev))
        if a.game and k == 0:
            e['TM2PC_NET_GAME'] = str(a.game)
        if a.migrate_start and k > 0:
            e['TM2PC_NET_TEST_MIGRATE_START'] = '1'
        # (a late machine: the same kills; which have fired comes with the host's state)
        secs = a.quit_last if a.quit_last and k == a.players - 1 else a.quit_host if a.quit_host and k == 0 else a.secs
        runs.append(subprocess.Popen([sys.executable, __file__, '--secs', str(secs), '--one', script, pad], env=e,
                                     stdout=subprocess.PIPE, text=True))
    if a.rejoin:  # (the dropped player back: another copy, the same name, its Join that much later)
        dev = 'port-dev%d' % (a.players + 1)
        e = dict(os.environ, CB_DEV=str(REPO / 'build' / dev), CB_OUT=str(REPO / 'build' / 'test-net'), NET_NAME='lobby_' + dev,
                 SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0')  # (the same kills: which have fired comes with the host's state)
        runs.append(subprocess.Popen([sys.executable, __file__, '--secs', str(a.secs), '--one', W(a.rejoin * 5) + ',' + JOIN, pads[0]],
                                     env=e, stdout=subprocess.PIPE, text=True))
    outs = [Path(p.communicate()[0].strip().splitlines()[-1]) for p in runs]
    if a.rejoin:  # (back in: told, and its battle the host's from where it came in)
        back = outs.pop()
        log = (back / 'log.txt').read_text(errors='replace').splitlines()
        print(back)
        for l in log:
            if l.startswith('network') and 'lobby ' not in l: print('  ', l[:160])
        host = [l.split()[2] for l in (outs[0] / 'state.txt').read_text().splitlines()[:-1] if len(l.split()) == 3]
        mine = [l.split()[2] for l in battle((back / 'state.txt').read_text().splitlines())] if (back / 'state.txt').exists() else []
        k = min(200, len(mine) - 1)  # (its first frames: its own battle's start, before the host's state came)
        at = host.index(mine[k]) if mine and k > 0 and mine[k] in host else -1
        total = min(len(mine) - k, len(host) - at) if at >= 0 else 0
        run = next((i for i in range(total) if mine[k + i] != host[at + i]), total)
        same = at >= 0 and run >= min(200, total) and total > 0 and not any('DESYNC' in (o_ / 'log.txt').read_text(errors='replace') for o_ in outs)
        print("back in: battle frames %d, %s the host's" % (len(mine), 'equal to' if same else 'NOT equal to'))
        ok_back = same and any('back into the match' in l for l in log)
        if not ok_back: return 1
    ok = 1
    late = len(outs) - 1 if a.late else -1
    for m, d in enumerate(outs):
        log = (d / 'log.txt').read_text(errors='replace').splitlines()
        print(d)
        for l in log:
            if (l.startswith('network') and 'waiting at' not in l and 'servers asked for' not in l) or l.startswith('runtime:') \
                    or 'WINS' in l:
                print('  ', l[:160])
        quitter = a.quit_last and m == len(outs) - 1  # (it left the match on purpose: not over for it)
        ok &= (m == 0 and a.quit_host > 0 or quitter or any('the match is over' in l for l in log)) and \
            not any('DESYNC' in l for l in log)
        if m == late and a.dropin:  # (came in while a match was on: dropped into it)
            ok &= any('drops in' in l for l in log)
        elif m == late:  # (came in while a match was on: told so, and played the next one only)
            ok &= any('A match is on' in l for l in log) and sum('the match starts' in l for l in log) == 1
        if a.quit_host and m > 0:  # (the host gone: every other machine saw another take over)
            ok &= any('is the host now' in l for l in log)
        if a.playlist and m != late:
            starts = [l for l in log if 'the match starts' in l]
            ok &= len(starts) >= 2
            print('   matches started:', [s.split('(')[1].rstrip(')') for s in starts])
    if a.late:  # (the late machine's battle: the host's second, frame for frame)
        host = [l.split()[2] for l in (outs[0] / 'state.txt').read_text().splitlines()[:-1] if len(l.split()) == 3]
        mine = [l.split()[2] for l in battle((outs[late] / 'state.txt').read_text().splitlines())] if (outs[late] / 'state.txt').exists() else []
        k = min(200, len(mine) - 1)  # (aligned on a frame well into the battle: the first ones may be menus' alike)
        at = host.index(mine[k]) - k if mine and mine[k] in host else -1
        at = at if at >= 0 else -1
        n = min(len(mine), len(host) - at) if at >= 0 else 0
        # (its first: the menu's frame before; its final kill cam replays what it recorded itself, less than the host
        # did: a few hundred frames the same, and no desync anywhere, is the test)
        run = next((i for i in range(1, n) if mine[i] != host[at + i]), n)
        if a.dropin and mine and mine[k] in host:  # (dropped in: its own load first, the host's from its state on)
            at = host.index(mine[k])
            n = min(len(mine) - k, len(host) - at)
            run = next((i for i in range(n) if mine[k + i] != host[at + i]), n)
        same = run >= min(300, n - 1) and not any('DESYNC' in (o_ / 'log.txt').read_text(errors='replace') for o_ in outs)
        print("late machine: battle frames %d, %s the host's from its frame %d" % (len(mine), 'equal to' if same else 'NOT equal to', at))
        ok &= same
        return 0 if ok else 1
    if a.playlist:  # (the same matches on both; and every machine voted: the joiners' vote screen too)
        ok &= [l for l in (outs[0] / 'log.txt').read_text(errors='replace').splitlines() if 'the match starts' in l] ==               [l for l in (outs[1] / 'log.txt').read_text(errors='replace').splitlines() if 'the match starts' in l]
        for i, d in enumerate(outs):
            voted = 'voted for' in (d / 'log.txt').read_text(errors='replace')
            print('machine %d %s' % (i + 1, 'voted' if voted else 'never saw the vote'))
            ok &= voted
    logs = [battle((d / 'state.txt').read_text().splitlines()) if (d / 'state.txt').exists() else [] for d in outs]
    if a.split:  # (a split screen PC: each machine's final kill cam its own, its views two; the first battle compared, to
        # its final kill cam's start: the VBlank its log line names)
        import re
        for i, d in enumerate(outs):
            m = re.search(r'final kill cam: replaying frames \d+\.\.\d+ \(at VBlank (\d+)\)', (d / 'log.txt').read_text(errors='replace'))
            if m: logs[i] = [l for l in logs[i] if int(l.split()[1]) < int(m.group(1))]
    if a.players > 2:  # (every machine's battle against the host's; the host gone: against the next machine's)
        ref = 1 if a.quit_host else 0
        for k in range(ref + 1, len(logs) - (1 if a.quit_last else 0)):
            a0, b0 = [l.split()[2] for l in logs[ref]], [l.split()[2] for l in logs[k]]
            bad = realigned(a0, b0) if a0 and b0 else 0
            print('machine %d against %d: battle frames %d/%d, %s' % (k + 1, ref + 1, len(a0), len(b0),
                                                                    'equal' if bad is None else 'differ from %d' % bad))
            ok &= bad is None
        return 0 if ok else 1
    la, lb = logs
    # (between battles the copies wait for their own players: the results, the vote; the frames there differ in number:
    # at a difference, the other's log realigned on the same state further on, once per gap)
    ha, hb = [l.split()[2] for l in la], [l.split()[2] for l in lb]
    bad = realigned(ha, hb)
    diff = [] if bad is None else [bad]
    n = min(len(la), len(lb))
    print('battle frames %d/%d, %s; %s' % (len(la), len(lb), 'equal' if n and not diff else 'differ from %d' % diff[0] if diff
          else 'none', 'both back in the lobby, no desync' if ok else 'NOT BOTH BACK, or a desync'))
    return 0 if ok and n and not diff else 1


if __name__ == '__main__':
    sys.exit(main())
