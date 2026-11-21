"""The master server (tools/master) with the lobby (tools/net_sim/lobby_test): the master started on 127.0.0.1, then
lobby_test's checks with TM2PC_TEST_MASTER set (a host listed, found, joined through it; after a host migration the new
host listed). Build build/net-sim first.

    python tools/test/master_test.py"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / 'build' / 'net-sim'
env = dict(os.environ, TM2PC_NET_LOCAL='1')
master = subprocess.Popen([str(BIN / 'tm2_master.exe'), '47300'], env=env, stderr=subprocess.PIPE, text=True)
backup = subprocess.Popen([str(BIN / 'tm2_master.exe'), '47301'], env=env, stderr=subprocess.PIPE, text=True)  # (two: a main, a backup)
try:
    r = subprocess.run([str(BIN / 'lobby_test.exe')], env=dict(env, TM2PC_TEST_MASTER='127.0.0.1:47300, 127.0.0.1:47301'), capture_output=True,
                       text=True, timeout=180)
    print(r.stdout)
finally:
    master.kill()
    backup.kill()
    for name, m in (('master', master), ('backup', backup)):
        print(name, 'said:', m.stderr.read().strip().replace(chr(10), ' | ')[:600])
sys.exit(r.returncode)
