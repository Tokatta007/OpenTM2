#!/bin/sh
# The TM2 PC master server on a fresh Ubuntu server (a DigitalOcean droplet): run as root in the folder holding these
# files (sh setup.sh). It builds the master, runs it as a service on UDP 27522 (started at boot, restarted if it
# stops), and opens that port (and SSH) in the firewall. Run it again after copying newer files: it rebuilds.
set -e
PORT=27522
# the compiler, once (an update run again needs no apt: Ubuntu's own updates may hold its lock)
if ! command -v gcc >/dev/null 2>&1; then
    while fuser /var/lib/dpkg/lock-frontend /var/lib/apt/lists/lock >/dev/null 2>&1; do echo "waiting for Ubuntu's updates to finish..."; sleep 5; done
    apt-get update -y
    apt-get install -y gcc
fi
gcc -O2 -I. -o /usr/local/bin/tm2_master master.c accounts.c net_udp.c
id tm2master >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin tm2master
cat > /etc/systemd/system/tm2-master.service <<UNIT
[Unit]
Description=TM2 PC master server
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/tm2_master $PORT /var/lib/tm2-master/tm2_master.db
User=tm2master
StateDirectory=tm2-master
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable tm2-master
systemctl restart tm2-master
if command -v ufw >/dev/null 2>&1; then
    ufw allow OpenSSH
    ufw allow $PORT/udp
    ufw --force enable
fi
sleep 1
systemctl --no-pager status tm2-master | head -5
echo "The master server runs on UDP $PORT. Its log: journalctl -u tm2-master -f"
