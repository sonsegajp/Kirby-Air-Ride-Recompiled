"""A small blocking client for the master server: the reference for the protocol (the game's client
is src/runtime/airnet.cpp), used by the tests and handy for poking a live server:

    python3 -c "from airride_master.client import *; c = Client('127.0.0.1'); c.login('name', 'pw'); print(c.request('list_servers'))"
"""

import json
import secrets
import socket
import struct
import time

from . import crypto
from . import protocol as P


class RequestFailed(Exception):
    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("err"), reply.get("msg")))
        self.code = reply.get("err")
        self.reply = reply


class Client:
    def __init__(self, host="127.0.0.1", port=P.DEFAULT_TCP_PORT, proto=P.PROTO_VERSION, timeout=5.0):
        self.server_host = host
        self.server_port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.events = []
        self.next_id = 1
        self.token = None
        self.username = None
        self.relay = None
        self._send({"t": "hello", "proto": proto, "client": "airride_master.client"})
        self.welcome = self._read()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # ---- wire -------------------------------------------------------------------------------------

    def _send(self, obj):
        self.sock.sendall(json.dumps(obj).encode() + b"\n")

    def _read(self, timeout=None):
        if timeout is not None:
            self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def request(self, t, **fields):
        """Sends a request and returns its reply; events that arrive meanwhile are kept."""
        rid = self.next_id
        self.next_id += 1
        self._send({"t": t, "id": rid, **fields})
        while True:
            msg = self._read()
            if msg.get("t") == "reply" and msg.get("id") == rid:
                if not msg.get("ok"):
                    raise RequestFailed(msg)
                return msg
            self._event(msg)

    def _event(self, msg):
        if msg.get("t") == "ping":
            self._send({"t": "pong", "n": msg.get("n")})
        self.events.append(msg)

    def event(self, t, timeout=3.0, where=None):
        """Waits for an event of type `t` (optionally matching `where(event)`)."""
        deadline = time.monotonic() + timeout
        while True:
            for i, e in enumerate(self.events):
                if e.get("t") == t and (where is None or where(e)):
                    return self.events.pop(i)
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("no %s event" % t)
            try:
                self._event(self._read(timeout=left))
            except socket.timeout:
                raise TimeoutError("no %s event" % t) from None
            finally:
                self.sock.settimeout(5.0)

    # ---- accounts ---------------------------------------------------------------------------------

    def register(self, username, password):
        sc = self.welcome["scrypt"]
        salt = secrets.token_bytes(P.SALT_BYTES)
        verifier = crypto.derive_verifier(password, salt, sc["n"], sc["r"], sc["p"])
        return self.request("register", username=username, salt=salt.hex(), n=sc["n"], r=sc["r"], p=sc["p"],
                            verifier=verifier.hex())

    def login(self, username, password):
        begin = self.request("login_begin", username=username)
        verifier = crypto.derive_verifier(password, bytes.fromhex(begin["salt"]), begin["n"], begin["r"], begin["p"])
        snonce = bytes.fromhex(begin["nonce"])
        cnonce = secrets.token_bytes(P.NONCE_BYTES)
        key = P.username_key(username)
        reply = self.request("login", username=username, cnonce=cnonce.hex(),
                             proof=crypto.login_proof(verifier, snonce, cnonce, key).hex())
        self.token = crypto.xor(bytes.fromhex(reply["token"]), crypto.token_mask(verifier, snonce, cnonce))
        self.username = username
        self.relay = reply.get("relay")
        return reply

    def resume(self, username, token):
        begin = self.request("login_begin", username=username)
        snonce = bytes.fromhex(begin["nonce"])
        cnonce = secrets.token_bytes(P.NONCE_BYTES)
        reply = self.request("resume", username=username, cnonce=cnonce.hex(),
                             proof=crypto.resume_proof(token, snonce, cnonce, P.username_key(username)).hex())
        self.token = token
        self.username = username
        self.relay = reply.get("relay")
        return reply

    # ---- lobbies ----------------------------------------------------------------------------------

    def host(self, **options):
        reply = self.request("host", **options)
        self.relay = reply["relay"]
        return reply

    def join(self, lobby, password=None):
        fields = {"lobby": lobby}
        if password is not None:
            fields["password"] = password
        reply = self.request("join", **fields)
        self.relay = reply["relay"]
        return reply

    def udp(self):
        """A relay endpoint for this member (call after host/join)."""
        return RelayClient(self.server_host, self.relay["port"], self.relay["id"], bytes.fromhex(self.relay["secret"]))

    def tcp_relay(self):
        """... the same over TCP (the control port), for networks without UDP."""
        return RelayStreamClient(self.server_host, self.server_port, self.relay["id"],
                                 bytes.fromhex(self.relay["secret"]))


class RelayClient:
    """One console's end of the relay."""

    def __init__(self, host, port, relay_id, secret):
        self.addr = (host, port)
        self.relay_id = relay_id
        self.secret = secret
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(2.0)

    def close(self):
        self.sock.close()

    def _header(self, kind):
        return struct.pack(">BBHI", kind, P.RELAY_VERSION, 0, self.relay_id)

    def hello(self, echo=1234):
        """Binds this socket's address; returns once acknowledged."""
        self.sock.sendto(self._header(P.RELAY_HELLO) + self.secret + struct.pack(">I", echo), self.addr)
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            data, _ = self.sock.recvfrom(2048)
            if data[0] == P.RELAY_HELLO_ACK and struct.unpack(">I", data[8:12])[0] == echo:
                return True
        return False

    def send(self, frame):
        self.sock.sendto(self._header(P.RELAY_FRAME) + frame, self.addr)

    def send_to(self, targets, frame):
        """A frame for those members only (relay ids)."""
        self.sock.sendto(self._frame_to(targets, frame), self.addr)

    def _frame_to(self, targets, frame):
        return (self._header(P.RELAY_FRAME_TO) + bytes((len(targets),))
                + b"".join(struct.pack(">I", t) for t in targets) + frame)

    def recv(self, timeout=1.0):
        """The next frame, or None."""
        self.sock.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                return None
            if data[0] == P.RELAY_FRAME:
                return data[P.RELAY_HEADER:]
            if time.monotonic() > deadline:
                return None


class RelayStreamClient(RelayClient):
    """One console's end of the relay over TCP: same messages, each preceded by its length."""

    def __init__(self, host, port, relay_id, secret, rcvbuf=None):
        self.addr = (host, port)
        self.relay_id = relay_id
        self.secret = secret
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(2.0)
        self.sock.connect(self.addr)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.sendall(P.RELAY_STREAM_MAGIC)
        self.buf = b""

    def _send(self, msg):
        self.sock.sendall(struct.pack(">H", len(msg)) + msg)

    def _recv(self, timeout):
        """The next message, or None after `timeout`."""
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buf) >= 2:
                n = struct.unpack(">H", self.buf[:2])[0]
                if len(self.buf) >= 2 + n:
                    msg, self.buf = self.buf[2:2 + n], self.buf[2 + n:]
                    return msg
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not data:
                raise ConnectionError("relay stream closed")
            self.buf += data

    def hello(self, echo=1234):
        self._send(self._header(P.RELAY_HELLO) + self.secret + struct.pack(">I", echo))
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            msg = self._recv(deadline - time.monotonic())
            if msg and msg[0] == P.RELAY_HELLO_ACK and struct.unpack(">I", msg[8:12])[0] == echo:
                return True
        return False

    def send(self, frame):
        self._send(self._header(P.RELAY_FRAME) + frame)

    def send_to(self, targets, frame):
        self._send(self._frame_to(targets, frame))

    def recv(self, timeout=1.0):
        deadline = time.monotonic() + timeout
        while True:
            msg = self._recv(max(0.0, deadline - time.monotonic()))
            if msg is None:
                return None
            if msg[0] == P.RELAY_FRAME:
                return msg[P.RELAY_HEADER:]


def frame(dst, src, payload=b"", ethertype=0x88B5):
    """An Ethernet frame (MACs as 6 bytes)."""
    return dst + src + struct.pack(">H", ethertype) + payload
