# Running an Air Ride server (official or dedicated)

The master server is one Python program (standard library only, Python 3.10 or newer) and one
SQLite database file. Any small Linux VPS will do; architecture and protocol are in
[../docs/ONLINE.md](../docs/ONLINE.md).

## Quick start: a dedicated server on any PC

**Windows, nothing to install:** unzip `AirRideServer-win64.zip` (the releases page; it has Python's
embeddable runtime built in - `tools/server/make_server_zip.ps1` makes it) and double-click
**StartServer.bat**. Close its window to stop it. Options go through:
`StartServer.bat --motd "Welcome!"`. From a source checkout, the same `StartServer.bat` here uses an
installed Python 3.10+.

**Linux / macOS** (Python 3.10+, standard library only): `./run.sh --db airride.db`.

It listens on TCP 27600 (control) and UDP 27601 (relay). Friends on your network join with your PC's
LAN address; over the internet, forward those two ports on your router (or use a VPS, below). In the
game: **Online > Settings > Service: Dedicated Server**, then the address.

## Ports to open

| Port  | Protocol | What                                                                 |
|-------|----------|----------------------------------------------------------------------|
| 27600 | TCP      | Control: accounts, server browser, lobbies, chat                     |
| 27601 | UDP      | Relay: the game's LAN packets for consoles with no direct path       |

Players' consoles talk to each other directly (UDP, hole punched; they find their public addresses
with public STUN servers, not this one) and use the master's relay only for consoles with no direct
path. For that relay, UDP is preferred (it copes best with packet loss) but not required: when no UDP gets through,
clients carry the relay over the TCP port instead, by themselves (docs/ONLINE.md, "The relay over
TCP"). The two may be the same number (TCP and UDP ports are separate), e.g. on a host whose
firewall passes only a few ports: `AIRRIDE_TCP_PORT=25565 AIRRIDE_UDP_PORT=25565`.

With `ufw`:

    sudo ufw allow 27600/tcp
    sudo ufw allow 27601/udp

Also open them in the VPS provider's firewall / security group if it has one. If the VPS sits
behind a NAT that maps the UDP port to another outside port, start the server with
`--public-udp-port <outside port>` (clients are told which UDP port to use).

## Install

    sudo useradd --system --home /opt/airride --shell /usr/sbin/nologin airride
    sudo mkdir -p /opt/airride
    sudo cp -r server /opt/airride/          # this directory
    python3 --version                        # 3.10 or newer
    cd /opt/airride/server && sudo -u airride ./run.sh test    # optional: the test suite (about 30 s)

    sudo cp /opt/airride/server/airride-master.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now airride-master
    systemctl status airride-master
    journalctl -u airride-master -f          # the log (logins, servers hosted; never passwords)

The unit keeps the database at `/var/lib/airride/airride.db`. Settings are environment variables
in the unit (or command-line options): `AIRRIDE_DB`, `AIRRIDE_HOST`, `AIRRIDE_TCP_PORT`,
`AIRRIDE_UDP_PORT`, `AIRRIDE_PUBLIC_UDP_PORT`, `AIRRIDE_MOTD`, `AIRRIDE_LOG_LEVEL`, and the TLS ones
below. `./run.sh --help` lists them.

To run it by hand instead: `./run.sh --db ./airride.db` (Ctrl+C stops it).

## Pointing the game at it

There are two ways, and they don't exclude each other:

- **As a dedicated server** (anyone can run one): players pick it in the game, under
  **Online > Settings > Service: Dedicated Server**, and type its address (`host` or `host:port`;
  27600 when no port is given). It has its own accounts: players create one on it the first time.
  The game remembers a login per server, so switching between the official service and a dedicated
  server signs in by itself on each. The choice is saved as `online_server = host:port` in
  `kirbyairride-settings.ini` (empty: the official service).
- **As the official service** of a build: the address is set when building, never in the source -
  `cmake -DKAR_ONLINE_MASTER=airride.example.org:27600`, or one line in `online_master.txt` beside
  `CMakeLists.txt` (git ignores it). A build without one offers dedicated servers only. Players can
  also override it with `online_master = host:port` in `kirbyairride-settings.ini`.

## Backups

Everything worth keeping is in the database file: accounts (usernames, password verifiers - never
passwords), profiles, stats and saved-login sessions. Lobbies and chat live in memory only and are
gone after a restart (players just host again).

Copy it safely while the server runs (SQLite's online backup; a plain `cp` of a live database can
catch it mid-write):

    sudo -u airride /opt/airride/server/run.sh --db /var/lib/airride/airride.db backup /var/lib/airride/backup-$(date +%F).db

A daily cron job (`sudo crontab -u airride -e`), keeping two weeks:

    15 4 * * * cd /opt/airride/server && ./run.sh --db /var/lib/airride/airride.db backup /var/lib/airride/backup-$(date +\%F).db && find /var/lib/airride -name 'backup-*.db' -mtime +14 -delete

Copy the backups off the VPS too (`scp`, or the provider's snapshots). To restore: stop the
service, replace `airride.db` with a backup (and delete any `airride.db-wal` / `airride.db-shm`
beside it), start the service.

## Administration

    ./run.sh --db /var/lib/airride/airride.db stats           # number of accounts
    ./run.sh --db /var/lib/airride/airride.db ban NAME        # suspends an account, ends its saved logins
    ./run.sh --db /var/lib/airride/airride.db unban NAME

A ban takes effect at the player's next login (a connected player is not thrown out at once).
Hosts can kick people from their own servers in the game.

## Updating

Copy the new `server` directory over the old one and `sudo systemctl restart airride-master`. A
restart drops everyone (lobbies are in memory); their games reconnect and log in again by
themselves, but matches in progress end. When the control protocol changes incompatibly, the new
server raises `MIN_PROTO_VERSION` (airride_master/protocol.py) and older games are told to update.

## TLS (optional, for later)

Logins never send passwords (see ONLINE.md, "Security"), but the rest of the control connection is
plain TCP. The server can already serve the same protocol over TLS on a second port:

    AIRRIDE_TLS_PORT=27602 AIRRIDE_TLS_CERT=/etc/letsencrypt/live/<domain>/fullchain.pem \
    AIRRIDE_TLS_KEY=/etc/letsencrypt/live/<domain>/privkey.pem

(open 27602/tcp; the airride user must be able to read the key). The game's client does not speak
TLS yet: that needs a domain name with a certificate, and a TLS layer in src/runtime/airnet.cpp
(SChannel on Windows, OpenSSL on Linux).

## Capacity

The relay forwards each datagram with a dictionary lookup and one `sendto` per recipient. A single
Python process handles tens of thousands of datagrams a second, i.e. dozens of simultaneous 4-player
matches; the hard limits are per member (1500 frames a second, 1514-byte frames). Watch CPU with
`top` during busy hours; if the relay ever becomes the bottleneck it is one small file
(`airride_master/relay.py`) to move into a faster process.
