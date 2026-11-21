"""The updater (src/runtime/update.c), end to end on this PC: a release served from 127.0.0.1 (a GitHub-style "latest"
answer and its zip), a scratch copy of the game (build/update-test) that finds it, downloads it and swaps its files;
the player's own files untouched; the next start removes what was set aside.

    python tools/test/update_test.py        (needs build/port-hooks/tm2pc.exe)
"""
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import threading
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXE = REPO / 'build' / 'port-hooks' / 'tm2pc.exe'
GAME = REPO / 'build' / 'update-test'
SERVE = REPO / 'build' / 'update-test-srv'


def main():
    for d in (GAME, SERVE):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    shutil.copy2(EXE, GAME / 'tm2pc.exe')
    shutil.copytree(REPO / 'assets' / 'fonts', GAME / 'fonts')
    (GAME / 'tm2pc.ini').write_text('KEEP-ME\n')
    (GAME / 'profiles').mkdir()
    (GAME / 'profiles' / 'p.txt').write_text('KEEP\n')
    exe_bytes = EXE.read_bytes()
    z = io.BytesIO()
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as w:  # (one folder, as a release's zip would be)
        w.writestr('TM2PC-v0.9.1/tm2pc.exe', exe_bytes)
        w.writestr('TM2PC-v0.9.1/ui/marker.txt', 'v0.9.1\n')
        w.writestr('TM2PC-v0.9.1/fonts/new.txt', 'a new file\n')
        w.writestr('TM2PC-v0.9.1/tm2pc.ini', 'OVERWRITTEN\n')         # (the player's: never written)
        w.writestr('TM2PC-v0.9.1/profiles/p.txt', 'OVERWRITTEN\n')
    (SERVE / 'r.zip').write_bytes(z.getvalue())

    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), lambda *a: http.server.SimpleHTTPRequestHandler(*a, directory=str(SERVE)))
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:%d/' % port
    for name, tag in (('newer.json', 'v0.9.1'), ('same.json', 'v0.9.0')):
        (SERVE / name).write_text(json.dumps({'tag_name': tag, 'assets': [
            {'name': 'notes.txt', 'browser_download_url': base + 'notes.txt'},
            {'name': 'TM2PC-%s-win64.zip' % tag, 'browser_download_url': base + 'r.zip'}]}))

    def run(url, auto):
        env = dict(os.environ, TM2PC_UPDATE_URL=base + url, TM2PC_UPDATE_WAIT='1', SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0')
        if auto:
            env['TM2PC_UPDATE_AUTO'] = '1'
        p = subprocess.run([str(GAME / 'tm2pc.exe'), '--settings', str(GAME / 'test.ini'), '--ui-shot',
                            '0,0,640x480,%s' % (GAME / 'shot.bmp')], cwd=GAME, env=env, capture_output=True, text=True, timeout=120)
        log = (GAME / 'tm2pc.log').read_text(errors='replace') if (GAME / 'tm2pc.log').exists() else p.stderr
        return [l for l in log.splitlines() if l.startswith('update:')]

    ok = True

    def check(what, cond):
        nonlocal ok
        ok &= bool(cond)
        print('%-62s %s' % (what, 'ok' if cond else 'FAILED'))

    log = run('newer.json', True)
    print('\n'.join('  ' + l for l in log))
    check('the newer release found', any('an update' in l for l in log))
    check('its files in place', any('in place' in l for l in log))
    check('the new file (ui/marker.txt)', (GAME / 'ui' / 'marker.txt').exists() and (GAME / 'ui' / 'marker.txt').read_text() == 'v0.9.1\n')
    check('the new executable', (GAME / 'tm2pc.exe').read_bytes() == exe_bytes)
    check('the old executable set aside (tm2pc.exe.old)', (GAME / 'tm2pc.exe.old').exists())
    check('the player\'s tm2pc.ini untouched', (GAME / 'tm2pc.ini').read_text() == 'KEEP-ME\n')
    check('the player\'s profiles untouched', (GAME / 'profiles' / 'p.txt').read_text() == 'KEEP\n')
    check('no file left half-done (*.new)', not list(GAME.rglob('*.new')))
    log = run('same.json', False)
    print('\n'.join('  ' + l for l in log))
    check('the next start: no update (same version)', not any('an update' in l for l in log))
    check('the next start: the old executable removed', not (GAME / 'tm2pc.exe.old').exists())

    # before the game (update_pregame): asked, "Yes", the window's bar, then the new version would start
    (GAME / 'ui' / 'marker.txt').unlink()
    env = dict(os.environ, TM2PC_UPDATE_URL=base + 'newer.json', TM2PC_UPDATE_ANSWER='yes', TM2PC_UPDATE_NO_RESTART='1',
               TM2PC_UPDATE_SHOT=str(GAME / 'window.bmp'), SDL_AUDIO_DRIVER='dummy', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SDL_WINDOW_ACTIVATE_WHEN_RAISED='0')
    p = subprocess.run([str(GAME / 'tm2pc.exe'), '--settings', str(GAME / 'test.ini'), '--ui-shot', '0,0,640x480,%s' % (GAME / 'shot.bmp')],
                       cwd=GAME, env=env, capture_output=True, text=True, timeout=120)
    log = [l for l in ((GAME / 'tm2pc.log').read_text(errors='replace') if (GAME / 'tm2pc.log').exists() else p.stderr).splitlines()
           if l.startswith('update:')]
    print('\n'.join('  ' + l for l in log))
    check('before the game: updated, then the restart', any('would restart' in l for l in log) and (GAME / 'ui' / 'marker.txt').exists())
    check('before the game: its window drawn', (GAME / 'window.bmp').exists())
    srv.shutdown()
    print('all checks passed' if ok else 'SOME CHECKS FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
