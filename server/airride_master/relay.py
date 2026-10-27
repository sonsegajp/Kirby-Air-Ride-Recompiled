"""The relay: joins the consoles of a server's match (its roster) into one virtual LAN.

Message = 8-byte header (type u8, version u8, reserved u16, relay id u32 big-endian) + body.

    HELLO     c->s  body: secret[8] + echo u32   binds the member's transport; keepalive
    HELLO_ACK s->c  body: echo u32               (the client's round-trip time)
    FRAME     both  body: one Ethernet frame     forwarded to the other consoles of the sender's LAN
    FRAME_TO  c->s  body: n u8, n relay ids u32, frame: forwarded (as a FRAME) to those consoles only

Each server's roster (the consoles of its match) is one LAN. Clients send frames peer to peer where
they can and through the relay to the rest: FRAME_TO names who still needs a frame, so no console
gets it twice.

One exception: a City Trial map's frames (EtherType protocol.MAP_ETHERTYPE: a console downloading the
host's map) go, as FRAME_TO only, between any members of one lobby - those waiting too. They are no
part of the LAN: its MACs are not learnt from them.

Two transports carry the same messages:
- UDP (the relay port): one message per datagram. The preferred one.
- TCP (the control port, for players whose network lets no UDP through): the connection starts
  with RELAY_STREAM_MAGIC, then each message is preceded by its length (u16 big-endian).
A member is bound to one transport at a time: the one its latest valid HELLO came by. Forwarding
treats them alike - a frame from a TCP member reaches UDP members by UDP and the other way round.

A FRAME is accepted only from the transport its relay id is bound to, so knowing someone's relay id
(it is in the frames they send) is not enough to speak for them: binding takes the secret, which
only the member's own control connection receives. Forwarded messages are passed on unchanged
(the receiver ignores the id in them).

The hot path does a dict lookup, a few compares and one send per recipient - no parsing beyond the
header. Streams never queue more than RELAY_STREAM_QUEUE bytes: past that, frames to that member
are dropped (old LAN frames are worthless), and a stream stuck that way is closed.
"""

import asyncio
import hmac
import logging
import socket
import time

from . import protocol as P

log = logging.getLogger("airride.relay")

_ACK_HEADER = bytes((P.RELAY_HELLO_ACK, P.RELAY_VERSION, 0, 0))
_MESSAGE_MAX = P.RELAY_HEADER + 1 + 4 * P.RELAY_TARGETS_MAX + P.FRAME_MAX  # (a FRAME_TO)


class Stream:
    """A member's TCP relay connection (the sending side)."""

    __slots__ = ("transport", "peer", "backed_since", "dropped")

    def __init__(self, transport):
        self.transport = transport
        self.peer = transport.get_extra_info("peername")
        self.backed_since = None
        self.dropped = 0

    def send(self, data):
        t = self.transport
        if t.is_closing():
            return False
        if t.get_write_buffer_size() > P.RELAY_STREAM_QUEUE:
            self.dropped += 1
            now = time.monotonic()
            if self.backed_since is None:
                self.backed_since = now
            elif now - self.backed_since > P.RELAY_STREAM_STALL:
                log.info("relay stream %s backed up for %.0f s: closing", self.peer, P.RELAY_STREAM_STALL)
                self.close()
            return False
        self.backed_since = None
        t.write(len(data).to_bytes(2, "big") + data)
        return True

    def close(self):
        if not self.transport.is_closing():
            self.transport.close()

    @property
    def closed(self):
        return self.transport.is_closing()


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.members = {}   # relay id -> lobby.Member
        self.streams = set()
        self.frames = 0
        self.bytes = 0
        self.dropped = 0

    # ---- bookkeeping (control side) ---------------------------------------------------------------

    def add(self, m):
        self.members[m.relay_id] = m

    def remove(self, m):
        self.members.pop(m.relay_id, None)
        m.addr = None
        if m.stream is not None:
            m.stream.close()
            m.stream = None

    def expire(self, now=None):
        """Forgets transports that went quiet (the client re-binds with its next HELLO)."""
        now = now or time.monotonic()
        for m in self.members.values():
            if now - m.last_relay > P.RELAY_TIMEOUT:
                m.addr = None
                if m.stream is not None:
                    m.stream.close()
                    m.stream = None

    def close_streams(self):
        for s in list(self.streams):
            s.close()

    # ---- forwarding (both transports) -------------------------------------------------------------

    def _deliver(self, o, data):
        if o.stream is not None:
            if o.stream.send(data):
                o.frames_out += 1
            else:
                o.dropped += 1
        elif o.addr is not None:
            self.transport.sendto(data, o.addr)
            o.frames_out += 1

    def _take(self, m, n):
        """Counts a frame of `n` bytes from `m`: False if it is over its rate (dropped)."""
        now = time.monotonic()
        m.last_relay = now
        # Token bucket: RELAY_RATE frames a second, bursts up to RELAY_BURST.
        tokens = m.tokens + (now - m.tokens_time) * P.RELAY_RATE
        m.tokens_time = now
        if tokens > P.RELAY_BURST:
            tokens = P.RELAY_BURST
        if tokens < 1.0:
            m.tokens = tokens
            m.dropped += 1
            self.dropped += 1
            return False
        m.tokens = tokens - 1.0
        m.frames_in += 1
        self.frames += 1
        self.bytes += n
        return True

    def _map_frame(self, m, data, n, targets):
        """A City Trial map's frame (protocol.MAP_ETHERTYPE): to the members it names (FRAME_TO only), any of the
        sender's lobby - the roster's or not. The LAN (and its MACs) is not involved."""
        if targets is None or m.lobby is None or n > P.FRAME_MAX:
            m.dropped += 1
            return
        if not self._take(m, n):
            return
        for rid in targets:
            o = self.members.get(rid)
            if o is not None and o is not m and o.lobby is m.lobby:
                self._deliver(o, data)

    def _frame(self, m, data, targets=None):
        """A frame from `m` (`data`: a FRAME message); `targets`: the relay ids it is for (FRAME_TO)."""
        n = len(data) - P.RELAY_HEADER
        if n >= P.FRAME_MIN and data[P.RELAY_HEADER + 12:P.RELAY_HEADER + 14] == P.MAP_ETHERTYPE:
            self._map_frame(m, data, n, targets)
            return
        g = m.lan
        if g is None or n < P.FRAME_MIN or n > P.FRAME_MAX:
            m.dropped += 1
            return
        if not self._take(m, n):
            return
        # Learn where the sender's MAC lives (unicast source addresses only).
        if not data[14] & 1:
            src = data[14:20]
            if g.macs.get(src) is not m:
                g.macs[src] = m
        if targets is not None:
            for rid in targets:
                o = self.members.get(rid)
                if o is not None and o is not m and o.lan is g:
                    self._deliver(o, data)
            return
        if not data[8] & 1:  # unicast destination
            dest = g.macs.get(data[8:14])
            if dest is not None and dest is not m and dest.lan is g:
                self._deliver(dest, data)
                return
        for o in g.members:  # broadcast, multicast, or a MAC not seen yet: the whole LAN
            if o is not m:
                self._deliver(o, data)

    def _frame_to(self, m, data):
        """FRAME_TO -> the FRAME the targets receive, and the targets."""
        if len(data) < P.RELAY_HEADER + 1:
            return
        k = data[8]
        body = P.RELAY_HEADER + 1 + 4 * k
        if not 1 <= k <= P.RELAY_TARGETS_MAX or len(data) < body:
            m.dropped += 1
            return
        targets = [int.from_bytes(data[i:i + 4], "big") for i in range(P.RELAY_HEADER + 1, body, 4)]
        self._frame(m, bytes((P.RELAY_FRAME,)) + data[1:P.RELAY_HEADER] + data[body:], targets)

    def _hello_ok(self, m, data):
        return (len(data) >= P.RELAY_HEADER + P.RELAY_SECRET_BYTES + 4
                and hmac.compare_digest(data[8:16], m.relay_secret))

    # ---- UDP (asyncio.DatagramProtocol) -----------------------------------------------------------

    def connection_made(self, transport):
        self.transport = transport

    def error_received(self, exc):
        # ICMP port unreachable from a member that went away, and the like: nothing to do.
        log.debug("udp error: %s", exc)

    def datagram_received(self, data, addr):
        if len(data) < P.RELAY_HEADER or data[1] != P.RELAY_VERSION:
            return
        m = self.members.get(int.from_bytes(data[4:8], "big"))
        if m is None:
            return
        kind = data[0]
        if kind == P.RELAY_FRAME:
            if m.addr == addr and m.stream is None:
                self._frame(m, data)
        elif kind == P.RELAY_FRAME_TO:
            if m.addr == addr and m.stream is None:
                self._frame_to(m, data)
        elif kind == P.RELAY_HELLO:
            if not self._hello_ok(m, data):
                return
            if m.addr != addr or m.stream is not None:
                if m.addr is not None or m.stream is not None:
                    log.info("relay %08x (%s) now udp %s", m.relay_id, m.name, addr)
                if m.stream is not None:
                    m.stream.close()
                    m.stream = None
                m.addr = addr
            m.last_relay = time.monotonic()
            self.transport.sendto(_ACK_HEADER + data[4:8] + data[16:20], addr)

    # ---- TCP streams ------------------------------------------------------------------------------

    async def serve_stream(self, reader, writer, first):
        """A relay connection on the control port; `first` is its first byte (already read)."""
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, P.RELAY_STREAM_SNDBUF)
            except OSError:
                pass
        stream = Stream(writer.transport)
        self.streams.add(stream)
        bound = None  # the member this stream speaks for, after its HELLO
        try:
            magic = first + await asyncio.wait_for(reader.readexactly(len(P.RELAY_STREAM_MAGIC) - 1), 10)
            if magic != P.RELAY_STREAM_MAGIC:
                return
            while not stream.closed:
                if bound is None:  # the HELLO must come promptly; afterwards expire() watches
                    head = await asyncio.wait_for(reader.readexactly(2), 10)
                else:
                    head = await reader.readexactly(2)
                n = int.from_bytes(head, "big")
                if n < P.RELAY_HEADER or n > _MESSAGE_MAX:
                    return
                data = await reader.readexactly(n)
                if data[1] != P.RELAY_VERSION:
                    continue
                m = self.members.get(int.from_bytes(data[4:8], "big"))
                kind = data[0]
                if kind == P.RELAY_FRAME:
                    if m is not None and m is bound and m.stream is stream:
                        self._frame(m, data)
                elif kind == P.RELAY_FRAME_TO:
                    if m is not None and m is bound and m.stream is stream:
                        self._frame_to(m, data)
                elif kind == P.RELAY_HELLO:
                    if m is None or not self._hello_ok(m, data):
                        if bound is None:
                            return  # not a member: hang up
                        continue
                    if bound is not None and bound is not m:
                        return  # one member per stream
                    bound = m
                    if m.stream is not stream:
                        log.info("relay %08x (%s) now tcp %s", m.relay_id, m.name, stream.peer)
                        if m.stream is not None:
                            m.stream.close()
                        m.stream = stream
                        m.addr = None
                    m.last_relay = time.monotonic()
                    stream.send(_ACK_HEADER + data[4:8] + data[16:20])
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            self.streams.discard(stream)
            if bound is not None and bound.stream is stream:
                bound.stream = None
            stream.close()
