"""Hosted servers ("lobbies"): their members, and the one session each runs.

A server holds up to 16 members, and its members are the session: joining puts you in it. The
session goes round

    Lobby (gathering) -> Countdown (by itself once 2+ players are in; the host can start at once
    or cancel) -> InMatch (locked: newcomers wait for the next match) -> Results -> Lobby ...

A match has at most `max_players` consoles (2-8; 4 unless the host says: the game's own LAN takes 4, the
8-player build 8). The *roster* is who plays: before a match, the first `max_players` members in join order;
during a match and its results, the match's players. The roster is one virtual LAN: the relay (and
peer-to-peer traffic between the clients) joins exactly its consoles. Everyone else waits.

Plain data and rules: no I/O here. The control layer (server.py) drives the timers, calls these and
then broadcasts the new state.
"""

import collections
import hashlib
import secrets
import time

from . import protocol as P

LOBBY, COUNTDOWN, MATCH, RESULTS = "lobby", "countdown", "match", "results"


class LobbyError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code
        self.msg = msg


class Member:
    """One account in one lobby. Outlives a dropped TCP connection for a grace period."""

    __slots__ = ("lobby", "user_id", "name", "key", "session", "joined", "away_since",
                 "relay_id", "relay_secret", "p2p_key", "mac", "candidates", "sent_peers",
                 "addr", "stream", "last_relay", "lan",
                 "tokens", "tokens_time", "frames_in", "frames_out", "dropped")

    def __init__(self, lobby, user_id, name, session, relay_id):
        self.lobby = lobby
        self.user_id = user_id
        self.name = name
        self.key = P.username_key(name)
        self.session = session          # ControlSession, or None while away
        self.joined = time.monotonic()
        self.away_since = None
        self.relay_id = relay_id        # also the member's peer id (it is in every frame they relay)
        self.relay_secret = secrets.token_bytes(P.RELAY_SECRET_BYTES)
        self.p2p_key = secrets.token_bytes(P.P2P_KEY_BYTES)  # proves peer-to-peer packets are theirs
        self.mac = ""                   # the console's MAC (hex), from peer_info
        self.candidates = []            # where peers may reach it directly, from peer_info
        self.sent_peers = None          # the last peers list sent to it (to send only changes)
        self.addr = None                # relay: UDP endpoint, once a HELLO with the secret arrived ...
        self.stream = None              # ... or the relay.Stream it arrived on (TCP relay)
        self.last_relay = 0.0
        self.lan = None                 # the lobby's Lan while on the roster
        self.tokens = float(P.RELAY_BURST)
        self.tokens_time = time.monotonic()
        self.frames_in = 0
        self.frames_out = 0
        self.dropped = 0

    def relay_info(self, udp_port):
        return {"id": self.relay_id, "secret": self.relay_secret.hex(), "port": udp_port,
                "key": self.p2p_key.hex()}

    def peer_entry(self):
        return {"id": self.relay_id, "name": self.name, "mac": self.mac, "key": self.p2p_key.hex(),
                "candidates": self.candidates}


class Lan:
    """The roster's virtual LAN (what the relay forwards within)."""

    __slots__ = ("members", "macs")

    def __init__(self):
        self.members = []      # [Member], in console order
        self.macs = {}         # source MAC (6 bytes) -> Member, learnt from relayed frames

    def add(self, m):
        self.members.append(m)
        m.lan = self

    def remove(self, m):
        self.members.remove(m)
        m.lan = None
        for mac in [mac for mac, who in self.macs.items() if who is m]:
            del self.macs[mac]


def _password_hash(lid, password):
    return hashlib.sha256(b"airride-lobby\0%d\0" % lid + password.encode("utf-8")).digest()


class Lobby:
    def __init__(self, lid, host, name, max_members, password, mode, region, migrate,
                 max_players=P.MATCH_DEFAULT_PLAYERS, auto_start=True, settings=None):
        self.lid = lid
        self.name = name
        self.max = max_members
        self.password = None
        self.set_password(password)
        self.mode = mode
        self.region = region
        self.migrate = migrate           # a new host when the host leaves (else the lobby closes)
        self.max_players = max_players   # consoles in a match
        self.auto_start = auto_start     # the countdown starts by itself when 2+ are in
        self.settings = settings or {}   # the host's game settings (opaque to the server)
        self.created = time.time()
        self.members = {}                # key -> Member, in join order
        self.banned = set()              # keys the host kicked
        self.chat = collections.deque(maxlen=P.CHAT_HISTORY)
        self.host = None
        self.lan = Lan()
        # the session
        self.state = LOBBY
        self.hold = False                # the host cancelled: no countdown by itself until they start
        self.deadline = None             # monotonic time the countdown / results end
        self.match = None                # {"id", "seed", "players": [Member], "settings", "start_at"}
        self.match_no = 0
        self.last_players = set()        # keys of the last match's players (may report its result)
        self.ended_by = set()            # keys of match players who reported the match over
        self.results = {}                # name -> what their client reported
        self.timer = None                # asyncio handle (server.py)
        self.add_member(host)
        self.host = host

    # ---- membership -------------------------------------------------------------------------------

    def set_password(self, password):
        """"" (or None): open to everyone."""
        self.password = _password_hash(self.lid, password) if password else None

    def check_password(self, password):
        if self.password is None:
            return True
        return isinstance(password, str) and secrets.compare_digest(_password_hash(self.lid, password), self.password)

    def add_member(self, m):
        self.members[m.key] = m

    def remove_member(self, m):
        """Removes `m`; returns the new host if the host changed (None otherwise)."""
        if m.lan is not None:
            m.lan.remove(m)
        self.members.pop(m.key, None)
        if m is self.host and self.members:
            if not self.migrate:
                return None
            present = [x for x in self.members.values() if x.session is not None] or list(self.members.values())
            self.host = min(present, key=lambda x: x.joined)
            return self.host
        return None

    @property
    def empty(self):
        return not self.members

    # ---- the session ------------------------------------------------------------------------------

    def roster(self):
        """Who plays: the match's players (still here) during a match and its results, else the
        first max_players members."""
        if self.state in (MATCH, RESULTS) and self.match is not None:
            return [m for m in self.match["players"] if self.members.get(m.key) is m]
        return list(self.members.values())[:self.max_players]

    def sync_lan(self):
        """The LAN holds the roster; True if that changed it."""
        wanted = self.roster()
        changed = False
        for m in list(self.lan.members):
            if m not in wanted:
                self.lan.remove(m)
                changed = True
        for m in wanted:
            if m.lan is not self.lan:
                self.lan.add(m)
                changed = True
        if changed:  # console order = roster order
            self.lan.members.sort(key=wanted.index)
        return changed

    def begin_countdown(self, now, seconds):
        self.state = COUNTDOWN
        self.deadline = now + seconds
        self.hold = False

    def cancel(self):
        """The host stops the countdown, or abandons the match: back to the lobby, and no new
        countdown by itself until the host starts one."""
        self.state = LOBBY
        self.deadline = None
        self.match = None
        self.hold = True

    def start_match(self, start_at, seed):
        players = self.roster()
        self.match_no += 1
        self.state = MATCH
        self.deadline = None
        self.ended_by = set()
        self.results = {}
        self.last_players = {m.key for m in players}
        self.match = {"id": self.match_no, "seed": seed, "players": players, "settings": dict(self.settings),
                      "start_at": start_at}
        return self.match

    def end_match(self, now, seconds):
        self.state = RESULTS
        self.deadline = now + seconds

    def back_to_lobby(self):
        self.state = LOBBY
        self.deadline = None
        self.match = None

    # ---- state ------------------------------------------------------------------------------------

    def summary(self):
        """One row of the server browser."""
        host_rtt = self.host.session.rtt if self.host and self.host.session else -1
        return {"id": self.lid, "name": self.name, "host": self.host.name if self.host else "",
                "members": len(self.members), "max": self.max, "locked": self.password is not None,
                "mode": self.mode, "region": self.region, "state": self.state,
                "max_players": self.max_players,
                "playing": len(self.roster()) if self.state == MATCH else 0,
                "ping": host_rtt}

    def state_dict(self):
        """Everything a member's screen shows."""
        now = time.monotonic()
        roster = self.roster()
        members = []
        for m in self.members.values():
            s = m.session
            members.append({
                "name": m.name,
                "colour": s.colour if s else "pink",
                "machine": s.machine if s else "",
                "host": m is self.host,
                "player": m in roster,
                "peer": m.relay_id,
                "ping": s.rtt if s else -1,
                "relay": m.addr is not None or m.stream is not None,
                "relay_tcp": m.stream is not None,
                "away": s is None,
            })
        match = None
        if self.match is not None:
            match = {"id": self.match["id"], "players": [m.name for m in self.match["players"]]}
        remaining = max(0, int((self.deadline - now) * 1000)) if self.deadline is not None else 0
        return {"id": self.lid, "name": self.name, "host": self.host.name if self.host else "",
                "max": self.max, "max_players": self.max_players, "locked": self.password is not None,
                "mode": self.mode, "region": self.region, "migrate": self.migrate,
                "auto_start": self.auto_start, "state": self.state, "hold": self.hold,
                "countdown_ms": remaining if self.state == COUNTDOWN else 0,
                "results_ms": remaining if self.state == RESULTS else 0,
                "match": match, "results": self.results if self.state == RESULTS else {},
                "settings": self.settings, "members": members}
