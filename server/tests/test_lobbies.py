from airride_master import server as server_module
from airride_master.client import RequestFailed

from .harness import ServerTest


def names(lobby):
    return sorted(m["name"] for m in lobby["members"])


class Hosting(ServerTest):
    def test_host_list_join_leave(self):
        a = self.user("alice")
        b = self.user("bob")
        reply = a.host(name="Alice's  City\tTrial", max=8, mode="city_trial", region="EU")
        lobby = reply["lobby"]
        self.assertEqual(lobby["name"], "Alice's City Trial")  # whitespace cleaned
        self.assertEqual((lobby["host"], lobby["max"], lobby["mode"], lobby["region"]), ("alice", 8, "city_trial", "EU"))
        self.assertIn("secret", reply["relay"])

        rows = b.request("list_servers")["servers"]
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual((row["name"], row["host"], row["members"], row["max"], row["locked"]),
                         ("Alice's City Trial", "alice", 1, 8, False))
        self.assertEqual(b.request("list_servers", mode="air_ride")["servers"], [])

        joined = b.join(row["id"])
        self.assertEqual(names(joined["lobby"]), ["alice", "bob"])
        self.assertTrue(any("bob joined" in line["text"] for line in joined["chat"]))
        ev = a.event("lobby", where=lambda e: len(e["lobby"]["members"]) == 2)
        self.assertEqual(names(ev["lobby"]), ["alice", "bob"])

        b.request("leave")
        ev = a.event("lobby", where=lambda e: len(e["lobby"]["members"]) == 1)
        self.assertEqual(names(ev["lobby"]), ["alice"])
        a.request("leave")
        self.assertEqual(b.request("list_servers")["servers"], [])

    def test_password_and_full(self):
        a = self.user("alice")
        lid = a.host(name="secret", max=2, password="kirby")["lobby"]["id"]
        b = self.user("bob")
        c = self.user("carol")
        self.assertTrue(b.request("list_servers")["servers"][0]["locked"])
        with self.assertRaises(RequestFailed) as e:
            b.join(lid)
        self.assertEqual(e.exception.code, "bad_password")
        with self.assertRaises(RequestFailed):
            b.join(lid, "Kirby")
        b.join(lid, "kirby")
        with self.assertRaises(RequestFailed) as e:
            c.join(lid, "kirby")
        self.assertEqual(e.exception.code, "full")

    def test_options_validated(self):
        a = self.user("alice")
        for fields in ({"max": 17}, {"max": 1}, {"mode": "top_ride"}, {"region": "x" * 13}, {"region": "<b>"},
                       {"password": "p" * 65}):
            with self.assertRaises(RequestFailed, msg=fields):
                a.host(**fields)
        lobby = a.host()["lobby"]  # all defaults
        self.assertEqual((lobby["name"], lobby["max"], lobby["mode"]), ("alice's server", 16, "any"))

    def test_lobby_set(self):
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host(name="one")["lobby"]["id"]
        b.join(lid)
        with self.assertRaises(RequestFailed) as e:
            b.request("lobby_set", name="mine now")
        self.assertEqual(e.exception.code, "not_host")
        a.request("lobby_set", name="two", password="pw", mode="air_ride")
        ev = b.event("lobby", where=lambda e: e["lobby"]["name"] == "two")
        self.assertTrue(ev["lobby"]["locked"])
        self.assertEqual(ev["lobby"]["mode"], "air_ride")
        with self.assertRaises(RequestFailed):
            a.request("lobby_set", max=1)

    def test_kick_bans_from_lobby(self):
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host()["lobby"]["id"]
        b.join(lid)
        with self.assertRaises(RequestFailed):
            b.request("kick", username="alice")
        a.request("kick", username="BOB")
        gone = b.event("lobby_gone")
        self.assertEqual(gone["reason"], "kicked")
        with self.assertRaises(RequestFailed) as e:
            b.join(lid)
        self.assertEqual(e.exception.code, "banned")
        with self.assertRaises(RequestFailed) as e:
            b.request("chat", text="hi")
        self.assertEqual(e.exception.code, "not_in_lobby")

    def test_host_migrates(self):
        a = self.user("alice")
        b = self.user("bob")
        c = self.user("carol")
        lid = a.host()["lobby"]["id"]
        b.join(lid)
        c.join(lid)
        a.request("leave")
        ev = c.event("lobby", where=lambda e: len(e["lobby"]["members"]) == 2)
        self.assertEqual(ev["lobby"]["host"], "bob")  # the longest-standing member
        c.event("chat", where=lambda e: "bob is now the host" in e["text"])
        b.request("kick", username="carol")  # the new host has the host's powers

    def test_host_leaving_closes_without_migration(self):
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host(migrate=False)["lobby"]["id"]
        b.join(lid)
        a.request("leave")
        self.assertEqual(b.event("lobby_gone")["reason"], "closed")
        self.assertEqual(b.request("list_servers")["servers"], [])

    def test_hosting_again_leaves_the_old_lobby(self):
        a = self.user("alice")
        a.host(name="first")
        a.host(name="second")
        rows = a.request("list_servers")["servers"]
        self.assertEqual([r["name"] for r in rows], ["second"])

    def test_dropped_connection_keeps_place_then_resumes(self):
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host()["lobby"]["id"]
        b.join(lid)
        token = b.token
        b.close()
        ev = a.event("lobby", where=lambda e: any(m["away"] for m in e["lobby"]["members"]))
        self.assertEqual(names(ev["lobby"]), ["alice", "bob"])
        b2 = self.connect()
        reply = b2.resume("bob", token)
        self.assertEqual(reply["lobby"]["id"], lid)  # back in the lobby, a player still, same relay
        self.assertTrue(next(m for m in reply["lobby"]["members"] if m["name"] == "bob")["player"])
        self.assertIn("relay", reply)
        b2.event("peers", where=lambda e: e["peers"] and e["peers"][0]["name"] == "alice")  # sent again
        b2.request("chat", text="back")

    def test_away_member_removed_after_grace(self):
        from airride_master import server as server_module
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host()["lobby"]["id"]
        b.join(lid)
        saved = server_module.AWAY_GRACE
        server_module.AWAY_GRACE = 0.5
        try:
            b.close()
            ev = a.event("lobby", timeout=5, where=lambda e: len(e["lobby"]["members"]) == 1)
            self.assertEqual(names(ev["lobby"]), ["alice"])
        finally:
            server_module.AWAY_GRACE = saved


class Session(ServerTest):
    def setUp(self):
        super().setUp()
        self._times = (server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD)
        server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD = 0.4, 0.4, 0.2

    def tearDown(self):
        server_module.COUNTDOWN_TIME, server_module.RESULTS_TIME, server_module.START_LEAD = self._times
        super().tearDown()

    def server_of(self, *names_, **options):
        clients = [self.user(n) for n in names_]
        lid = clients[0].host(name="session", **options)["lobby"]["id"]
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
    def players(lobby):
        return [m["name"] for m in lobby["members"] if m["player"]]

    def test_joining_puts_you_in_the_match(self):
        cs = self.server_of("user1", "user2", "user3", "user4", "user5", auto_start=False)
        st = self.state(cs[0])
        self.assertEqual(st["state"], "lobby")
        self.assertEqual(self.players(st), ["user1", "user2", "user3", "user4"])  # 4 consoles a match
        self.assertFalse(next(m for m in st["members"] if m["name"] == "user5")["player"])  # waits
        cs[1].request("leave")
        self.assertEqual(self.players(self.state(cs[0])), ["user1", "user3", "user4", "user5"])

    def test_round_trip_of_the_session(self):
        a, b = self.server_of("alice", "bob", settings={"mode": "city_trial", "time": 5})
        # 2 players: the countdown starts by itself, then the match.
        cd = a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")
        self.assertGreater(cd["lobby"]["countdown_ms"], 0)
        ma = a.event("match_start", timeout=3)["match"]
        mb = b.event("match_start", timeout=3)["match"]
        self.assertEqual(ma, mb)
        self.assertEqual([p["name"] for p in ma["players"]], ["alice", "bob"])
        self.assertEqual([p["console"] for p in ma["players"]], [0, 1])
        self.assertEqual(ma["settings"], {"mode": "city_trial", "time": 5})
        self.assertGreater(ma["start_at"], 0)
        self.assertEqual(ma["start_in_ms"], 200)
        self.assertIsInstance(ma["seed"], int)
        self.assertEqual(ma["players"][0]["peer"], a.relay["id"])
        # Someone joining during the match waits for the next one.
        c = self.user("carol")
        c.join(a.relay and ma and self.state(a)["id"])
        st = self.state(a)
        self.assertEqual(st["state"], "match")
        self.assertEqual(self.players(st), ["alice", "bob"])
        self.assertEqual(st["match"]["players"], ["alice", "bob"])
        with self.assertRaises(RequestFailed):  # not in this match
            c.request("match_end")
        # bob's game says it is over: not yet (the host's, or everyone's, ends it) ...
        b.request("match_end", results={"place": 2})
        self.assertEqual(self.state(a)["state"], "match")
        a.request("match_end", results={"place": 1})
        st = a.event("lobby", where=lambda e: e["lobby"]["state"] == "results")["lobby"]
        self.assertEqual(st["results"], {"alice": {"place": 1}, "bob": {"place": 2}})
        # ... then the next round, with carol.
        a.event("lobby", timeout=3, where=lambda e: e["lobby"]["state"] == "countdown")
        m2 = c.event("match_start", timeout=3)["match"]
        self.assertEqual([p["name"] for p in m2["players"]], ["alice", "bob", "carol"])
        self.assertEqual(m2["id"], ma["id"] + 1)
        self.assertNotEqual(m2["seed"], ma["seed"])

    def test_host_starts_and_cancels(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        self.assertEqual(self.state(a)["state"], "lobby")  # no countdown by itself
        with self.assertRaises(RequestFailed) as e:
            b.request("session_start")
        self.assertEqual(e.exception.code, "not_host")
        a.request("session_start")
        self.assertEqual(self.state(a)["state"], "countdown")
        a.request("session_cancel")
        st = self.state(a)
        self.assertEqual((st["state"], st["hold"]), ("lobby", True))
        a.request("session_start", now=True)
        b.event("match_start")
        a.request("session_cancel")  # abandons the match
        self.assertEqual(self.state(a)["state"], "lobby")

    def test_cancel_holds_the_automatic_countdown(self):
        a, b = self.server_of("alice", "bob")
        a.event("lobby", where=lambda e: e["lobby"]["state"] == "countdown")
        a.request("session_cancel")
        c = self.user("carol")
        c.join(self.state(a)["id"])  # a change in players does not restart it
        self.assertEqual(self.state(a)["state"], "lobby")

    def test_needs_two_players(self):
        a = self.user("alice")
        a.host()
        with self.assertRaises(RequestFailed) as e:
            a.request("session_start", now=True)
        self.assertEqual(e.exception.code, "not_enough_players")

    def test_match_down_to_one_player_ends(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        a.request("session_start", now=True)
        a.event("match_start")
        a.events.clear()
        b.request("leave")
        self.assertEqual(a.event("lobby", where=lambda e: e["lobby"]["state"] != "match")["lobby"]["state"], "results")
        a.event("lobby", timeout=3, where=lambda e: e["lobby"]["state"] == "lobby")

    def test_results_continue(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        a.request("session_start", now=True)
        a.event("match_start")
        a.request("match_end")
        self.assertEqual(self.state(a)["state"], "results")
        a.request("session_continue")
        self.assertEqual(self.state(a)["state"], "lobby")

    def test_settings(self):
        a, b = self.server_of("alice", "bob", auto_start=False)
        a.request("settings_set", settings={"mode": "air_ride", "laps": 3, "course": "fantasy_meadows"})
        st = b.event("lobby", where=lambda e: e["lobby"]["settings"].get("laps") == 3)["lobby"]
        self.assertEqual(st["settings"]["course"], "fantasy_meadows")
        with self.assertRaises(RequestFailed) as e:
            b.request("settings_set", settings={})
        self.assertEqual(e.exception.code, "not_host")
        for bad in ([1, 2], "x", {"big": "y" * 5000}):
            with self.assertRaises(RequestFailed):
                a.request("settings_set", settings=bad)
        a.request("session_start", now=True)
        self.assertEqual(a.event("match_start")["match"]["settings"]["laps"], 3)
        with self.assertRaises(RequestFailed) as e:  # not during a match
            a.request("settings_set", settings={"laps": 5})
        self.assertEqual(e.exception.code, "in_match")

    def test_max_players(self):
        a, b, c = self.server_of("alice", "bob", "carol", max_players=2, auto_start=False)
        self.assertEqual(self.players(self.state(a)), ["alice", "bob"])
        for bad in (1, 5):
            with self.assertRaises(RequestFailed):
                a.request("lobby_set", max_players=bad)
        a.request("lobby_set", max_players=3)
        self.assertEqual(self.players(self.state(a)), ["alice", "bob", "carol"])

    def test_peers(self):
        a, b, c = self.server_of("alice", "bob", "carol", max_players=2, auto_start=False)
        a.request("peer_info", mac="024b41520001",
                  candidates=[{"type": "lan", "ip": "192.168.1.10", "port": 40000},
                              {"type": "public", "ip": "203.0.113.5", "port": 51000}])
        b.request("peer_info", mac="024b41520002", candidates=[{"type": "lan", "ip": "10.0.0.7", "port": 40001}])
        peers = b.event("peers", where=lambda e: e["peers"] and e["peers"][0]["mac"])["peers"]
        self.assertEqual(len(peers), 1)
        pa = peers[0]
        self.assertEqual((pa["name"], pa["id"], pa["mac"]), ("alice", a.relay["id"], "024b41520001"))
        self.assertEqual(pa["key"], a.relay["key"])
        self.assertEqual(pa["candidates"][:2], [{"type": "lan", "ip": "192.168.1.10", "port": 40000},
                                                {"type": "public", "ip": "203.0.113.5", "port": 51000}])
        self.assertEqual(pa["candidates"][2], {"type": "seen", "ip": "127.0.0.1", "port": 40000})
        self.assertTrue(any(e["t"] == "peers" and e["peers"] and e["peers"][0]["name"] == "bob"
                            for e in a.events) or a.event("peers", where=lambda e: e["peers"]))
        # carol waits: no peers for her
        c.request("ping")
        self.assertTrue(all(not e["peers"] for e in c.events if e["t"] == "peers"))
        for bad in ({"mac": "zz"}, {"candidates": [{"type": "lan", "ip": "not-an-ip", "port": 1}]},
                    {"candidates": [{"type": "magic", "ip": "1.2.3.4", "port": 1}]},
                    {"candidates": [{"type": "lan", "ip": "1.2.3.4", "port": 70000}]},
                    {"candidates": [{"type": "lan", "ip": "1.2.3.4", "port": 1}] * 9}):
            with self.assertRaises(RequestFailed, msg=bad):
                a.request("peer_info", **bad)
        # carol moves onto the roster: she gets both, they get her
        a.request("lobby_set", max_players=3)
        got = c.event("peers", where=lambda e: len(e["peers"]) == 2)["peers"]
        self.assertEqual(sorted(p["name"] for p in got), ["alice", "bob"])


class Chat(ServerTest):
    def test_chat(self):
        a = self.user("alice")
        b = self.user("bob")
        lid = a.host()["lobby"]["id"]
        b.join(lid)
        a.request("chat", text="  hello\x07 there\n ")
        line = b.event("chat", where=lambda e: e["from"] == "alice")
        self.assertEqual(line["text"], "hello there")
        with self.assertRaises(RequestFailed):
            a.request("chat", text="\x00\x01")
        a.request("chat", text="y" * 500)
        self.assertEqual(len(b.event("chat", where=lambda e: e["from"] == "alice")["text"]), 200)

    def test_chat_rate_limited(self):
        a = self.user("alice")
        a.host()
        sent = 0
        with self.assertRaises(RequestFailed) as e:
            for i in range(20):
                a.request("chat", text="spam %d" % i)
                sent += 1
        self.assertEqual(e.exception.code, "rate_limited")
        self.assertLessEqual(sent, 5)
