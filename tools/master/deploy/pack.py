"""The files a master server needs, in one folder to copy to a server: build/master-deploy (master.c, the UDP code and
its headers; setup.sh for an Ubuntu server, Dockerfile and compose.yaml for a container: a Synology NAS). Then: scp -r build/master-deploy root@SERVER:~ and ssh root@SERVER "cd master-deploy && sh setup.sh"

    python tools/master/deploy/pack.py"""
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
OUT = REPO / 'build' / 'master-deploy'
shutil.rmtree(OUT, ignore_errors=True)
OUT.mkdir(parents=True)
for f in ('tools/master/master.c', 'tools/master/accounts.c', 'tools/master/accounts.h', 'tools/master/deploy/setup.sh', 'tools/master/deploy/Dockerfile', 'tools/master/deploy/compose.yaml', 'tools/master/deploy/relay_check.py',
          'src/net/master.h', 'src/net/net_udp.c', 'src/net/net_udp.h', 'src/net/session.h', 'src/net/rollback.h'):
    data = (REPO / f).read_bytes().replace(b'\r\n', b'\n')  # (Linux line ends: the shell script would not run with CRLF)
    (OUT / Path(f).name).write_bytes(data)
print(OUT)
