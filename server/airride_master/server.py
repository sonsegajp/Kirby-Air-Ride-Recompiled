"""The master server: control connections (JSON lines over TCP), lobbies, and the UDP relay.

Control protocol (docs/ONLINE.md has the full list):

    client -> server   {"t": "<request>", "id": <int>, ...}
    server -> client   {"t": "reply", "id": <same>, "ok": true, ...}
                       {"t": "reply", "id": <same>, "ok": false, "err": "<code>", "msg": "<text for the player>"}
    server -> client   events: {"t": "lobby" | "chat" | "lobby_gone" | "ping" | "bye", ...}

The first message must be {"t": "hello", "proto": N}; a client older than MIN_PROTO_VERSION gets
{"t": "bye", "code": "update_required"} and the connection closes.
"""

import asyncio
import collections
import ipaddress
import json
import logging
import secrets
import socket
import ssl
import time
from dataclasses import dataclass

from . import __version__
from . import crypto
from . import protocol as P
from .db import Database
from .lobby import COUNTDOWN, LOBBY, MATCH, RESULTS, Lobby, LobbyError, Member
from .relay import Relay

log = logging.getLogger("airride.master")

PING_INTERVAL = 5.0       # server -> client pings (round trip time; liveness)
IDLE_TIMEOUT = 30.0       # nothing heard for this long: the connection is dead
UNAUTH_TIMEOUT = 600.0    # connected but never logged in
AWAY_GRACE = 30.0         # a member whose connection dropped keeps their place this long
MAX_CONNECTIONS = 2000
MAX_PER_IP = 16
MAX_LOBBIES = 500
WRITE_BUFFER_LIMIT = 256 * 1024
LOGIN_FAILS = 10          # failed logins per account or address ...
LOGIN_WINDOW = 600.0      # ... within this many seconds before a lockout
REGISTER_PER_IP = 5       # accounts per address ...
REGISTER_WINDOW = 3600.0  # ... per this many seconds
MATCH_REPORT_GAP = 30.0   # seconds between a player's match reports
COUNTDOWN_TIME = 5.0      # seconds from 2 players being in to the match starting
RESULTS_TIME = 10.0       # seconds the results stay up before the next round
START_LEAD = 1.5          # the match-start message names a start this far ahead (all start together)


@dataclass
class Config:
    host: str = "0.0.0.0"
    tcp_port: int = P.DEFAULT_TCP_PORT
    udp_port: int = P.DEFAULT_UDP_PORT
    public_udp_port: int = 0      # the UDP port clients are told (0: udp_port; set when NAT maps it)
    db_path: str = "airride.db"
    motd: str = ""
    tls_port: int = 0             # 0: no TLS listener (see docs/ONLINE.md "Security")
    tls_cert: str = ""
    tls_key: str = ""


class Fail(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code
        self.msg = msg


class Bucket:
    """Token bucket: `rate` a second, at most `burst` saved up."""

    __slots__ = ("rate", "burst", "tokens", "time")

    def __init__(self, rate, burst):
        self.rate = rate
        self.burst = burst
        self.tokens = float(burst)
        self.time = time.monotonic()

    def take(self, n=1.0):
        now = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (now - self.time) * self.rate)
        self.time = now
        if self.tokens < n:
            return False
        self.tokens -= n
        return True


class Window:
    """Events per key within a sliding window (login failures, registrations)."""

    def __init__(self, window):
        self.window = window
        self.events = collections.defaultdict(collections.deque)

    def count(self, key):
        q = self.events.get(key)
        if not q:
            return 0
        cutoff = time.monotonic() - self.window
        while q and q[0] < cutoff:
            q.popleft()
        if not q:
            del self.events[key]
            return 0
        return len(q)

    def add(self, key):
        self.events[key].append(time.monotonic())

    def prune(self):
        for key in list(self.events):
            self.count(key)


class Session:
    """One control connection."""

    def __init__(self, server, reader, writer):
        self.server = server
        self.reader = reader
        self.writer = writer
        peer = writer.get_extra_info("peername") or ("?", 0)
        self.ip = peer[0]
        self.proto = 0
        self.client = ""
        self.user_id = None
        self.name = None
        self.key = None
        self.colour = "pink"
        self.machine = "warp_star"
        self.token = None          # the session token this connection signed in with
        self.member = None         # lobby.Member while in a lobby
        self.challenge = None      # (username key, server nonce) from login_begin
        self.rtt = -1
        self.ping_n = 0
        self.ping_time = 0.0
        self.connected = time.monotonic()
        self.last_rx = self.connected
        self.requests = Bucket(20, 60)
        self.chat = Bucket(0.5, 5)
        self.last_report = 0.0
        self.closed = False

    def send(self, obj):
        if self.closed:
            return
        transport = self.writer.transport
        if transport.is_closing():
            return
        if transport.get_write_buffer_size() > WRITE_BUFFER_LIMIT:
            log.info("%s: not reading its messages; closing", self)
            self.close()
            return
        self.writer.write(json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n")

    def reply(self, msg, **fields):
        self.send({"t": "reply", "id": msg.get("id"), "ok": True, **fields})

    def fail(self, msg, code, text):
        self.send({"t": "reply", "id": msg.get("id"), "ok": False, "err": code, "msg": text})

    def bye(self, code, text):
        self.send({"t": "bye", "code": code, "msg": text})
        self.close()

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.writer.close()
            except Exception:  # noqa: BLE001 - the socket may already be gone
                pass

    def __str__(self):
        return "%s(%s)" % (self.name or "-", self.ip)


class MasterServer:
    def __init__(self, config):
        self.config = config
        self.db = Database(config.db_path)
        self.secret = self.db.call(self.db.secret)
        self.relay = Relay()
        self.sessions = set()
        self.online = {}          # username key -> Session (signed in)
        self.away = {}            # username key -> Member whose connection dropped
        self.lobbies = {}         # lobby id -> Lobby
        self.next_lobby = 1
        self.per_ip = collections.Counter()  # open TCP connections (control and relay) per address
        self.connections = 0
        self.login_fails = Window(LOGIN_WINDOW)
        self.registrations = Window(REGISTER_WINDOW)
        self._servers = []
        self._udp = None
        self._tasks = []
        self.tcp_port = 0
        self.udp_port = 0
        self.tls_port = 0
        self.handlers = {
            "ping": self.h_ping, "pong": self.h_pong,
            "register": self.h_register, "login_begin": self.h_login_begin, "login": self.h_login,
            "resume": self.h_resume, "logout": self.h_logout, "set_password": self.h_set_password,
            "profile_get": self.h_profile_get, "profile_set": self.h_profile_set,
            "report_match": self.h_report_match,
            "list_servers": self.h_list_servers, "host": self.h_host, "join": self.h_join,
            "leave": self.h_leave, "kick": self.h_kick, "lobby_set": self.h_lobby_set,
            "peer_info": self.h_peer_info, "settings_set": self.h_settings_set,
            "session_start": self.h_session_start, "session_cancel": self.h_session_cancel,
            "session_continue": self.h_session_continue, "match_end": self.h_match_end,
            "map_status": self.h_map_status, "chat": self.h_chat,
        }
        self.public = {"ping", "pong", "register", "login_begin", "login", "resume"}

    # ---- life cycle -------------------------------------------------------------------------------

    async def start(self):
        cfg = self.config
        loop = asyncio.get_running_loop()
        srv = await asyncio.start_server(self._accept, cfg.host, cfg.tcp_port, limit=P.MAX_LINE)
        self._servers.append(srv)
        self.tcp_port = srv.sockets[0].getsockname()[1]
        if cfg.tls_port and cfg.tls_cert:
            ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
            ctx.load_cert_chain(cfg.tls_cert, cfg.tls_key or None)
            tls = await asyncio.start_server(self._accept, cfg.host, cfg.tls_port, limit=P.MAX_LINE, ssl=ctx)
            self._servers.append(tls)
            self.tls_port = tls.sockets[0].getsockname()[1]
        self._udp, _ = await loop.create_datagram_endpoint(lambda: self.relay, local_addr=(cfg.host, cfg.udp_port))
        sock = self._udp.get_extra_info("socket")
        for opt in (socket.SO_RCVBUF, socket.SO_SNDBUF):
            try:
                sock.setsockopt(socket.SOL_SOCKET, opt, 4 * 1024 * 1024)
            except OSError:
                pass
        self.udp_port = sock.getsockname()[1]
        self._tasks.append(asyncio.create_task(self._housekeeping()))
        log.info("airride-master %s: control tcp/%d%s, relay udp/%d, database %s", __version__, self.tcp_port,
                 ", tls/%d" % self.tls_port if self.tls_port else "", self.udp_port, cfg.db_path)

    async def stop(self):
        for t in self._tasks:
            t.cancel()
        for srv in self._servers:
            srv.close()
        for s in list(self.sessions):
            s.bye("shutdown", "The server is restarting. Please try again in a minute.")
        self.relay.close_streams()
        for srv in self._servers:
            try:
                await asyncio.wait_for(srv.wait_closed(), 2)
            except (asyncio.TimeoutError, Exception):  # noqa: BLE001
                pass
        if self._udp:
            self._udp.close()
        self.db.close()

    async def _housekeeping(self):
        n = 0
        while True:
            await asyncio.sleep(1.0)
            n += 1
            now = time.monotonic()
            for s in list(self.sessions):
                if now - s.last_rx > IDLE_TIMEOUT:
                    log.info("%s: timed out", s)
                    s.close()
                elif s.user_id is None and now - s.connected > UNAUTH_TIMEOUT:
                    s.bye("idle", "Disconnected for inactivity.")
                elif s.proto and now - s.ping_time >= PING_INTERVAL:
                    s.ping_n += 1
                    s.ping_time = now
                    s.send({"t": "ping", "n": s.ping_n})
            for key, m in list(self.away.items()):
                if now - m.away_since > AWAY_GRACE:
                    del self.away[key]
                    self._remove_member(m, "timed out")
            self.relay.expire(now)
            if n % 60 == 0:
                self.login_fails.prune()
                self.registrations.prune()
            if n % 3600 == 0:
                await self.db.run(self.db.purge)

    # ---- connections ------------------------------------------------------------------------------

    async def _accept(self, reader, writer):
        """A new TCP connection: a relay stream if it starts with the relay magic (see relay.py),
        else the control protocol (whose first byte is '{')."""
        s = Session(self, reader, writer)
        if self.connections >= MAX_CONNECTIONS or self.per_ip[s.ip] >= MAX_PER_IP:
            s.bye("busy", "Too many connections. Please try again later.")
            return
        self.connections += 1
        self.per_ip[s.ip] += 1
        try:
            try:
                first = await asyncio.wait_for(reader.readexactly(1), IDLE_TIMEOUT)
            except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError):
                return
            if first == P.RELAY_STREAM_MAGIC[:1]:
                await self.relay.serve_stream(reader, writer, first)
                return
            self.sessions.add(s)
            await self._control(s, reader, first)
        finally:
            self._dropped(s)
            s.close()
            self.connections -= 1
            self.per_ip[s.ip] -= 1
            if self.per_ip[s.ip] <= 0:
                del self.per_ip[s.ip]

    async def _control(self, s, reader, first):
        pending = first  # the byte read to tell the protocols apart
        try:
            while not s.closed:
                try:
                    line = pending + await reader.readline()
                    pending = b""
                except (ValueError, asyncio.LimitOverrunError):
                    s.bye("bad_request", "Message too long.")
                    break
                if not line:
                    break
                s.last_rx = time.monotonic()
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    s.bye("bad_request", "Malformed message.")
                    break
                if not isinstance(msg, dict) or not isinstance(msg.get("t"), str):
                    s.bye("bad_request", "Malformed message.")
                    break
                await self._dispatch(s, msg)
        except (ConnectionError, OSError, asyncio.IncompleteReadError):
            pass
        except Exception:  # noqa: BLE001 - one bad connection must not take the server down
            log.exception("%s: error", s)

    async def _dispatch(self, s, msg):
        t = msg["t"]
        if not s.proto:
            if t != "hello":
                s.bye("bad_request", "Say hello first.")
                return
            self.h_hello(s, msg)
            return
        if not s.requests.take():
            if s.requests.tokens < -100:
                s.bye("flood", "Too many requests.")
            s.requests.tokens -= 1  # sustained flooding digs the hole deeper
            s.fail(msg, "rate_limited", "Slow down a little.")
            return
        handler = self.handlers.get(t)
        if handler is None:
            s.fail(msg, "unknown", "Unknown request '%s'." % t[:32])
            return
        if t not in self.public and s.user_id is None:
            s.fail(msg, "not_logged_in", "Please log in first.")
            return
        try:
            await handler(s, msg)
        except Fail as e:
            s.fail(msg, e.code, e.msg)
        except LobbyError as e:
            s.fail(msg, e.code, e.msg)

    def _dropped(self, s):
        if s not in self.sessions:
            return
        self.sessions.discard(s)
        if s.key and self.online.get(s.key) is s:
            del self.online[s.key]
            log.info("%s: disconnected", s)
        m = s.member
        if m is not None and m.session is s:
            # Keep their place (and relay) for a while: a dropped connection often comes back.
            m.session = None
            m.away_since = time.monotonic()
            self.away[m.key] = m
            self._changed(m.lobby)
        s.member = None

    # ---- handshake, accounts ----------------------------------------------------------------------

    def h_hello(self, s, msg):
        proto = msg.get("proto")
        if not isinstance(proto, int) or proto < P.MIN_PROTO_VERSION:
            s.bye("update_required", "This version of Kirby Air Ride: Recompiled is too old for online play. "
                                     "Please update the game.")
            return
        s.proto = min(proto, P.PROTO_VERSION)
        s.client = P.clean_text(msg.get("client", ""), 64)
        s.ping_time = time.monotonic() - PING_INTERVAL + 1.0  # the first ping a second from now (its RTT shows soon)
        s.send({"t": "welcome", "proto": P.PROTO_VERSION, "min_proto": P.MIN_PROTO_VERSION,
                "server": "%s %s" % (P.SERVER_SOFTWARE, __version__),
                "udp_port": self.config.public_udp_port or self.udp_port,
                "relay_tcp": True,  # relay streams on this TCP port (when UDP does not get through)
                "scrypt": {"n": P.SCRYPT_N, "r": P.SCRYPT_R, "p": P.SCRYPT_P},
                "motd": self.config.motd, "online": len(self.online), "lobbies": len(self.lobbies)})

    async def h_ping(self, s, msg):
        s.reply(msg)

    async def h_pong(self, s, msg):
        if msg.get("n") == s.ping_n:
            s.rtt = int((time.monotonic() - s.ping_time) * 1000)

    @staticmethod
    def _scrypt_params(msg):
        try:
            n, r, p = int(msg["n"]), int(msg["r"]), int(msg["p"])
        except (KeyError, TypeError, ValueError):
            raise Fail("bad_request", "Missing password parameters.") from None
        lim = P.SCRYPT_LIMITS
        if not (lim["n"][0] <= n <= lim["n"][1] and n & (n - 1) == 0 and lim["r"][0] <= r <= lim["r"][1]
                and lim["p"][0] <= p <= lim["p"][1]):
            raise Fail("bad_request", "Unsupported password parameters.")
        if not P.is_hex(msg.get("salt"), P.SALT_BYTES) or not P.is_hex(msg.get("verifier"), P.VERIFIER_BYTES):
            raise Fail("bad_request", "Missing password data.")
        return bytes.fromhex(msg["salt"]), n, r, p, bytes.fromhex(msg["verifier"])

    async def h_register(self, s, msg):
        name = msg.get("username")
        if not P.valid_username(name):
            raise Fail("bad_username", "Usernames are 3 to 16 letters, digits, '_', '.' or '-', "
                                       "starting with a letter or digit.")
        if P.username_key(name) in P.RESERVED_NAMES:
            raise Fail("name_taken", "That username is reserved.")
        salt, n, r, p, verifier = self._scrypt_params(msg)
        if self.registrations.count(s.ip) >= REGISTER_PER_IP:
            raise Fail("too_many_attempts", "Too many new accounts from your address. Try again later.")
        if not await self.db.run(self.db.create_user, name, salt, n, r, p, verifier):
            raise Fail("name_taken", "That username is taken.")
        self.registrations.add(s.ip)
        log.info("registered %s from %s", name, s.ip)
        s.reply(msg, username=name)

    async def h_login_begin(self, s, msg):
        name = msg.get("username")
        if not P.valid_username(name):
            raise Fail("bad_login", "Wrong username or password.")
        key = P.username_key(name)
        creds = await self.db.run(self.db.credentials, key)
        if creds:
            salt, n, r, p = creds[2], creds[3], creds[4], creds[5]
        else:  # the same answer as for a real account: login_begin does not tell who exists
            salt, n, r, p = crypto.fake_salt(self.secret, key), P.SCRYPT_N, P.SCRYPT_R, P.SCRYPT_P
        nonce = secrets.token_bytes(P.NONCE_BYTES)
        s.challenge = (key, nonce)
        s.reply(msg, salt=salt.hex(), n=n, r=r, p=p, nonce=nonce.hex())

    def _take_challenge(self, s, msg):
        name = msg.get("username")
        challenge, s.challenge = s.challenge, None
        if not P.valid_username(name) or not challenge or challenge[0] != P.username_key(name):
            raise Fail("bad_request", "Start the login again.")
        if not P.is_hex(msg.get("cnonce"), P.NONCE_BYTES) or not P.is_hex(msg.get("proof"), 32):
            raise Fail("bad_request", "Malformed login.")
        key = challenge[0]
        if self.login_fails.count(key) >= LOGIN_FAILS or self.login_fails.count(s.ip) >= 3 * LOGIN_FAILS:
            raise Fail("too_many_attempts", "Too many failed logins. Wait a few minutes and try again.")
        return key, challenge[1], bytes.fromhex(msg["cnonce"]), bytes.fromhex(msg["proof"])

    def _failed_login(self, s, key):
        self.login_fails.add(key)
        self.login_fails.add(s.ip)

    async def h_login(self, s, msg):
        key, snonce, cnonce, proof = self._take_challenge(s, msg)
        creds = await self.db.run(self.db.credentials, key)
        if not creds or not crypto.equal(crypto.login_proof(creds[6], snonce, cnonce, key), proof):
            self._failed_login(s, key)
            raise Fail("bad_login", "Wrong username or password.")
        if creds[7]:
            raise Fail("banned", "This account is suspended.")
        token, expires = await self.db.run(self.db.create_session, creds[0])
        token_enc = crypto.xor(token, crypto.token_mask(creds[6], snonce, cnonce))
        extra = await self._signed_in(s, creds[0], creds[1], token)
        log.info("login %s from %s", s.name, s.ip)
        s.reply(msg, token=token_enc.hex(), expires=expires, **extra)

    async def h_resume(self, s, msg):
        key, snonce, cnonce, proof = self._take_challenge(s, msg)
        creds = await self.db.run(self.db.credentials, key)
        token = None
        expires = 0
        if creds and not creds[7]:
            for t, e in await self.db.run(self.db.sessions_of, creds[0]):
                if crypto.equal(crypto.resume_proof(t, snonce, cnonce, key), proof):
                    token, expires = t, e
                    break
        if token is None:
            self._failed_login(s, key)
            raise Fail("bad_token", "Your saved login has expired. Please log in again.")
        extra = await self._signed_in(s, creds[0], creds[1], token)
        log.info("resume %s from %s", s.name, s.ip)
        s.reply(msg, expires=expires, **extra)

    async def _signed_in(self, s, user_id, name, token):
        """Common to login and resume. One connection per account: an older one is replaced, and
        a lobby place (left by a dropped connection or the replaced one) carries over."""
        if s.user_id is not None and s.user_id != user_id:
            self._sign_out(s)
        key = P.username_key(name)
        profile = await self.db.run(self.db.profile, key)
        await self.db.run(self.db.touch, user_id)
        s.user_id, s.name, s.key, s.token = user_id, name, key, token
        s.colour, s.machine = profile["colour"], profile["machine"]
        member = None
        old = self.online.get(key)
        if old is not None and old is not s:
            member, old.member = old.member, None
            old.bye("elsewhere", "You logged in from somewhere else.")
            if old.key == key and self.online.get(key) is old:
                del self.online[key]
        self.online[key] = s
        if member is None:
            member = self.away.pop(key, None)
        else:
            self.away.pop(key, None)
        extra = {"profile": profile}
        if member is not None and member.lobby.lid in self.lobbies:
            member.session = s
            member.away_since = None
            s.member = member
            lobby = member.lobby
            extra["lobby"] = lobby.state_dict()
            extra["relay"] = member.relay_info(self.config.public_udp_port or self.udp_port)
            extra["chat"] = list(lobby.chat)
            member.sent_peers = None  # (sent again, after the reply)
            asyncio.get_running_loop().call_soon(self._changed, lobby, s)
        return extra

    def _sign_out(self, s):
        if s.member is not None:
            self._remove_member(s.member, "left")
            s.member = None
        if s.key and self.online.get(s.key) is s:
            del self.online[s.key]
        s.user_id = s.name = s.key = s.token = None

    async def h_logout(self, s, msg):
        if s.token:
            await self.db.run(self.db.delete_session, s.token)
        log.info("logout %s", s)
        self._sign_out(s)
        s.reply(msg)

    async def h_set_password(self, s, msg):
        salt, n, r, p, verifier = self._scrypt_params(msg)
        await self.db.run(self.db.set_verifier, s.user_id, salt, n, r, p, verifier)
        s.token = None  # every saved login is revoked; the client logs in again to get a new one
        log.info("%s changed their password", s)
        s.reply(msg)

    # ---- profiles ---------------------------------------------------------------------------------

    async def h_profile_get(self, s, msg):
        name = msg.get("username") or s.name
        if not P.valid_username(name):
            raise Fail("not_found", "No such player.")
        profile = await self.db.run(self.db.profile, P.username_key(name))
        if not profile:
            raise Fail("not_found", "No such player.")
        other = self.online.get(P.username_key(name))
        profile["online"] = other is not None
        profile["lobby"] = other.member.lobby.lid if other and other.member else 0
        s.reply(msg, profile=profile)

    async def h_profile_set(self, s, msg):
        colour = msg.get("colour", s.colour)
        machine = msg.get("machine", s.machine)
        if colour not in P.COLOURS or machine not in P.MACHINES:
            raise Fail("bad_request", "Unknown colour or machine.")
        await self.db.run(self.db.set_profile, s.user_id, colour, machine)
        s.colour, s.machine = colour, machine
        if s.member:
            self._changed(s.member.lobby)
        s.reply(msg, profile=await self.db.run(self.db.profile, s.key))

    async def h_report_match(self, s, msg):
        """Stats are self-reported (the server cannot see inside a match): a player of the
        server's current or last match may report one result every MATCH_REPORT_GAP seconds."""
        m = s.member
        if m is None or m.lobby is None or m.key not in m.lobby.last_players:
            raise Fail("not_in_match", "Only a match played online counts.")
        now = time.monotonic()
        if now - s.last_report < MATCH_REPORT_GAP:
            raise Fail("rate_limited", "Match already recorded.")
        s.last_report = now
        await self.db.run(self.db.add_match, s.user_id, bool(msg.get("won")))
        s.reply(msg, profile=await self.db.run(self.db.profile, s.key))

    # ---- server browser ---------------------------------------------------------------------------

    async def h_list_servers(self, s, msg):
        mode = msg.get("mode")
        rows = [lb.summary() for lb in self.lobbies.values() if not mode or mode == "any" or lb.mode in (mode, "any")]
        rows.sort(key=lambda r: (r["members"] >= r["max"], -r["members"], r["id"]))
        s.reply(msg, servers=rows[:200], online=len(self.online))

    def _relay_id(self):
        while True:
            rid = secrets.randbits(32)
            if rid and rid not in self.relay.members:
                return rid

    def _lobby_options(self, s, msg, lobby=None):
        name = P.clean_text(msg.get("name", lobby.name if lobby else ""), P.LOBBY_NAME_MAX)
        if not name:
            name = ("%s's server" % s.name)[:P.LOBBY_NAME_MAX]
        try:
            max_members = int(msg.get("max", lobby.max if lobby else P.LOBBY_MAX_MEMBERS))
        except (TypeError, ValueError):
            raise Fail("bad_request", "Invalid size.") from None
        if not 2 <= max_members <= P.LOBBY_MAX_MEMBERS:
            raise Fail("bad_request", "A server holds 2 to 16 people.")
        if lobby and max_members < len(lobby.members):
            raise Fail("bad_request", "More people are in the server already.")
        mode = msg.get("mode", lobby.mode if lobby else "any")
        if mode not in P.MODES:
            raise Fail("bad_request", "Unknown mode.")
        region = msg.get("region", lobby.region if lobby else "")
        if not P.valid_region(region):
            raise Fail("bad_request", "Regions are up to 12 letters, digits, spaces or '-'.")
        password = msg.get("password")
        if password is not None and (not isinstance(password, str) or len(password) > P.LOBBY_PASSWORD_MAX):
            raise Fail("bad_request", "Passwords are up to 64 characters.")
        migrate = msg.get("migrate", lobby.migrate if lobby else True)
        try:
            max_players = int(msg.get("max_players", lobby.max_players if lobby else P.MATCH_DEFAULT_PLAYERS))
        except (TypeError, ValueError):
            raise Fail("bad_request", "Invalid number of players.") from None
        if not 2 <= max_players <= P.MATCH_MAX_PLAYERS:
            raise Fail("bad_request", "A match has 2 to %d players." % P.MATCH_MAX_PLAYERS)
        auto_start = msg.get("auto_start", lobby.auto_start if lobby else True)
        return name, max_members, password, mode, region.strip(), bool(migrate), max_players, bool(auto_start)

    @staticmethod
    def _settings_arg(msg, default):
        settings = msg.get("settings", default)
        if not isinstance(settings, dict):
            raise Fail("bad_request", "Settings are a JSON object.")
        if len(json.dumps(settings, separators=(",", ":"), ensure_ascii=False).encode("utf-8")) > P.SETTINGS_MAX:
            raise Fail("bad_request", "The settings are too large.")
        m = settings.get("map")
        if m is not None and (not isinstance(m, dict) or (m.get("folder") and not P.is_hex(m.get("hash"), P.MAP_HASH_HEX // 2))):
            raise Fail("bad_request", "The map in the settings has no hash.")
        return settings

    async def h_host(self, s, msg):
        name, max_members, password, mode, region, migrate, max_players, auto_start = self._lobby_options(s, msg)
        settings = self._settings_arg(msg, {})
        if len(self.lobbies) >= MAX_LOBBIES:
            raise Fail("busy", "Too many servers are open right now. Try again later.")
        if s.member is not None:
            self._remove_member(s.member, "left")
            s.member = None
        lid = self.next_lobby
        self.next_lobby += 1
        m = Member(None, s.user_id, s.name, s, self._relay_id())
        lobby = Lobby(lid, m, name, max_members, password or "", mode, region, migrate, max_players, auto_start,
                      settings)
        m.lobby = lobby
        self.lobbies[lid] = lobby
        self.relay.add(m)
        s.member = m
        lobby.sync_lan()
        log.info("%s hosts #%d '%s'", s, lid, name)
        s.reply(msg, lobby=lobby.state_dict(), relay=m.relay_info(self.config.public_udp_port or self.udp_port),
                chat=[])
        self._changed(lobby)

    async def h_join(self, s, msg):
        try:
            lid = int(msg.get("lobby"))
        except (TypeError, ValueError):
            raise Fail("bad_request", "Which server?") from None
        lobby = self.lobbies.get(lid)
        if lobby is None:
            raise Fail("not_found", "That server has closed.")
        if s.member is not None and s.member.lobby is lobby:
            raise Fail("already_in", "You are in that server already.")
        if s.key in lobby.banned:
            raise Fail("banned", "The host removed you from that server.")
        if len(lobby.members) >= lobby.max:
            raise Fail("full", "That server is full.")
        if not lobby.check_password(msg.get("password")):
            raise Fail("bad_password", "Wrong server password.")
        if s.member is not None:
            self._remove_member(s.member, "left")
            s.member = None
        m = Member(lobby, s.user_id, s.name, s, self._relay_id())
        lobby.add_member(m)
        self.relay.add(m)
        s.member = m
        self._system(lobby, "%s joined." % s.name)
        self._session(lobby)
        lobby.sync_lan()
        s.reply(msg, lobby=lobby.state_dict(), relay=m.relay_info(self.config.public_udp_port or self.udp_port),
                chat=list(lobby.chat))
        self._changed(lobby)

    def _remove_member(self, m, how):
        """Takes `m` out of their lobby (left, kicked, timed out) and tells the rest."""
        lobby = m.lobby
        self.relay.remove(m)
        if self.away.get(m.key) is m:
            del self.away[m.key]
        if lobby is None or m.key not in lobby.members:
            return
        was_host = m is lobby.host
        new_host = lobby.remove_member(m)
        if lobby.empty:
            self._close(lobby)
            log.info("#%d closed (empty)", lobby.lid)
            return
        if was_host and new_host is None:  # no migration: the lobby closes with its host
            for other in list(lobby.members.values()):
                lobby.remove_member(other)
                self.relay.remove(other)
                self.away.pop(other.key, None)
                if other.session:
                    other.session.member = None
                    other.session.send({"t": "lobby_gone", "reason": "closed", "msg": "The host closed the server."})
            self._close(lobby)
            log.info("#%d closed (host left)", lobby.lid)
            return
        self._system(lobby, "%s %s." % (m.name, {"kicked": "was removed by the host", "timed out": "lost connection"}
                                        .get(how, "left")))
        if new_host is not None:
            self._system(lobby, "%s is now the host." % new_host.name)
        self._changed(lobby)

    def _member(self, s):
        if s.member is None or s.member.lobby is None or s.member.lobby.lid not in self.lobbies:
            raise Fail("not_in_lobby", "You are not in a server.")
        return s.member

    def _host_member(self, s):
        m = self._member(s)
        if m.lobby.host is not m:
            raise Fail("not_host", "Only the host can do that.")
        return m

    async def h_leave(self, s, msg):
        m = self._member(s)
        self._remove_member(m, "left")
        s.member = None
        s.reply(msg)

    async def h_kick(self, s, msg):
        m = self._host_member(s)
        name = msg.get("username")
        target = m.lobby.members.get(P.username_key(name)) if isinstance(name, str) else None
        if target is None:
            raise Fail("not_found", "No such player in this server.")
        if target is m:
            raise Fail("bad_request", "You cannot remove yourself; leave instead.")
        m.lobby.banned.add(target.key)
        session = target.session
        self._remove_member(target, "kicked")
        if session is not None:
            session.member = None
            session.send({"t": "lobby_gone", "reason": "kicked", "msg": "The host removed you from the server."})
        s.reply(msg)

    async def h_lobby_set(self, s, msg):
        m = self._host_member(s)
        lobby = m.lobby
        name, max_members, password, mode, region, migrate, max_players, auto_start = self._lobby_options(s, msg, lobby)
        lobby.name, lobby.max, lobby.mode, lobby.region = name, max_members, mode, region
        lobby.migrate, lobby.auto_start = migrate, auto_start
        lobby.max_players = max_players
        if password is not None:  # "" removes the password
            lobby.set_password(password)
        if "settings" in msg:
            if lobby.state == MATCH:
                raise Fail("in_match", "Game settings change between matches.")
            lobby.settings = self._settings_arg(msg, {})
        self._changed(lobby)
        s.reply(msg, lobby=lobby.state_dict())

    # ---- the session ------------------------------------------------------------------------------

    async def h_settings_set(self, s, msg):
        """The host's game settings: an opaque JSON object (the game's rules), shown to everyone."""
        m = self._host_member(s)
        if m.lobby.state == MATCH:
            raise Fail("in_match", "Game settings change between matches.")
        m.lobby.settings = self._settings_arg(msg, None)
        self._changed(m.lobby)
        s.reply(msg, settings=m.lobby.settings)

    async def h_session_start(self, s, msg):
        """The host starts the countdown (or, with now, the match at once)."""
        m = self._host_member(s)
        lobby = m.lobby
        if lobby.state == MATCH:
            raise Fail("in_match", "A match is on.")
        if lobby.state == RESULTS:
            lobby.back_to_lobby()
        if len(lobby.roster()) < 2:
            raise Fail("not_enough_players", "A match needs at least 2 players.")
        if not lobby.map_ready():
            raise Fail("map_missing", "Not every player has the map yet.")
        now = time.monotonic()
        if msg.get("now"):
            lobby.begin_countdown(now, 0)
        elif lobby.state != COUNTDOWN:
            lobby.begin_countdown(now, COUNTDOWN_TIME)
        self._changed(lobby)
        s.reply(msg)

    async def h_session_cancel(self, s, msg):
        """The host stops the countdown or abandons the match; no new countdown by itself until
        they start one."""
        m = self._host_member(s)
        m.lobby.cancel()
        self._changed(m.lobby)
        s.reply(msg)

    async def h_session_continue(self, s, msg):
        """The host ends the results early."""
        m = self._host_member(s)
        if m.lobby.state == RESULTS:
            m.lobby.back_to_lobby()
            self._changed(m.lobby)
        s.reply(msg)

    async def h_match_end(self, s, msg):
        """A player's game says the match is over (with what it reports: opaque JSON). The host's
        word ends it; otherwise every player's."""
        m = self._member(s)
        lobby = m.lobby
        if lobby.state != MATCH or m.key not in lobby.last_players:
            raise Fail("not_in_match", "You are not in a match.")
        results = msg.get("results", {})
        if not isinstance(results, dict) or len(json.dumps(results).encode("utf-8")) > P.RESULTS_MAX:
            raise Fail("bad_request", "Invalid results.")
        lobby.results[m.name] = results
        lobby.ended_by.add(m.key)
        if m is lobby.host or {x.key for x in lobby.roster()} <= lobby.ended_by:
            lobby.end_match(time.monotonic(), RESULTS_TIME)
            log.info("#%d match %d over", lobby.lid, lobby.match_no)
        self._changed(lobby)
        s.reply(msg)

    async def h_map_status(self, s, msg):
        """The City Trial map this member's console has (its hash; "": none). A match on a map starts only when
        every player of it has that map."""
        m = self._member(s)
        ok = msg.get("map_ok", "")
        if ok and not P.is_hex(ok, P.MAP_HASH_HEX // 2):
            raise Fail("bad_request", "Invalid map hash.")
        ok = ok.lower()
        if ok != m.map_ok:
            m.map_ok = ok
            self._changed(m.lobby)
        s.reply(msg)

    async def h_peer_info(self, s, msg):
        """Where this console can be reached directly (its LAN addresses, its public endpoint from
        STUN) and its MAC: passed on to the other players for peer-to-peer play."""
        m = self._member(s)
        mac = msg.get("mac", m.mac)
        if mac and not P.is_hex(mac, 6):
            raise Fail("bad_request", "Invalid MAC address.")
        raw = msg.get("candidates", [])
        if not isinstance(raw, list) or len(raw) > P.CANDIDATES_MAX:
            raise Fail("bad_request", "Invalid addresses.")
        candidates = []
        for c in raw:
            try:
                ip = str(ipaddress.ip_address(c["ip"]))
                port = int(c["port"])
                kind = c["type"]
            except (KeyError, TypeError, ValueError):
                raise Fail("bad_request", "Invalid addresses.") from None
            if kind not in P.CANDIDATE_TYPES or not 1 <= port <= 65535:
                raise Fail("bad_request", "Invalid addresses.")
            candidates.append({"type": kind, "ip": ip, "port": port})
        # The address the master sees, with the console's own port: right behind NATs that keep
        # ports, and a STUN-less fallback.
        seen = self._seen_ip(s.ip)
        lan_ports = [c["port"] for c in candidates if c["type"] == "lan"]
        if seen and lan_ports and not any(c["ip"] == seen and c["port"] == lan_ports[0] for c in candidates):
            candidates.append({"type": "seen", "ip": seen, "port": lan_ports[0]})
        m.mac = mac.lower() if mac else ""
        m.candidates = candidates
        self._send_peers(m.lobby)
        s.reply(msg)

    @staticmethod
    def _seen_ip(ip):
        try:
            a = ipaddress.ip_address(ip)
        except ValueError:
            return None
        if a.version == 6 and a.ipv4_mapped:
            a = a.ipv4_mapped
        return str(a)

    def _session(self, lobby):
        """The session's automatic steps: countdown once 2+ are in (and every player has the settings' map), the
        match when it runs out, results when a match is down to one player, the lobby again when results time is up."""
        now = time.monotonic()
        for _ in range(4):  # one step may enable the next (results over -> countdown)
            players = len(lobby.roster())
            st = lobby.state
            if st == LOBBY and lobby.auto_start and not lobby.hold and players >= 2 and lobby.map_ready():
                lobby.begin_countdown(now, COUNTDOWN_TIME)
            elif st == COUNTDOWN and (players < 2 or not lobby.map_ready()):
                lobby.back_to_lobby()
            elif st == COUNTDOWN and now >= lobby.deadline:
                self._start_match(lobby)
            elif st == MATCH and players < 2:
                lobby.end_match(now, RESULTS_TIME)
            elif st == RESULTS and now >= lobby.deadline:
                lobby.back_to_lobby()
            else:
                break
        if lobby.timer is not None:
            lobby.timer.cancel()
            lobby.timer = None
        if lobby.deadline is not None:
            lobby.timer = asyncio.get_running_loop().call_later(max(0.0, lobby.deadline - now) + 0.005,
                                                                self._timer, lobby.lid)

    def _timer(self, lid):
        lobby = self.lobbies.get(lid)
        if lobby is not None:
            lobby.timer = None
            self._changed(lobby)

    def _start_match(self, lobby):
        start_at = int(time.time() * 1000 + START_LEAD * 1000)
        match = lobby.start_match(start_at, secrets.randbits(32))
        lobby.sync_lan()
        event = {"t": "match_start", "match": {
            "id": match["id"], "seed": match["seed"], "start_at": start_at, "start_in_ms": int(START_LEAD * 1000),
            "mode": lobby.mode, "settings": match["settings"],
            "players": [{"console": i, "name": p.name, "peer": p.relay_id, "mac": p.mac}
                        for i, p in enumerate(match["players"])]}}
        for p in match["players"]:
            if p.session:
                p.session.send(event)
        log.info("#%d match %d: %s", lobby.lid, match["id"], ", ".join(p.name for p in match["players"]))

    def _close(self, lobby):
        if lobby.timer is not None:
            lobby.timer.cancel()
            lobby.timer = None
        self.lobbies.pop(lobby.lid, None)

    # ---- chat -------------------------------------------------------------------------------------

    async def h_chat(self, s, msg):
        m = self._member(s)
        text = P.clean_text(msg.get("text"), P.CHAT_MAX)
        if not text:
            raise Fail("bad_request", "Nothing to say?")
        if not s.chat.take():
            raise Fail("rate_limited", "You are sending messages too quickly.")
        line = {"from": s.name, "text": text, "time": int(time.time())}
        m.lobby.chat.append(line)
        for other in m.lobby.members.values():
            if other.session:
                other.session.send({"t": "chat", **line})
        s.reply(msg)

    def _system(self, lobby, text):
        line = {"from": "", "text": text, "time": int(time.time()), "sys": True}
        lobby.chat.append(line)
        for m in lobby.members.values():
            if m.session:
                m.session.send({"t": "chat", **line})

    def _changed(self, lobby, skip=None):
        """After any change: the session's automatic steps, the roster's LAN, and the new state to
        everyone (and peer lists to the roster)."""
        if lobby is None or lobby.lid not in self.lobbies:
            return
        self._session(lobby)
        lobby.sync_lan()
        event = {"t": "lobby", "lobby": lobby.state_dict()}
        for m in lobby.members.values():
            if m.session and m.session is not skip:
                m.session.send(event)
        self._send_peers(lobby)

    def _send_peers(self, lobby):
        """Each roster console gets the others (ids, MACs, keys, addresses); the rest get none.
        Sent only when a console's list changes."""
        roster = lobby.lan.members
        for m in lobby.members.values():
            peers = [o.peer_entry() for o in roster if o is not m] if m.lan is lobby.lan else []
            key = json.dumps(peers, sort_keys=True)
            if m.session and key != m.sent_peers:
                m.sent_peers = key
                m.session.send({"t": "peers", "peers": peers})
