"""The relay over TCP (the control port), alone and mixed with UDP members."""

import socket
import struct
import time

from airride_master import protocol as P
from airride_master.client import RelayStreamClient, frame

from .harness import ServerTest

BROADCAST = b"\xff" * 6


def mac(n):
    return bytes((0x02, 0x4B, 0x41, 0x52, 0x00, n))


class TcpRelay(ServerTest):
    def setUp(self):
        super().setUp()
        # alice + bob + carol play in server 1; dave is alone in server 2.
        self.a, self.b, self.c, self.d = (self.user(n) for n in ("alice", "bob", "carol", "dave"))
        lid = self.a.host(name="tcp relay", auto_start=False)["lobby"]["id"]
        for x in (self.b, self.c):
            x.join(lid)
        self.d.host(name="other")
        self.relays = []

    def tearDown(self):
        for r in self.relays:
            r.close()
        super().tearDown()

    def tcp(self, client, **kw):
        r = RelayStreamClient(client.server_host, client.server_port, client.relay["id"],
                              bytes.fromhex(client.relay["secret"]), **kw)
        self.relays.append(r)
        self.assertTrue(r.hello())
        return r

    def udp(self, client):
        r = client.udp()
        self.relays.append(r)
        self.assertTrue(r.hello())
        return r

    def member(self, name):
        return self.srv.call(lambda: next(m for m in self.srv.server.relay.members.values() if m.name == name))

    def test_advertised(self):
        self.assertTrue(self.a.welcome["relay_tcp"])

    def test_tcp_only_lan(self):
        ta, tb, tc = self.tcp(self.a), self.tcp(self.b), self.tcp(self.c)
        td = self.tcp(self.d)
        f = frame(BROADCAST, mac(1), b"over tcp")
        ta.send(f)
        self.assertEqual(tb.recv(), f)
        self.assertEqual(tc.recv(), f)
        self.assertIsNone(td.recv(timeout=0.2))  # another server
        g = frame(mac(1), mac(2), b"unicast back")
        tb.send(g)
        self.assertEqual(ta.recv(), g)
        self.assertIsNone(tc.recv(timeout=0.2))
        h = frame(BROADCAST, mac(3), b"frame_to over tcp")
        tc.send_to([self.b.relay["id"]], h)
        self.assertEqual(tb.recv(), h)
        self.assertIsNone(ta.recv(timeout=0.2))
        state = self.srv.call(lambda: self.srv.server.lobbies[1].state_dict())
        self.assertTrue(all(m["relay"] and m["relay_tcp"] for m in state["members"]))

    def test_mixed_transports(self):
        ta = self.tcp(self.a)     # TCP
        ub = self.udp(self.b)     # UDP
        uc = self.udp(self.c)     # UDP
        f = frame(BROADCAST, mac(1), b"tcp to udp")
        ta.send(f)
        self.assertEqual(ub.recv(), f)
        self.assertEqual(uc.recv(), f)
        g = frame(BROADCAST, mac(2), b"udp to tcp")
        ub.send(g)
        self.assertEqual(ta.recv(), g)
        self.assertEqual(uc.recv(), g)
        h = frame(mac(1), mac(3), b"udp unicast to tcp")
        uc.send(h)
        self.assertEqual(ta.recv(), h)
        self.assertIsNone(ub.recv(timeout=0.2))
        state = self.srv.call(lambda: self.srv.server.lobbies[1].state_dict())
        tcp = {m["name"]: m["relay_tcp"] for m in state["members"] if m["relay"]}
        self.assertEqual(tcp, {"alice": True, "bob": False, "carol": False})

    def test_switching_transport(self):
        ua = self.udp(self.a)
        ub = self.udp(self.b)
        ta = self.tcp(self.a)  # alice moves to TCP: her UDP address is no longer hers
        ua.send(frame(BROADCAST, mac(1), b"stale udp"))
        self.assertIsNone(ub.recv(timeout=0.3))
        f = frame(BROADCAST, mac(1), b"via tcp")
        ta.send(f)
        self.assertEqual(ub.recv(), f)
        self.assertTrue(ua.hello())  # and back to UDP: the stream is closed
        with self.assertRaises((ConnectionError, OSError)):
            for _ in range(20):
                ta.send(frame(BROADCAST, mac(1), b"stale tcp"))
                ta.recv(timeout=0.05)
        g = frame(BROADCAST, mac(1), b"udp again")
        ua.send(g)
        self.assertEqual(ub.recv(), g)

    def test_rejected_streams(self):
        for opening in (b"AIRRELAX", P.RELAY_STREAM_MAGIC + struct.pack(">H", 20) + struct.pack(
                ">BBHI", P.RELAY_HELLO, P.RELAY_VERSION, 0, self.a.relay["id"]) + b"\x00" * 12):
            s = socket.create_connection(("127.0.0.1", self.srv.tcp_port), timeout=3)
            s.sendall(opening)
            self.assertEqual(s.recv(100), b"")  # closed: bad magic / wrong secret
            s.close()
        # A frame before any HELLO also ends it.
        s = socket.create_connection(("127.0.0.1", self.srv.tcp_port), timeout=3)
        s.sendall(P.RELAY_STREAM_MAGIC + struct.pack(">H", 4) + b"abcd")
        self.assertEqual(s.recv(100), b"")
        s.close()

    def test_one_member_per_stream(self):
        ta = self.tcp(self.a)
        ta._send(struct.pack(">BBHI", P.RELAY_HELLO, P.RELAY_VERSION, 0, self.b.relay["id"])
                 + bytes.fromhex(self.b.relay["secret"]) + b"\x00\x00\x00\x01")
        with self.assertRaises((ConnectionError, OSError)):
            ta.recv(timeout=1.0)
            ta.send(frame(BROADCAST, mac(1), b"x"))
            ta.recv(timeout=0.5)

    def test_leaving_closes_stream(self):
        ta = self.tcp(self.a)
        self.a.request("leave")
        with self.assertRaises((ConnectionError, OSError)):
            ta.recv(timeout=1.0)

    def test_backed_up_stream_drops_frames(self):
        """A member that stops reading gets frames dropped, not seconds of them queued; the others
        keep receiving."""
        slow = self.tcp(self.b, rcvbuf=4096)  # never reads after this
        uc = self.udp(self.c)
        ua = self.udp(self.a)
        big = frame(BROADCAST, mac(1), b"x" * 1400)
        for i in range(2500):
            ua.send(big)
            if i % 100 == 0:
                time.sleep(0.01)
        time.sleep(0.3)
        bob = self.member("bob")
        dropped = self.srv.call(lambda: bob.dropped)
        queued = self.srv.call(lambda: bob.stream.transport.get_write_buffer_size() if bob.stream else 0)
        self.assertGreater(dropped, 0)
        self.assertLessEqual(queued, P.RELAY_STREAM_QUEUE + 2 * (P.FRAME_MAX + 10))
        carol = self.member("carol")
        self.assertEqual(self.srv.call(lambda: carol.frames_out), 2500)  # carol was served throughout
        self.assertIsNotNone(uc.recv())
        del slow

    def test_latency(self):
        ta, tb = self.tcp(self.a), self.tcp(self.b)
        times = []
        for i in range(200):
            f = frame(mac(2), mac(1), struct.pack(">I", i))
            t0 = time.perf_counter()
            ta.send(f)
            self.assertEqual(tb.recv(), f)
            times.append(time.perf_counter() - t0)
        times.sort()
        print("\n    tcp relay one way (python clients): median %.0f us, p95 %.0f us"
              % (times[100] * 1e6, times[190] * 1e6), end="")
        self.assertLess(times[100], 0.005)
