# Online play

Kirby Air Ride's own LAN mode (Air Ride and City Trial, up to 4 consoles per match - the game's
limit; up to 8 players in City Trial with the 8-rider build, PCs 5-8 on spare controller ports of
the first two consoles: [MORE_PLAYERS.md](MORE_PLAYERS.md)), played over the internet: a master server for accounts, the server browser and sessions,
and the game's LAN frames peer to peer between the players where a direct path works, through the
master's relay where not.

## Official service and dedicated servers

The same server program (`server/`) runs both. The **official service** is the one a build is made
for (its address set when building, never in the source). A **dedicated server** is one anyone runs
for themselves or a group (`python -m airride_master`, see `server/DEPLOY.md`): players choose it in
**Online > Settings > Service: Dedicated Server** and type its address. Each has its own accounts; the
game keeps a remembered login per server.

## Architecture

```
 game (LAN mode, its own IP stack)                         master server (VPS, Python)
   | ETH library replaced by host code                      +--------------------------------+
 src/runtime/lan.cpp  -- rt::lan::Port -->                  | control  TCP, JSON lines       |
 src/runtime/airnet.cpp                                     |   accounts, profiles (SQLite)  |
   |  control: one TCP connection  ---------------------->  |   server browser, sessions,    |
   |  relay:   UDP datagrams, or a TCP stream ----------->  |   peer candidates, chat        |
   |           (to the consoles with no direct path)        | relay    UDP, or TCP on the    |
   |  STUN: this console's public address (UDP)             |          control port          |
   |                                                        +--------------------------------+
   |  peer to peer: UDP straight to the other consoles (LAN address > public address)
 the menu (UI) reads rt::airnet::Snapshot each frame
```

- **Accounts** have a username (3-16 of `A-Z a-z 0-9 _ . -`, unique regardless of case) and a
  profile: Kirby's colour, a favourite machine, matches played and won.
- **Servers** are hosted by any player and listed by the master: name, host, members out of at
  most 16, password flag, mode preference (Any / Air Ride / City Trial), region, the host's ping to
  the master, the session's state and the consoles in a match.
- **A server is one session**: joining puts you in it. The first `max_players` members (4, the
  game's LAN limit; a per-server value so it can rise later) are the *roster*: the consoles of the
  next match, joined into one virtual LAN at once. Members beyond that, and anyone who joins during a
  match, wait for the next one.
- **The session** goes round: **Lobby** (gathering) -> **Countdown** (starts by itself once 2
  players are in, 5 s; the host can start at once, or cancel - which holds the automatic countdown
  until the host starts one) -> **InMatch** (locked; the master sends the players a *match start*:
  console order, settings, a shared seed, a start time) -> **Results** (when the host's game
  reports the match over, or every player's; 10 s, or until the host continues) -> Lobby again, with
  whoever is there now. A match down to one player ends.
- **Game settings**: an opaque JSON object per server (the mode and its rules; the game's fields
  are defined by the game side), up to 4 KiB, set by the host between matches, seen by everyone,
  copied into each match start.
- **Host powers**: kick (the kicked player cannot rejoin that server), the server's settings, the
  game settings, start / cancel / continue. When the host leaves, the longest-present member becomes
  host (or, if the host chose so, the server closes).
- **Dropped connections**: a member whose TCP connection drops keeps their place (and their relay)
  for 30 seconds. The client reconnects by itself and resumes; it lands back in the same server and
  roster, and the LAN never stopped meanwhile (UDP and peer-to-peer traffic do not depend on the
  control connection), so a match survives a short TCP outage.

The game's side: `src/runtime/lan.cpp` (another part of the port) replaces the game's broadband
adapter driver and hands whole Ethernet frames to the installed `rt::lan::Port`. While the player
is on the roster, `rt::airnet::client()` installs its network as that port (the adapter reports its
cable plugged in once a path works); off the roster it uninstalls it (no cable).

## Files

| Path | What |
|------|------|
| `server/airride_master/` | the master server (`python3 -m airride_master`) |
| `server/airride_master/protocol.py` | constants and validation (keep in step with airnet.h) |
| `server/airride_master/server.py` | control connections, accounts, servers, the session's steps, chat |
| `server/airride_master/lobby.py` | server / session rules: roster, states (no I/O) |
| `server/airride_master/relay.py` | the relay (UDP and TCP) |
| `server/airride_master/db.py` | SQLite: accounts, profiles, sessions |
| `server/airride_master/crypto.py` | login cryptography |
| `server/airride_master/client.py` | a reference client in Python (tests, debugging) |
| `server/tests/` | the test suite; `tests/cpp/airnet_test.cpp` drives the C++ client; `tests/natsim.py` simulates NATs and a STUN server |
| `server/DEPLOY.md`, `server/airride-master.service`, `server/run.sh` | running it on a VPS |
| `src/runtime/airnet.h`, `airnet.cpp` | the game's client |
| `src/runtime/settings.*` | `online_master`, `online_server`, `online_user`, `online_token`, `online_token_server`, `online_relay_tcp`, `online_p2p`, `online_stun` |

## Control protocol (TCP, default port 27600)

One JSON object per line (UTF-8, `\n`-terminated, at most 8 KiB). The client sends requests with an
`id`; the server answers each with a `reply` carrying the same `id`, and also sends events.

```
-> {"t":"login_begin","id":7,"username":"meta"}
<- {"t":"reply","id":7,"ok":true,"salt":"…","n":16384,"r":8,"p":1,"nonce":"…"}
<- {"t":"reply","id":8,"ok":false,"err":"bad_login","msg":"Wrong username or password."}
```

`msg` is always a sentence to show the player; `err` is a stable code for program logic.

### Handshake and versions

The first message must be `{"t":"hello","proto":2,"client":"…"}`. The server answers
`{"t":"welcome","proto":2,"min_proto":2,"server":"airride-master 1.0.0","udp_port":27601,
"scrypt":{"n":16384,"r":8,"p":1},"relay_tcp":true,"motd":"…","online":N,"lobbies":N}` (`relay_tcp`:
the control port also takes relay streams). A client whose `proto` is below the server's
`MIN_PROTO_VERSION` instead gets `{"t":"bye","code":"update_required","msg":"…"}` and the connection
closes; the game shows that message (`Snapshot::updateRequired`). Protocol 2 (sessions, peer to
peer) replaced 1 (match groups): clients speaking 1 are told to update.

### Requests

| `t` | Fields | Reply fields | Notes |
|-----|--------|--------------|-------|
| `ping` | | | round-trip check |
| `register` | `username`, `salt`, `n`, `r`, `p`, `verifier` | `username` | see Accounts |
| `login_begin` | `username` | `salt`, `n`, `r`, `p`, `nonce` | one nonce per attempt |
| `login` | `username`, `cnonce`, `proof` | `token` (masked), `expires`, `profile`, [`lobby`, `relay`, `chat`] | |
| `resume` | `username`, `cnonce`, `proof` | `expires`, `profile`, [`lobby`, `relay`, `chat`] | log in with a session token |
| `logout` | | | ends this session (token revoked) |
| `set_password` | `salt`, `n`, `r`, `p`, `verifier` | | revokes every session |
| `profile_get` | [`username`] | `profile` (+ `online`, `lobby`) | own profile without `username` |
| `profile_set` | [`colour`], [`machine`] | `profile` | ids from protocol.py |
| `report_match` | `won` | `profile` | a player of the current / last match; one per 30 s |
| `list_servers` | [`mode`] | `servers`, `online` | |
| `host` | `name`, `max`, `max_players`, `password`, `mode`, `region`, `auto_start`, `migrate`, `settings` (all optional) | `lobby`, `relay`, `chat` | leaves any current server |
| `join` | `lobby`, [`password`] | `lobby`, `relay`, `chat` | you are in the session at once |
| `leave` | | | |
| `kick` | `username` | | host |
| `lobby_set` | any `host` field | `lobby` | host; `password:""` opens it |
| `settings_set` | `settings` (object) | `settings` | host; not during a match |
| `session_start` | [`now`] | | host: the countdown (`now`: the match at once); 2+ players |
| `session_cancel` | | | host: stop the countdown / abandon the match; holds the automatic countdown |
| `session_continue` | | | host: end the results early |
| `match_end` | [`results`] (object) | | a player's game says the match is over |
| `peer_info` | `mac` (12 hex), `candidates` | | where this console can be reached (see Peer to peer) |
| `chat` | `text` | | 200 characters; 5 lines per 10 s |

Every request but `ping`, `register`, `login_begin`, `login` and `resume` needs a login. Requests
are rate-limited (20 a second, bursts of 60).

### Events (server to client)

| `t` | Fields | When |
|-----|--------|------|
| `ping` | `n` | every 5 s (the first 1 s after hello); the client answers `{"t":"pong","n":…}` (the server measures ping; 30 s of silence closes the connection) |
| `lobby` | `lobby` | anything in the server or session changed (the whole state: it is small) |
| `peers` | `peers` | the roster's other consoles, to each roster console (empty for those waiting) |
| `match_start` | `match` | to the match's players, when the countdown runs out (or the host starts now) |
| `chat` | `from`, `text`, `time`, [`sys`] | a chat line (`sys`: joins, leaves, host changes) |
| `lobby_gone` | `reason` (`kicked`, `closed`), `msg` | removed from the server |
| `bye` | `code`, `msg` | the server closes the connection: `update_required`, `elsewhere` (the account logged in elsewhere: one connection per account), `shutdown`, `busy`, `bad_request`, `flood`, `idle` |

A server's state (`lobby`): `{"id","name","host","max","max_players","locked","mode","region",
"migrate","auto_start","state":"lobby|countdown|match|results","hold","countdown_ms","results_ms",
"match":{"id","players":[names]}|null,"results":{name: object},"settings":{…},
"members":[{"name","colour","machine","host","player","peer","ping","relay","relay_tcp","away"}]}`
(`player`: on the roster; `peer`: their peer id; `relay`: the relay hears from them; `relay_tcp`:
over TCP; `away`: connection dropped, place kept; `countdown_ms` / `results_ms`: time left).

`relay`: `{"id":<u32>,"secret":"<16 hex>","port":27601,"key":"<16 hex>"}` - this member's relay
credentials, and its peer-to-peer key. The relay id is also its *peer id*.

`peers`: `[{"id","name","mac","key","candidates":[{"type":"lan|public|seen","ip","port"}]}]`
(`seen`: added by the master - the address it sees the console's control connection come from,
with the console's own UDP port: right behind NATs that keep ports).

`match_start`: `{"id","seed","start_at" (master's unix ms),"start_in_ms","mode","settings",
"players":[{"console":0..n-1,"name","peer","mac"}]}` - the same for every player. The client turns
`start_in_ms` into its own clock (`MatchStart::startAtMs`, less half its round trip).

## Peer to peer (UDP between consoles)

1. **Candidates.** On entering a server, each client gathers its LAN addresses (IPv4: the interface
   toward the master and toward the internet - a UDP "connect", nothing is sent - and its host
   name's addresses) with its UDP socket's port, and asks STUN servers (`online_stun`, default
   `stun.l.google.com:19302,stun.cloudflare.com:3478`; a minimal RFC 5389 Binding request from the
   same socket) for its public address. It sends them to the master in `peer_info` (again whenever
   STUN's answer changes; STUN is asked again every 30 s). The master hands each roster console the
   others' (`peers`).
2. **Hole punching.** Each client pings every candidate of every peer: every 200 ms for the first
   10 s, every 2 s after (keepalive; it also keeps NAT mappings open). Both ends do it at once, so a
   cone NAT on either side lets the other's packets in once it has sent out. A ping from an address
   not on the list is answered and that address tried too (a peer-reflexive candidate).
3. **Paths.** A candidate *works* once a pong comes back from it. Per peer the client uses the best
   working one: a LAN address over a public one, then the lower round trip (by more than 5 ms, so
   it does not flap). No pong for 6.5 s: the path is gone, frames to that peer go through the relay
   until one works again. The round trip is measured on every pong (`Member::peerRttMs`).
4. **Frames.** Each frame goes to each console that should get it exactly once: a unicast frame to
   the console whose MAC it names, broadcasts and multicasts to every peer - directly where a path
   works, else through the relay in one `FRAME_TO` naming those peers. A console with peer to peer
   off (`online_p2p = 0`), or with no list yet, sends plain `FRAME`s (the relay forwards them to
   the whole roster). Mixed paths in one match work: in the tests two consoles talk directly while
   a third, behind a symmetric NAT, reaches both through the relay.
5. **Security.** Peer packets carry the sender's peer id; pings and pongs carry its 8-byte key (from
   the master, to the roster only) and the receiver's id. Frames are taken only from an address that
   answered this client's ping as that peer.

Packets (the same 8-byte header as the relay: type, version 1, 0, 0, sender's peer id, big-endian):

| Type | Body |
|------|------|
| `0x10` PING | sender's key (8) + echo (u32) + receiver's peer id (u32) |
| `0x11` PONG | sender's key (8) + the ping's echo (u32) + receiver's peer id (u32) |
| `0x12` FRAME | one Ethernet frame |

The client's link (`Snapshot::linkUp`, the adapter's cable) is up while it is on the roster and
either the relay answers or every peer has a direct path.

Not done (yet): IPv6 candidates (IPv4 only), TURN-style relays other than the master, and
port-prediction tricks for symmetric NATs (those consoles use the relay).

## Relay protocol (UDP, default port 27601)

Every datagram: an 8-byte header, then the body.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | type: `0x01` HELLO, `0x02` FRAME, `0x03` FRAME_TO, `0x81` HELLO_ACK |
| 1 | 1 | version (1) |
| 2 | 2 | reserved (0) |
| 4 | 4 | relay id, big-endian (from the control connection) |

- **HELLO** (client to server): body = secret (8 bytes) + echo (u32). Binds the sender's address
  (IP and port, as the server sees it - through any NAT) to the relay id, and keeps the NAT mapping
  open. Sent every second while in a server (every 250 ms until the first answer). A new address
  with the right secret moves the binding (NAT rebinding, network change).
- **HELLO_ACK** (server to client): body = the echo. The client's round-trip time (`pingMs`).
- **FRAME** (both ways): body = one Ethernet frame as the game built it (destination MAC, source
  MAC, type, payload; 14-1514 bytes). Accepted only from the address bound to the relay id, from a
  roster console, within its rate limit (1500 a second, bursts of 3000). Forwarded unchanged (the
  receiver ignores the id) to the roster's other consoles: a broadcast or multicast destination goes
  to all of them; a unicast destination goes to the console that MAC was last seen sending from, or
  to all of them while it is unknown (as a switch does).
- **FRAME_TO** (client to server): body = n (u8, 1-16) + n relay ids (u32 each) + one frame. The
  frame goes, as a FRAME, to those of them on the sender's roster only (the others got it peer to
  peer).
- An address silent for 15 s is forgotten (the next HELLO restores it).

### The relay over TCP (when UDP does not get through)

Some networks let no UDP through (the first VPS's provider firewall passes only a few TCP ports).
The same messages then go over TCP, on the control port: a relay connection starts with the 8 bytes
`AIRRELAY` (a control connection starts with `{`, which is how the server tells them apart), then
carries the messages above, each preceded by its length (u16 big-endian). The first must be a HELLO
with the secret (within 10 s), or the server hangs up; one member per connection.

- A member is bound to one transport at a time: the one its latest valid HELLO came by (a UDP HELLO
  closes the member's stream; a stream HELLO forgets its UDP address). Forwarding treats both alike:
  a frame from a TCP member reaches UDP members by UDP and the other way round.
- Server side: TCP_NODELAY, a 64 KiB kernel send buffer, and at most 64 KiB queued per stream - past
  that, frames to that member are dropped (an old LAN frame is worthless; seconds of them queued
  would be worse than losing them), and a stream backed up for 5 s is closed. A stream silent for
  15 s is closed (the client sends a HELLO every second).
- Client side: UDP first. With no HELLO_ACK within 1.5 s of entering a server (and a master that
  advertises `relay_tcp`), the client opens the stream to the master's address and port and uses it
  from then on; a master whose UDP failed once gets TCP straight away next time.
  `online_relay_tcp = 1` in the settings file (or `Client::forceRelayTcp`) forces TCP. The game's
  thread still never blocks: a frame is written to the socket at once if nothing is waiting, else
  queued (64 KiB at most, then dropped) for the network thread. A broken stream reconnects every
  second; the link stays up for 10 s across that, as with UDP. `Snapshot::relayTcp` says which is in
  use (and `Member::relayTcp` for the others).

With the relay over TCP, players with a direct path between them do not touch it at all: only
consoles with no direct path pay its extra hop and TCP's head-of-line stalls under loss.

### Measured (localhost, C++ clients; a frame from A to B and an answer back, 200 times)

| Path | Median | p95 |
|------|--------|-----|
| peer to peer (LAN address) | 0.06-0.09 ms | 0.15-0.36 ms |
| relay, both UDP | 0.24-0.37 ms | 0.43-1.3 ms |
| relay, one UDP and one TCP | 0.30-0.43 ms | 0.54-1.9 ms |
| relay, both TCP | 0.28-0.31 ms | 0.48-0.82 ms |
| peer to peer through two simulated NATs (Python NAT simulator in the path) | 0.31-0.46 ms | 0.5-8 ms |
| through the relay (TCP) from behind a symmetric NAT | 0.30-0.57 ms | 0.5-8 ms |

Over the internet these are the players' pings: peer to peer, one trip between the two; through
the relay, both players' trips to the master.

No buffering anywhere on the path: the client sends each frame from the game's thread the moment
the game sends it (one `sendto` per destination, built on the stack); received frames wait in a
64-frame ring until the game polls (the oldest is dropped if it overflows); the server forwards each
datagram as it arrives.

## Accounts and security

The password never leaves the player's PC, and the server never stores it:

1. **Register**: the client picks a random 16-byte `salt` and computes
   `verifier = scrypt(password, salt, N=16384, r=8, p=1, 32 bytes)` (parameters from `welcome`),
   then sends `username, salt, n, r, p, verifier`. The server stores the verifier.
2. **Log in**: `login_begin` returns the account's salt and parameters and a fresh 16-byte server
   nonce (for unknown usernames, a stable fake salt: probing does not reveal who exists). The
   client recomputes the verifier and sends a fresh client nonce and
   `proof = HMAC-SHA256(verifier, "airride-login\0" ‖ server_nonce ‖ client_nonce ‖ lower(username))`.
   Each server nonce is good for one attempt (no replays). The reply's session token (24 bytes) is
   masked: `token = token_enc XOR HMAC-SHA256(verifier, "airride-token\0" ‖ server_nonce ‖ client_nonce)[:24]`.
3. **Resume** (saved login, or a reconnect): `login_begin` for a nonce, then
   `proof = HMAC-SHA256(token, "airride-resume\0" ‖ server_nonce ‖ client_nonce ‖ lower(username))`.
   The token itself never crosses the wire after it is issued. Tokens last 30 days; an account keeps
   its 8 newest; logout and password changes revoke them.

Failed logins: 10 per account (30 per address) within 10 minutes lock further attempts for the
rest of the window. New accounts: 5 per address per hour.

**Why challenge-response and not TLS (yet)**: it needs no certificates or TLS library in the game,
and a passive eavesdropper on a login sees neither the password nor anything replayable. What it
does not protect, and TLS would:
- Registration and password changes send the *verifier*. An eavesdropper there could log in to
  this service as that player (not learn the password, which scrypt protects, nor use it elsewhere).
- The rest of the control traffic (chat, lobby passwords, server lists, peer addresses and keys) is
  readable and an active man in the middle could tamper with it. Lobby passwords are casual
  "friends only" locks.
- An eavesdropper on a login can try to guess the password offline (each guess costs a scrypt).

The plan for TLS: the server already serves the same protocol over TLS on an extra port
(`--tls-port/--tls-cert/--tls-key`, see server/DEPLOY.md); the client's transport is one socket in
`airnet.cpp` (`openTcp`, `flush`, `readTcp`) where a TLS layer (SChannel on Windows, OpenSSL on
Linux) would wrap the byte stream. It needs a domain name with a certificate.

The relay: a relay id alone is not enough to send as someone (the id appears in forwarded frames).
Binding an address needs the 8-byte secret, which only that member's control connection receives;
frames are accepted only from the bound address. The roster's consoles see each other's frames, as
on a real LAN, and (peer to peer) each other's addresses.

## The client API (src/runtime/airnet.h)

Everything is non-blocking: calls queue work for the client's thread, results land in a
`Snapshot`. A per-frame ImGui loop:

```cpp
static rt::airnet::Snapshot s;
static uint32_t matchesSeen = 0;
auto& net = rt::airnet::client();          // the game's client (installs the LAN port)
net.snapshot(s);                           // copies only when something changed
switch (s.state) {
case State::Offline:   if (Button("Connect")) net.connect(); break;   // resumes a saved login
case State::Connected: /* username + password fields */ if (Button("Log in")) net.login(user, pw, remember);
                       if (Button("Register")) net.registerAccount(user, pw, remember); break;
case State::Online:    /* browser / server */ break;
default:               Text("%s...", stateName(s.state));
}
if (s.inLobby) {
    Text("%s", sessionStateName(s.lobby.session));
    if (s.lobby.session == SessionState::Countdown)
        Text("starts in %lld s", (long long)(s.lobby.countdownEndsMs - clockMs() + 999) / 1000);
    for (auto& m : s.lobby.members)
        Text("%s %s %s %d ms", m.name.c_str(), m.player ? "" : "(next match)", pathName(m.path), m.peerRttMs);
    if (s.isHost && Button("Start now")) net.startMatch(true);
}
if (s.matchSerial != matchesSeen) { matchesSeen = s.matchSerial; /* start the game's match: s.match */ }
if (s.errorSerial != shownError) { /* toast s.error */ }
```

- Connection: `connect(address = serviceAddress(), autoLogin = true)`, `disconnect()`.
  `serviceAddress()` is the dedicated server chosen in Online Settings (`online_server`), else the
  official service (`online_master`).
  Account calls connect by themselves when needed. Every address the master's name resolves to is
  tried in turn (4 s each). The master's address never appears in anything shown to the player
  (the Snapshot has no address field, and error texts do not name it); only the log does.
- Account: `registerAccount`, `login` (both with `remember`: keep the login in the settings file),
  `resumeSaved`, `logout`, `changePassword`.
- Profiles: `setProfile(colour, machine)` (ids from `kColours` / `kMachines`, with display names),
  `viewProfile(name)` -> `Snapshot::viewed`, `reportMatch(won)`.
- Browser: `refreshServers(mode)` -> `Snapshot::servers` (with `session`, `maxPlayers`);
  `host(HostOptions)` (`maxPlayers`, `autoStart`, `settingsJson`, ...), `updateServer(HostOptions)`,
  `clearServerPassword()`, `join(id, password)`, `leave()`, `kick(name)`.
- Session: `Snapshot::player` (this console plays the next / current match), `lobby.session`,
  `countdownEndsMs` / `resultsEndsMs` (on `clockMs()`), `matchPlayers`, `settingsJson`,
  `resultsJson`, `hold`; host: `startMatch(now)`, `cancelMatch()`, `continueSession()`,
  `setGameSettings(json)`; everyone in the match: `endMatch(resultsJson)`. The match start:
  `Snapshot::match` (`MatchStart`: id, seed, `startAtMs` on `clockMs()`, mode, `settingsJson`,
  `consoles` in order with name / peer id / MAC, `myConsole`), with `matchSerial` bumping each time.
- Members: `player`, `peerId`, `path` (`None`, `Relay`, `DirectLan`, `DirectInternet`),
  `peerRttMs` (direct: measured; relay: estimated from both players' pings to the master), `relay`,
  `relayTcp`, `away`, `pingMs`.
- Chat: `chat(text)` -> `Snapshot::chat` (last 100 lines).
- Network: `Snapshot::linkUp`, `relayTcp`, `p2p`, `publicAddress` (STUN answered), `pingMs`, frame
  counters; `Client::port()` is the `rt::lan::Port`; `forceRelayTcp(bool)`, `enableP2p(bool)`.
- Old match-group calls (`joinGroup`, `leaveGroup`, `moveToGroup`, `arrangeGroups`, `autoFill`,
  `setPlaying`) are kept as no-ops so the menu builds; `Member::group`, `Snapshot::myGroup` and
  `Lobby::groups` show the roster as group 1. They will go once the menu uses the session.

Call `rt::airnet::shutdown()` at exit (leaves the server politely, stops the thread).

Settings (kirbyairride-settings.ini): `online_master = host:port` (the official service; its default
is set when building - CMake's `KAR_ONLINE_MASTER` or `online_master.txt` - and never in the source),
`online_server = host:port` (a dedicated server chosen in Online Settings; empty: the official
service), `online_user`, `online_token` and `online_token_server` (the remembered login and the
server it is for, empty for the official one: a login is offered only on its own server),
`online_relay_tcp` (1: the relay always over TCP), `online_p2p` (0: no peer to peer, all
through the relay), `online_stun` (STUN servers, `host:port,host:port`; empty: none).

## Testing

```
cd server
python3 -m unittest discover -s tests -t .      # or ./run.sh test
```

The suite starts a server on localhost for each test and covers handshake and versions (protocol 1
clients told to update), accounts (rules, case-insensitive names, wrong passwords, single-use
nonces, lockout, resume, logout, password change, bans, one connection per account), profiles and
stats, hosting/joining/leaving, passwords, full servers, kicks, host migration and closing, dropped
connections (place kept, then removed; peers sent again on return), the session (joining puts you
on the roster, the 4-console cap and waiting, the automatic countdown and match start - same message
to every player, console order, settings, seed - newcomers waiting during a match and playing the
next, match end by the host or by everyone, results, host start / start now / cancel and hold,
continue, a match down to one player, game settings rules, max players), peer lists (candidates
validated, the master's `seen` address, only the roster gets them), chat (cleaning, rate limit),
the relay between fake UDP consoles (one LAN per server, waiting members cut off, unicast by MAC,
flooding, FRAME_TO, roster changes, spoofing, size and rate limits, rebinding, latency), and the
relay over TCP (`test_relay_tcp.py`: a TCP-only LAN, TCP and UDP members together, FRAME_TO,
switching transports, rejected streams, one member per stream, leaving closes the stream, a member
that stops reading gets frames dropped while the others are served, latency).

The C++ client is tested against the real server by `tests/test_cpp_client.py`, which runs
`airnet_test` (CMake target, not built by default):

```
cmake --build <build dir> --target airnet_test
```

- `CppClient`: SHA-256 / HMAC / scrypt against published vectors; two clients register, host
  (with game settings), browse, join (on the roster at once), chat; peer to peer over the LAN
  address (each frame exactly once), then the relay with peer to peer off - UDP, one and both on
  TCP, and back - then one console peer to peer and the other not; a dropped connection; game
  settings (host only, valid JSON only, not during a match), a match start (same id, seed, start,
  console order, MACs, settings for both), match end, results, stats, continue, countdown, cancel;
  kick; remembered login; every resolved address tried; an unreachable master reported without
  its address. Then the Python client logs in to the C++ client's account (the implementations
  agree).
- `CppClientUdpBlocked`: the master's UDP answers nothing: the relay moves to TCP by itself, peer
  to peer still works.
- `CppClientNat`: three consoles, each behind a simulated NAT (`tests/natsim.py`: two
  port-restricted cone NATs, one symmetric) with a local STUN server, and a master whose UDP answers
  nothing (as on the VPS): the two behind cone NATs find a direct path by hole punching; the one
  behind the symmetric NAT reaches both through the relay (TCP); every frame arrives exactly once;
  a three-console match start.
