#!/usr/bin/env python3
"""Several installations on one Linux PC (test mode, docs/multi-instance.md "On Linux"): a UDP relay between two
sessions' network namespaces, over Unix datagram sockets (files cross namespaces).

Run one in each session's namespace, entered by its game's pid:
    nsenter -t <game pid> -U -n --preserve-credentials python3 -I app/linux/hf-mi-relay.py <me> <peer> <port>...
<me> and <peer> name the two relays (a, b); the ports are the OTHER installation's: its HELLFORGE_PEER_LINK port (the
backends' link) and, with HELLFORGE_TEST_BROKER=1, its broker's P2P port, 48000 + (its SteamID mod 1000). The relay
binds those ports on this namespace's loopback; a datagram sent to one goes to the other relay, which sends it from the
same source port to the same port on its own loopback, so each side sees 127.0.0.1:<sender's port> as if both ran in one
namespace. Its sockets are in $XDG_RUNTIME_DIR (a socket path has at most 108 bytes). Test mode only: nothing a player's
session starts runs it."""
import os
import select
import socket
import struct
import sys
import time

folder = os.environ.get('XDG_RUNTIME_DIR') or '/tmp'
me, peer, ports = sys.argv[1], sys.argv[2], [int(p) for p in sys.argv[3:]]
own = os.path.join(folder, 'hf-mi-relay-%s.sock' % me)
other = os.path.join(folder, 'hf-mi-relay-%s.sock' % peer)
try:
    os.unlink(own)
except FileNotFoundError:
    pass
unix = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
unix.bind(own)
udp = {}
for port in ports:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', port))
    udp[port] = s
by_fd = {s.fileno(): (port, s) for port, s in udp.items()}
counts = {'out': 0, 'in': 0, 'dropped': 0}
last = time.monotonic()
print('relay %s: bound %s' % (me, ports), flush=True)
while True:
    ready, _, _ = select.select([unix] + list(udp.values()), [], [], 5)
    for r in ready:
        if r is unix:
            # from the other relay: (sender's port, destination port), then the datagram
            data = unix.recv(70000)
            source, destination = struct.unpack('!HH', data[:4])
            s = udp.get(source)
            if s:
                s.sendto(data[4:], ('127.0.0.1', destination))
                counts['in'] += 1
            else:
                counts['dropped'] += 1
        else:
            destination, s = by_fd[r.fileno()]
            data, (_, source) = s.recvfrom(70000)
            try:
                unix.sendto(struct.pack('!HH', source, destination) + data, other)
                counts['out'] += 1
            except OSError:
                counts['dropped'] += 1
    if time.monotonic() - last > 30:
        last = time.monotonic()
        print('relay %s: %s' % (me, counts), flush=True)
