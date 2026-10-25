import json
import secrets
import socket

from airride_master import crypto
from airride_master import protocol as P
from airride_master import server as server_module
from airride_master.client import RequestFailed

from .harness import ServerTest


class Handshake(ServerTest):
    def test_welcome(self):
        c = self.connect()
        w = c.welcome
        self.assertEqual(w["t"], "welcome")
        self.assertEqual(w["proto"], P.PROTO_VERSION)
        self.assertEqual(w["udp_port"], self.srv.udp_port)
        self.assertEqual(w["scrypt"], {"n": P.SCRYPT_N, "r": P.SCRYPT_R, "p": P.SCRYPT_P})
        self.assertEqual(w["motd"], "test server")

    def test_old_client_told_to_update(self):
        c = self.connect(proto=1)  # (match groups; before sessions)
        self.assertEqual(c.welcome["t"], "bye")
        self.assertEqual(c.welcome["code"], "update_required")
        self.assertIn("update", c.welcome["msg"].lower())
        with self.assertRaises(ConnectionError):
            c._read()

    def test_hello_first(self):
        s = socket.create_connection(("127.0.0.1", self.srv.tcp_port), timeout=3)
        s.sendall(b'{"t":"list_servers","id":1}\n')
        msg = json.loads(s.makefile().readline())
        self.assertEqual((msg["t"], msg["code"]), ("bye", "bad_request"))
        s.close()

    def test_garbage_and_long_lines_close(self):
        for payload in (b"not json\n", b"x" * (P.MAX_LINE + 10) + b"\n"):
            s = socket.create_connection(("127.0.0.1", self.srv.tcp_port), timeout=3)
            s.sendall(b'{"t":"hello","proto":%d}\n' % P.PROTO_VERSION)
            f = s.makefile("rb")
            f.readline()  # welcome
            s.sendall(payload)
            msg = json.loads(f.readline())
            self.assertEqual(msg["t"], "bye")
            s.close()

    def test_needs_login(self):
        c = self.connect()
        with self.assertRaises(RequestFailed) as e:
            c.request("list_servers")
        self.assertEqual(e.exception.code, "not_logged_in")


class Accounts(ServerTest):
    def test_register_and_login(self):
        c = self.connect()
        c.register("Meta_Knight", "s3cret-pw")
        reply = c.login("meta_knight", "s3cret-pw")  # case-insensitive
        self.assertEqual(reply["profile"]["username"], "Meta_Knight")
        self.assertEqual(reply["profile"]["colour"], "pink")
        self.assertEqual(len(c.token), P.TOKEN_BYTES)
        c.request("ping")

    def test_names_unique_regardless_of_case(self):
        self.connect().register("Kirby1", "password1")
        with self.assertRaises(RequestFailed) as e:
            self.connect().register("KIRBY1", "password2")
        self.assertEqual(e.exception.code, "name_taken")

    def test_username_rules(self):
        c = self.connect()
        for bad in ("ab", "x" * 17, "_lead", "sp ace", "émile", "a/b", "", 7, "admin", "Server"):
            with self.assertRaises(RequestFailed, msg=repr(bad)) as e:
                c.register(bad, "password")
            self.assertIn(e.exception.code, ("bad_username", "name_taken"))
        for good in ("abc", "x" * 16, "K.i-r_b9"):
            c.register(good, "password")

    def test_wrong_password(self):
        self.connect().register("dedede", "rightpass")
        c = self.connect()
        with self.assertRaises(RequestFailed) as e:
            c.login("dedede", "wrongpass")
        self.assertEqual(e.exception.code, "bad_login")
        c.login("dedede", "rightpass")

    def test_unknown_user_looks_like_a_real_one(self):
        c = self.connect()
        a = c.request("login_begin", username="nobody_here")
        b = c.request("login_begin", username="NOBODY_HERE")
        self.assertEqual(a["salt"], b["salt"])  # stable: probing does not reveal who exists
        self.assertNotEqual(a["nonce"], b["nonce"])
        with self.assertRaises(RequestFailed) as e:
            c.login("nobody_here", "whatever")
        self.assertEqual(e.exception.code, "bad_login")

    def test_nonce_is_single_use(self):
        c = self.connect()
        c.register("waddle", "password")
        begin = c.request("login_begin", username="waddle")
        verifier = crypto.derive_verifier("password", bytes.fromhex(begin["salt"]))
        cnonce = secrets.token_bytes(16)
        proof = crypto.login_proof(verifier, bytes.fromhex(begin["nonce"]), cnonce, "waddle").hex()
        c.request("login", username="waddle", cnonce=cnonce.hex(), proof=proof)
        with self.assertRaises(RequestFailed) as e:  # replaying the same proof
            c.request("login", username="waddle", cnonce=cnonce.hex(), proof=proof)
        self.assertEqual(e.exception.code, "bad_request")

    def test_server_never_stores_a_password(self):
        c = self.connect()
        c.register("tac", "plaintext-pw")
        row = self.srv.server.db.call(self.srv.server.db.credentials, "tac")
        self.assertEqual(row[6], crypto.derive_verifier("plaintext-pw", row[2], row[3], row[4], row[5]))
        with open(self.srv.config.db_path, "rb") as f:
            self.assertNotIn(b"plaintext-pw", f.read())

    def test_lockout_after_failures(self):
        self.connect().register("knuckle", "password")
        c = self.connect()
        for _ in range(server_module.LOGIN_FAILS):
            begin = c.request("login_begin", username="knuckle")
            with self.assertRaises(RequestFailed):
                c.request("login", username="knuckle", cnonce="00" * 16, proof="11" * 32)
        with self.assertRaises(RequestFailed) as e:
            c.login("knuckle", "password")
        self.assertEqual(e.exception.code, "too_many_attempts")
        del begin

    def test_resume_and_logout(self):
        c = self.user("bronto")
        token = c.token
        d = self.connect()
        reply = d.resume("bronto", token)
        self.assertEqual(reply["profile"]["username"], "bronto")
        d.request("logout")
        e = self.connect()
        with self.assertRaises(RequestFailed) as err:
            e.resume("bronto", token)
        self.assertEqual(err.exception.code, "bad_token")

    def test_bad_token(self):
        self.user("burt")
        c = self.connect()
        with self.assertRaises(RequestFailed) as e:
            c.resume("burt", secrets.token_bytes(P.TOKEN_BYTES))
        self.assertEqual(e.exception.code, "bad_token")

    def test_set_password_revokes_tokens(self):
        c = self.user("scarfy", "oldpassword")
        old_token = c.token
        sc = c.welcome["scrypt"]
        salt = secrets.token_bytes(16)
        c.request("set_password", salt=salt.hex(), n=sc["n"], r=sc["r"], p=sc["p"],
                  verifier=crypto.derive_verifier("newpassword", salt).hex())
        with self.assertRaises(RequestFailed):
            self.connect().resume("scarfy", old_token)
        with self.assertRaises(RequestFailed):
            self.connect().login("scarfy", "oldpassword")
        self.connect().login("scarfy", "newpassword")

    def test_one_connection_per_account(self):
        a = self.user("gordo")
        b = self.connect()
        b.login("gordo", "hunter22")
        bye = a.event("bye")
        self.assertEqual(bye["code"], "elsewhere")
        b.request("ping")

    def test_banned(self):
        c = self.user("chilly")
        db = self.srv.server.db
        db.call(db.set_banned, "chilly", True)
        with self.assertRaises(RequestFailed) as e:
            self.connect().login("chilly", "hunter22")
        self.assertEqual(e.exception.code, "banned")
        with self.assertRaises(RequestFailed):
            self.connect().resume("chilly", c.token)


class Profiles(ServerTest):
    def test_get_and_set(self):
        a = self.user("pinky")
        b = self.user("bluey")
        p = a.request("profile_set", colour="yellow", machine="dragoon")["profile"]
        self.assertEqual((p["colour"], p["machine"]), ("yellow", "dragoon"))
        seen = b.request("profile_get", username="PINKY")["profile"]
        self.assertEqual((seen["username"], seen["colour"], seen["machine"]), ("pinky", "yellow", "dragoon"))
        self.assertTrue(seen["online"])
        self.assertEqual((seen["played"], seen["wins"]), (0, 0))
        mine = b.request("profile_get")["profile"]
        self.assertEqual(mine["username"], "bluey")

    def test_invalid_values(self):
        a = self.user("pinky")
        for fields in ({"colour": "chartreuse"}, {"machine": "bicycle"}, {"colour": 3}):
            with self.assertRaises(RequestFailed):
                a.request("profile_set", **fields)
        with self.assertRaises(RequestFailed) as e:
            a.request("profile_get", username="ghost_user")
        self.assertEqual(e.exception.code, "not_found")

    def test_match_reports(self):
        a = self.user("racer")
        b = self.user("rival")
        with self.assertRaises(RequestFailed) as e:  # only from a match played online
            a.request("report_match", won=True)
        self.assertEqual(e.exception.code, "not_in_match")
        lid = a.host(name="r", auto_start=False)["lobby"]["id"]
        b.join(lid)
        a.request("session_start", now=True)
        a.event("match_start")
        p = a.request("report_match", won=True)["profile"]
        self.assertEqual((p["played"], p["wins"]), (1, 1))
        with self.assertRaises(RequestFailed):  # one per MATCH_REPORT_GAP
            a.request("report_match", won=True)
