"""City Trial maps: the settings' map, every player's report of it, the match waiting for them, and the map's
frames through the relay between any members of a lobby (docs/ONLINE.md "Maps")."""

import struct
import time

from airride_master import protocol as P
from airride_master import server as server_module
from airride_master.client import RequestFailed, frame

from .harness import ServerTest

HASH = "ab" * 32
OTHER = "cd" * 32
MAP = {"folder": "HyruleField", "title": "Hyrule Field", "hash": HASH, "size": 1234}
MAP_ETHERTYPE = 0x88B6
NOBODY = b"\x00" * 6


def mac(n):
    return bytes((0x02, 0x4B, 0x41, 0x52, 0x00, n))


class MapSettings(ServerTest):
    def setUp(self):
        super().setUp()
        self._times = (server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD)
        server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD = 0.4, 0.4, 0.2

    def tearDown(self):
        server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD = self._times
        super().tearDown()

    def server_of(self, *names_, **options):
        clients = [self.user(n) for n in names_]
        lid = clients[0].host(name="maps", **options)["lobby"]["id"]
        for c in clients[1:]:
            c.join(lid)
        return clients

    def state(self, c):
        """The latest lobby state `c` has been sent."""
        c.request("ping")
        last = None
        for e in list(c.events):
            if e["t"] == "lobby":
                last = e["lobby"]
        c.events = [e for e in c.events if e["t"] != "lobby"]
        return last

    @staticmethod
    def map_ok(lobby, name):
        return next(m for m in lobby["members"] if m["name"] == name)["map_ok"]

    def test_settings_map_needs_a_hash(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        for bad in ({"map": "HyruleField"}, {"map": {"folder": "HyruleField"}}, {"map": {"folder": "x", "hash": "12"}},
                    {"map": {"folder": "x", "hash": "zz" * 32}}):
            with self.assertRaises(RequestFailed, msg=bad):
                a.request("settings_set", settings=bad)
        a.request("settings_set", settings={"map": {"folder": "", "title": ""}})  # (no map: the city)
        a.request("settings_set", settings={"map": MAP})
        self.assertEqual(self.state(b)["settings"]["map"], MAP)
        with self.assertRaises(RequestFailed):  # (lobby_set checks it too)
            a.request("lobby_set", settings={"map": {"folder": "x"}})

    def test_map_status(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        self.assertEqual(self.map_ok(self.state(a), "bob"), "")
        b.request("map_status", map_ok=HASH.upper())
        self.assertEqual(self.map_ok(self.state(a), "bob"), HASH)  # (kept lower case)
        for bad in ("12", "zz" * 32, 5):
            with self.assertRaises(RequestFailed, msg=bad):
                b.request("map_status", map_ok=bad)
        b.request("map_status", map_ok="")
        self.assertEqual(self.map_ok(self.state(a), "bob"), "")
        c = self.user("carol")
        with self.assertRaises(RequestFailed) as e:  # (in a lobby only)
            c.request("map_status", map_ok=HASH)
        self.assertEqual(e.exception.code, "not_in_lobby")

    def test_countdown_waits_for_every_player_map(self):
        a, b, c = self.server_of("alice", "bob", "carol", max_players=2, settings={"map": MAP})
        self.assertEqual(self.state(a)["state"], "lobby")  # 2 players in, but neither has the map
        a.request("map_status", map_ok=HASH)
        self.assertEqual(self.state(a)["state"], "lobby")
        b.request("map_status", map_ok=OTHER)  # (another map: not this one)
        self.assertEqual(self.state(a)["state"], "lobby")
        b.request("map_status", map_ok=HASH)  # carol waits: hers does not count
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")
        m = b.event("match_start", timeout=3)["match"]
        self.assertEqual(m["settings"]["map"], MAP)
        self.assertEqual([p["name"] for p in m["players"]], ["alice", "bob"])

    def test_no_map_no_wait(self):
        a, b = self.server_of("alice", "bob", settings={"map": {"folder": "", "title": "Original"}})
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")
        b.event("match_start", timeout=3)

    def test_host_start_waits_for_map(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        a.request("settings_set", settings={"map": MAP})
        a.request("map_status", map_ok=HASH)
        with self.assertRaises(RequestFailed) as e:
            a.request("session_start", now=True)
        self.assertEqual(e.exception.code, "map_missing")
        b.request("map_status", map_ok=HASH)
        a.request("session_start", now=True)
        self.assertEqual(b.event("match_start")["match"]["settings"]["map"]["hash"], HASH)
        a.request("match_end")
        a.request("session_continue")
        # another map for the next match: nobody has it, so no match until they do
        a.request("settings_set", settings={"map": dict(MAP, hash=OTHER)})
        with self.assertRaises(RequestFailed) as e:
            a.request("session_start")
        self.assertEqual(e.exception.code, "map_missing")
        a.request("settings_set", settings={})  # the city again: no wait
        a.request("session_start", now=True)
        b.event("match_start")

    def test_countdown_stops_for_a_player_without_the_map(self):
        server_module.COUNTDOWN_TIME = 5.0
        a, b = self.server_of("alice", "bob", settings={"map": MAP})
        a.request("map_status", map_ok=HASH)
        b.request("map_status", map_ok=HASH)
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")
        c = self.user("carol")  # a third player, without it
        c.join(self.state(a)["id"])
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "lobby" and len(e["lobby"]["members"]) == 3)
        c.request("map_status", map_ok=HASH)
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")


class MapRelay(ServerTest):
    """The map's frames (a download from the host's console) reach members not on the roster; the game's do not."""

    def setUp(self):
        super().setUp()
        # 2 consoles a match: alice (the host) and bob play; carol waits. erin hosts another server.
        self.a, self.b, self.c, self.e = (self.user(n) for n in ("alice", "bob", "carol", "erin"))
        self.lid = self.a.host(name="maps", max_players=2, auto_start=False)["lobby"]["id"]
        for x in (self.b, self.c):
            x.join(self.lid)
        self.e.host(name="other")
        self.ua, self.ub, self.uc, self.ue = (x.udp() for x in (self.a, self.b, self.c, self.e))
        for u in (self.ua, self.ub, self.uc, self.ue):
            self.assertTrue(u.hello())

    def tearDown(self):
        for u in (self.ua, self.ub, self.uc, self.ue):
            u.close()
        super().tearDown()

    def assertNothing(self, *relays):
        for u in relays:
            self.assertIsNone(u.recv(timeout=0.2))

    @staticmethod
    def map_frame(src, kind, body=b""):
        return frame(NOBODY, src, struct.pack(">BBH", kind, 1, 0) + bytes.fromhex(HASH)[:8] + body, ethertype=MAP_ETHERTYPE)

    def test_waiting_member_downloads_from_the_host(self):
        ask = self.map_frame(mac(3), 1)
        self.uc.send_to([self.a.relay["id"]], ask)
        self.assertEqual(self.ua.recv(), ask)  # (arrives as a plain FRAME, with carol's relay id)
        chunk = self.map_frame(mac(1), 3, struct.pack(">I", 0) + b"m" * 1200)
        self.ua.send_to([self.c.relay["id"]], chunk)
        self.assertEqual(self.uc.recv(), chunk)
        self.assertNothing(self.ub)
        # the game's frames still stay on the roster's LAN
        self.uc.send_to([self.a.relay["id"]], frame(b"\xff" * 6, mac(3), b"let me play"))
        self.assertNothing(self.ua)

    def test_map_frames_are_addressed_and_stay_in_the_lobby(self):
        f = self.map_frame(mac(1), 2, struct.pack(">IIH", 1000, 100, 1200))
        self.ua.send(f)  # a plain FRAME of the map's: to no one
        self.assertNothing(self.ub, self.uc)
        self.ua.send_to([self.e.relay["id"], 12345], f)  # another server's member, nobody
        self.assertNothing(self.ue)
        self.ua.send_to([self.b.relay["id"], self.c.relay["id"]], f)
        self.assertEqual(self.ub.recv(), f)
        self.assertEqual(self.uc.recv(), f)
        self.assertEqual(self.srv.call(lambda: dict(self.srv.server.lobbies[self.lid].lan.macs)), {})  # (no MAC learnt)

    def test_map_frames_count_against_the_rate(self):
        saved = P.RELAY_RATE
        P.RELAY_RATE = 1
        try:
            m = self.srv.call(lambda: next(x for x in self.srv.server.relay.members.values() if x.name == "alice"))
            self.srv.call(lambda: (setattr(m, "tokens", 3.0), setattr(m, "tokens_time", time.monotonic())))
            for _ in range(10):
                self.ua.send_to([self.c.relay["id"]], self.map_frame(mac(1), 3, b"x" * 100))
            got = 0
            while self.uc.recv(timeout=0.3) is not None:
                got += 1
        finally:
            P.RELAY_RATE = saved
        self.assertIn(got, (3, 4))
