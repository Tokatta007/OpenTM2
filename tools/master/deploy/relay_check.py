"""Live check of both masters (run it on the droplet: from the user's own LAN the NAS sees every packet as the router): a test listing (socket A), the list asked from socket B, then a packet relayed
B -> A and an answer A -> B through each master's RELAY. Goodbyes at the end."""
import socket
import struct
import time

MASTERS = [('147.182.184.67', 27522), ('opentm2-masterserver2.synology.me', 27522)]
BUILD = 0x7E57B11D  # (a made-up build id: only this test's own list request matches it)


def listing():
    w = b'TM2A' + struct.pack('<I', BUILD)
    w += b'TM2 TEST'.ljust(32, b'\0') + b'Claude'.ljust(24, b'\0') + b'Relay test'.ljust(32, b'\0')
    w += struct.pack('<H', 27520) + bytes([0, 8, 0, 1, 0, 0])
    return w


def addr(a, p):
    return a.encode().ljust(16, b'\0') + struct.pack('<H', p)


def recv(s, want_kind):
    end = time.time() + 3
    while time.time() < end:
        try:
            d, src = s.recvfrom(4096)
        except socket.timeout:
            break
        if len(d) >= 2 and d[0] == 0x53 and d[1] == want_kind:
            return d, src
    return None, None


for host, port in MASTERS:
    m = (socket.gethostbyname(host), port)
    a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); a.bind(('', 0)); a.settimeout(1)
    b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); b.bind(('', 0)); b.settimeout(1)
    print('== %s (%s)' % (host, m[0]))
    a.sendto(b'S\x01' + listing(), m)  # REGISTER from A
    time.sleep(0.5)
    t0 = time.time()
    b.sendto(b'S\x03' + struct.pack('<I', BUILD), m)  # LIST from B
    d, _ = recv(b, 4)
    if not d:
        print('  list: no answer'); continue
    a_at = d[2:18].split(b'\0')[0].decode(); a_port = d[18] | d[19] << 8
    print('  list: answered in %.0f ms, the test server seen at %s:%d' % ((time.time() - t0) * 1000, a_at, a_port))
    t0 = time.time()
    b.sendto(b'S\x09' + addr(a_at, a_port) + b'hello through the relay', m)  # RELAY B -> A
    d, _ = recv(a, 10)
    if not d:
        print('  relay B -> A: nothing arrived'); continue
    b_at = d[2:18].split(b'\0')[0].decode(); b_port = d[18] | d[19] << 8
    print('  relay B -> A: "%s" in %.0f ms (from %s:%d)' % (d[20:].decode(), (time.time() - t0) * 1000, b_at, b_port))
    t0 = time.time()
    a.sendto(b'S\x09' + addr(b_at, b_port) + b'and back again', m)  # RELAY A -> B
    d, _ = recv(b, 10)
    print('  relay A -> B: %s' % ('"%s" in %.0f ms' % (d[20:].decode(), (time.time() - t0) * 1000) if d else 'nothing arrived'))
    a.sendto(b'S\x08', m); b.sendto(b'S\x08', m)  # BYE
    print('  goodbyes sent')
