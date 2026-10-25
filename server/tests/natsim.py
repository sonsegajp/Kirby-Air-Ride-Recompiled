"""A NAT simulator and a STUN server, for the peer-to-peer tests (all on localhost).

A client in gateway mode (rt::airnet::detail::setUdpGateway) sends every datagram to its Nat
wrapped as [IPv4 4 bytes][port 2 bytes][datagram] (the far end) and gets every datagram back
wrapped the same way (the sender). The Nat sends them on from its own "public" socket(s):

- cone (port-restricted cone, the common home router): one public socket for everything (the same
  public port whoever it talks to), and an incoming datagram gets in only from an address and port
  the inside client has sent to. Hole punching works: both ends send at once, each opening the way
  for the other.
- symmetric: a public socket per far end (each gets its own port), with the same filtering. What a
  STUN server sees is useless to anyone else: hole punching fails, and the relay must carry it.
"""

import select
import socket
import struct
import threading


class Nat(threading.Thread):
    def __init__(self, symmetric=False):
        super().__init__(daemon=True)
        self.symmetric = symmetric
        self.inside = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.inside.bind(("127.0.0.1", 0))
        self.client = None
        self.outs = {}       # far end (symmetric) or None (cone) -> public socket
        self.allowed = {}    # public socket -> far ends the client has sent to through it
        self.dropped = 0
        self.passed = 0
        self._halt = False
        self.start()

    @property
    def address(self):
        return "127.0.0.1:%d" % self.inside.getsockname()[1]

    def _public(self, dest):
        key = dest if self.symmetric else None
        sock = self.outs.get(key)
        if sock is None:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("127.0.0.1", 0))
            self.outs[key] = sock
            self.allowed[sock] = set()
        return sock

    def run(self):
        while not self._halt:
            socks = [self.inside] + list(self.outs.values())
            try:
                ready, _, _ = select.select(socks, [], [], 0.1)
            except (OSError, ValueError):
                return
            for s in ready:
                try:
                    data, src = s.recvfrom(4096)
                except OSError:
                    continue
                if s is self.inside:
                    if len(data) < 6:
                        continue
                    self.client = src
                    dest = (socket.inet_ntoa(data[:4]), struct.unpack(">H", data[4:6])[0])
                    pub = self._public(dest)
                    self.allowed[pub].add(dest)
                    try:
                        pub.sendto(data[6:], dest)
                    except OSError:
                        pass
                elif src in self.allowed.get(s, ()) and self.client:
                    self.passed += 1
                    self.inside.sendto(socket.inet_aton(src[0]) + struct.pack(">H", src[1]) + data, self.client)
                else:
                    self.dropped += 1

    def stop(self):
        self._halt = True
        self.join(2)
        for s in [self.inside] + list(self.outs.values()):
            s.close()


class StunServer(threading.Thread):
    """Answers RFC 5389 Binding requests with XOR-MAPPED-ADDRESS (what a public STUN server does)."""

    COOKIE = 0x2112A442

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.answered = 0
        self._halt = False
        self.start()

    @property
    def address(self):
        return "127.0.0.1:%d" % self.sock.getsockname()[1]

    def run(self):
        while not self._halt:
            try:
                data, src = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(data) < 20 or data[:2] != b"\x00\x01" or struct.unpack(">I", data[4:8])[0] != self.COOKIE:
                continue
            ip = struct.unpack(">I", socket.inet_aton(src[0]))[0] ^ self.COOKIE
            attr = struct.pack(">HHBBHI", 0x0020, 8, 0, 1, src[1] ^ (self.COOKIE >> 16), ip)
            self.sock.sendto(struct.pack(">HHI", 0x0101, len(attr), self.COOKIE) + data[8:20] + attr, src)
            self.answered += 1

    def stop(self):
        self._halt = True
        self.join(2)
        self.sock.close()
