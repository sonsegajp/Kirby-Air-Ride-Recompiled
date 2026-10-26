"""Protocol constants and input validation (shared by the server, the reference client and tests).

The control protocol is JSON lines over TCP; the relay is a small binary protocol over UDP. Both are
described in docs/ONLINE.md. Keep this file and src/runtime/airnet.h in step.
"""

import re
import unicodedata

# ---- versions -------------------------------------------------------------------------------------
# PROTO_VERSION is what this server speaks; clients older than MIN_PROTO_VERSION are told to update.
# 2: sessions (no match groups), peer-to-peer candidates, FRAME_TO.
PROTO_VERSION = 2
MIN_PROTO_VERSION = 2
SERVER_SOFTWARE = "airride-master"

# ---- default ports --------------------------------------------------------------------------------
DEFAULT_TCP_PORT = 27600
DEFAULT_UDP_PORT = 27601

# ---- limits ---------------------------------------------------------------------------------------
MAX_LINE = 8192             # one control message, bytes
LOBBY_MAX_MEMBERS = 16      # people in one hosted server
MATCH_MAX_PLAYERS = 8       # consoles in one match (the 8-player build; the game's own LAN takes 4)
MATCH_DEFAULT_PLAYERS = 4   # a server's when its host names none
SETTINGS_MAX = 4096         # the host's game settings, as JSON, bytes
RESULTS_MAX = 2048          # a match result a client reports, as JSON, bytes
CANDIDATES_MAX = 8          # addresses a client offers for peer-to-peer
CANDIDATE_TYPES = ("lan", "public")
P2P_KEY_BYTES = 8
CHAT_MAX = 200              # characters in a chat line
CHAT_HISTORY = 50           # lines a newcomer receives
LOBBY_NAME_MAX = 32
REGION_MAX = 12
LOBBY_PASSWORD_MAX = 64
TOKEN_BYTES = 24            # session token (48 hex characters in the client's settings file)
TOKEN_LIFETIME = 30 * 24 * 3600
NONCE_BYTES = 16
SALT_BYTES = 16
VERIFIER_BYTES = 32

# Password hashing (scrypt, computed by the client: see docs/ONLINE.md "Accounts"). The server
# advertises these for new accounts and stores each account's own, so they can be raised later.
SCRYPT_N = 16384
SCRYPT_R = 8
SCRYPT_P = 1
SCRYPT_LIMITS = {"n": (1024, 1 << 18), "r": (1, 8), "p": (1, 4)}  # at most 256 MiB (128 * n * r)

# ---- relay (UDP) ----------------------------------------------------------------------------------
# Every datagram starts with an 8-byte header: type u8, version u8, reserved u16, relay id u32 (BE).
RELAY_VERSION = 1
RELAY_HELLO = 0x01        # c->s: header + secret[8] + echo u32. Binds the sender's address; keepalive.
RELAY_FRAME = 0x02        # both ways: header + one Ethernet frame (14..1514 bytes)
RELAY_FRAME_TO = 0x03     # c->s: header + n u8 + n relay ids (u32 BE) + frame: to those members only
                          # (the others get it peer to peer); they receive it as a FRAME
RELAY_TARGETS_MAX = 16
RELAY_HELLO_ACK = 0x81    # s->c: header + echo u32 (the client's round trip time)
RELAY_HEADER = 8
RELAY_SECRET_BYTES = 8
FRAME_MIN = 14
FRAME_MAX = 1514
RELAY_RATE = 1500         # frames a second per member (sustained) ...
RELAY_BURST = 3000        # ... and burst
RELAY_TIMEOUT = 15.0      # seconds without a datagram before a member's address is forgotten
# The relay over TCP (when UDP does not get through), on the control port: the connection starts
# with RELAY_STREAM_MAGIC, then carries the same messages as the datagrams above, each preceded by
# its length (u16 big-endian). The first must be a HELLO.
RELAY_STREAM_MAGIC = b"AIRRELAY"
RELAY_STREAM_QUEUE = 64 * 1024   # bytes queued to a member's stream before frames are dropped ...
RELAY_STREAM_STALL = 5.0         # ... and seconds it may stay that backed up before it is closed
RELAY_STREAM_SNDBUF = 64 * 1024  # the kernel's send buffer for a stream (no seconds of frames queued there)

# ---- profiles -------------------------------------------------------------------------------------
# Kirby's colours in Kirby Air Ride.
COLOURS = ("pink", "yellow", "red", "blue", "green", "purple", "brown", "white")
# Air Ride machines (ids; display names live in the client).
MACHINES = (
    "warp_star", "compact_star", "winged_star", "shadow_star", "bulk_star", "slick_star",
    "formula_star", "wagon_star", "rocket_star", "swerve_star", "turbo_star", "jet_star",
    "wheelie_bike", "rex_wheelie", "wheelie_scooter", "hydra", "dragoon",
)
MODES = ("any", "air_ride", "city_trial")

USERNAME_MIN = 3
USERNAME_MAX = 16
PASSWORD_MIN = 6   # enforced by the client (the server never sees the password)
_USERNAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{%d,%d}$" % (USERNAME_MIN - 1, USERNAME_MAX - 1))
_REGION_RE = re.compile(r"^[A-Za-z0-9 -]{0,%d}$" % REGION_MAX)
RESERVED_NAMES = {"admin", "administrator", "server", "system", "host", "master", "moderator",
                  "mod", "root", "nintendo", "hal", "kirby", "airride", "everyone", "null", "none"}


def valid_username(name):
    """A username as typed: 3-16 of A-Z a-z 0-9 _ . - starting with a letter or digit."""
    return isinstance(name, str) and bool(_USERNAME_RE.match(name))


def username_key(name):
    """The case-insensitive key that makes usernames unique."""
    return name.lower()


def clean_text(text, limit):
    """Free text (chat, lobby names): printable characters only, spaces collapsed, cut to `limit`."""
    if not isinstance(text, str):
        return ""
    out = []
    for ch in text:
        cat = unicodedata.category(ch)
        if ch in "\t\r\n":
            ch = " "
        elif cat[0] == "C" or cat in ("Zl", "Zp"):
            continue  # control, format, surrogate, private use, unassigned
        out.append(ch)
    cleaned = " ".join("".join(out).split())
    return cleaned[:limit]


def valid_region(region):
    return isinstance(region, str) and bool(_REGION_RE.match(region))


def is_hex(value, nbytes):
    if not isinstance(value, str) or len(value) != 2 * nbytes:
        return False
    try:
        bytes.fromhex(value)
    except ValueError:
        return False
    return True
