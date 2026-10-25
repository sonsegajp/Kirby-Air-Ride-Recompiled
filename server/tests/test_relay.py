import struct
import time

from airride_master import protocol as P
from airride_master.client import RelayClient, frame

from .harness import ServerTest

BROADCAST = b"\xff" * 6


def mac(n):
    return bytes((0x02, 0x4B, 0x41, 0x52, 0x00, n))  # locally administered


class Relay(ServerTest):
    def setUp(self):
        super().setUp()
        # Server 1 (3 consoles a match): alice, bob, carol play; dave waits. Server 2: erin alone.
        self.a, self.b, self.c, self.d, self.e = (self.user(n) for n in ("alice", "bob", "carol", "dave", "erin"))
        self.lid = self.a.host(name="relay", max_players=3, auto_start=False)["lobby"]["id"]
        for x in (self.b, self.c, self.d):
            x.join(self.lid)
        self.e.host(name="other")
        self.ua, self.ub, self.uc, self.ud, self.ue = (x.udp() for x in (self.a, self.b, self.c, self.d, self.e))
        for u in (self.ua, self.ub, self.uc, self.ud, self.ue):
            self.assertTrue(u.hello())

    def tearDown(self):
        for u in (self.ua, self.ub, self.uc, self.ud, self.ue):
            u.close()
        super().tearDown()

    def assertNothing(self, *relays):
        for u in relays:
            self.assertIsNone(u.recv(timeout=0.2))

    def test_broadcast_stays_in_the_match(self):
        f = frame(BROADCAST, mac(1), b"hello lan")
        self.ua.send(f)
        self.assertEqual(self.ub.recv(), f)
        self.assertEqual(self.uc.recv(), f)
        self.assertNothing(self.ua, self.ud, self.ue)

    def test_unicast_goes_to_the_owner_of_the_mac(self):
        self.ua.send(frame(BROADCAST, mac(1), b"I am A"))
        self.ub.recv()
        self.uc.recv()
        f = frame(mac(1), mac(2), b"to A")
        self.ub.send(f)
        self.assertEqual(self.ua.recv(), f)
        self.assertNothing(self.uc)
        # A MAC nobody has used yet: the whole LAN (a switch floods unknown destinations).
        f = frame(mac(9), mac(2), b"to whom it may concern")
        self.ub.send(f)
        self.assertEqual(self.ua.recv(), f)
        self.assertEqual(self.uc.recv(), f)

    def test_frame_to_named_consoles_only(self):
        f = frame(BROADCAST, mac(1), b"only carol needs it from the relay")
        self.ua.send_to([self.c.relay["id"]], f)
        self.assertEqual(self.uc.recv(), f)  # (arrives as a plain FRAME)
        self.assertNothing(self.ub)
        # Not on this LAN (waiting; another server; nobody): nothing.
        self.ua.send_to([self.d.relay["id"], self.e.relay["id"], 12345], f)
        self.assertNothing(self.ud, self.ue, self.ub, self.uc)
        g = frame(BROADCAST, mac(2), b"to both")
        self.ub.send_to([self.a.relay["id"], self.c.relay["id"]], g)
        self.assertEqual(self.ua.recv(), g)
        self.assertEqual(self.uc.recv(), g)

    def test_waiting_members_neither_send_nor_receive(self):
        self.ud.send(frame(BROADCAST, mac(4), b"let me in"))
        self.assertNothing(self.ua, self.ub, self.uc)
        self.ua.send(frame(BROADCAST, mac(1), b"players only"))
        self.assertIsNotNone(self.ub.recv())
        self.assertNothing(self.ud)

    def test_roster_change_takes_effect(self):
        self.a.request("lobby_set", max_players=2)  # carol waits now
        f = frame(BROADCAST, mac(1), b"two consoles")
        self.ua.send(f)
        self.assertEqual(self.ub.recv(), f)
        self.assertNothing(self.uc)
        self.a.request("lobby_set", max_players=4)  # carol and dave play
        self.ua.send(f)
        self.assertEqual(self.ub.recv(), f)
        self.assertEqual(self.uc.recv(), f)
        self.assertEqual(self.ud.recv(), f)
        self.b.request("leave")
        self.ua.send(f)
        self.assertEqual(self.uc.recv(), f)
        self.assertNothing(self.ub)

    def test_spoofing_is_ignored(self):
        # A frame carrying alice's relay id from another socket (dave's) is dropped ...
        evil = RelayClient("127.0.0.1", self.srv.udp_port, self.ua.relay_id, b"\x00" * 8)
        try:
            evil.sock.sendto(struct.pack(">BBHI", P.RELAY_FRAME, P.RELAY_VERSION, 0, self.ua.relay_id)
                             + frame(BROADCAST, mac(1), b"fake"), evil.addr)
            self.assertNothing(self.ub)
            # ... and binding needs the secret.
            evil.sock.settimeout(0.3)
            evil.sock.sendto(struct.pack(">BBHI", P.RELAY_HELLO, P.RELAY_VERSION, 0, self.ua.relay_id)
                             + b"\x00" * 8 + b"\x00\x00\x00\x01", evil.addr)
            self.assertIsNone(evil.recv(timeout=0.3))
        finally:
            evil.close()
        f = frame(BROADCAST, mac(1), b"still mine")
        self.ua.send(f)
        self.assertEqual(self.ub.recv(), f)

    def test_size_limits(self):
        self.ua.send(b"\xff" * 13)  # shorter than an Ethernet header
        self.ua.send(frame(BROADCAST, mac(1), b"x" * (P.FRAME_MAX - 14 + 1)))
        self.assertNothing(self.ub)
        f = frame(BROADCAST, mac(1), b"x" * (P.FRAME_MAX - 14))
        self.ua.send(f)
        self.assertEqual(self.ub.recv(), f)

    def test_rate_limit(self):
        # A slow refill (1 frame a second) so the count does not depend on how fast this machine is.
        saved = P.RELAY_RATE
        P.RELAY_RATE = 1
        try:
            m = self.srv.call(lambda: next(x for x in self.srv.server.relay.members.values() if x.name == "alice"))
            self.srv.call(lambda: (setattr(m, "tokens", 3.0), setattr(m, "tokens_time", time.monotonic())))
            for _ in range(10):
                self.ua.send(frame(BROADCAST, mac(1), b"burst"))
            got = 0
            while self.ub.recv(timeout=0.3) is not None:
                got += 1
        finally:
            P.RELAY_RATE = saved
        self.assertIn(got, (3, 4))
        self.assertGreaterEqual(self.srv.call(lambda: m.dropped), 6)

    def test_rebind_after_address_change(self):
        # alice's NAT gives her a new port: her next HELLO moves the binding.
        moved = RelayClient("127.0.0.1", self.srv.udp_port, self.ua.relay_id, self.ua.secret)
        try:
            self.assertTrue(moved.hello())
            f = frame(BROADCAST, mac(1), b"from the new port")
            moved.send(f)
            self.assertEqual(self.ub.recv(), f)
            self.ub.send(frame(BROADCAST, mac(2), b"reply"))
            self.assertIsNotNone(moved.recv())
        finally:
            moved.close()

    def test_leaving_lobby_stops_relay(self):
        self.b.request("leave")
        time.sleep(0.05)
        self.ub.send(frame(BROADCAST, mac(2), b"gone"))
        self.assertNothing(self.ua)

    def test_latency(self):
        """Round trips through the relay on localhost stay well under a frame (16 ms)."""
        times = []
        for i in range(200):
            f = frame(BROADCAST, mac(1), struct.pack(">I", i))
            t0 = time.perf_counter()
            self.ua.send(f)
            self.assertEqual(self.ub.recv(), f)
            times.append(time.perf_counter() - t0)
        times.sort()
        self.assertLess(times[len(times) // 2], 0.005)
