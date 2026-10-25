"""The game's C++ client (src/runtime/airnet.cpp, built as airnet_test) against this server.

Build it first:  cmake --build <build dir> --target airnet_test
The executable is looked for in $AIRNET_TEST_EXE, then build/*/airnet_test[.exe]; without it the
test is skipped.
"""

import glob
import os
import socket
import subprocess
import tempfile
import unittest

from .harness import ServerTest
from .natsim import Nat, StunServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def find_exe():
    env = os.environ.get("AIRNET_TEST_EXE")
    if env:
        return env if os.path.isfile(env) else None
    name = "airnet_test.exe" if os.name == "nt" else "airnet_test"  # (WSL could run the .exe, but not reach UDP)
    found = glob.glob(os.path.join(ROOT, "build", "*", name))
    return max(found, key=os.path.getmtime) if found else None


def run_exe(test, *extra):
    with tempfile.TemporaryDirectory() as cwd:  # it saves a settings file (remembered login)
        proc = subprocess.run([find_exe(), "--master", "127.0.0.1:%d" % test.srv.tcp_port, "--prefix", "cpp", *extra],
                              cwd=cwd, capture_output=True, text=True, timeout=180)
    out = proc.stdout + proc.stderr
    if proc.returncode != 0 or "AIRNET TEST PASSED" not in out:
        test.fail("airnet_test failed (exit %d):\n%s" % (proc.returncode, out))
    for line in out.splitlines():
        if "relay round trip" in line or "crypto vectors" in line or "relay ping" in line:
            print("\n    " + line.strip(), end="")
    # The C++ client's scrypt and login proof agree with Python's: log in to its account here.
    reply = test.connect().login("cpp_alice", "correct horse")
    test.assertEqual(reply["profile"]["colour"], "yellow")
    test.assertEqual((reply["profile"]["played"], reply["profile"]["wins"]), (1, 1))
    return out


@unittest.skipUnless(find_exe(), "airnet_test not built")
class CppClient(ServerTest):
    def test_cpp_client(self):
        run_exe(self)


@unittest.skipUnless(find_exe(), "airnet_test not built")
class CppClientUdpBlocked(ServerTest):
    """No UDP gets through (as on a host whose firewall lets only TCP in): the server tells clients a
    UDP port where nothing answers, and they must move the relay to TCP by themselves."""

    def server_config(self):
        self.blackhole = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.blackhole.bind(("127.0.0.1", 0))  # bound (no ICMP unreachable), never read: datagrams vanish
        return {"public_udp_port": self.blackhole.getsockname()[1]}

    def tearDown(self):
        super().tearDown()
        self.blackhole.close()

    def test_falls_back_to_tcp(self):
        out = run_exe(self, "--udp-blocked", "1")
        self.assertIn("using TCP", out)


@unittest.skipUnless(find_exe(), "airnet_test not built")
class CppClientNat(ServerTest):
    """Three consoles behind simulated NATs, with a STUN server, and a master whose UDP gets nowhere
    (as on the VPS: its relay runs over TCP). Two cone NATs: a direct path by hole punching. One
    symmetric NAT: that console reaches the others through the relay - mixed paths in one match."""

    def server_config(self):
        self.blackhole = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.blackhole.bind(("127.0.0.1", 0))
        return {"public_udp_port": self.blackhole.getsockname()[1]}

    def setUp(self):
        super().setUp()
        self.stun = StunServer()
        self.nats = [Nat(), Nat(), Nat(symmetric=True)]

    def tearDown(self):
        for n in self.nats:
            n.stop()
        self.stun.stop()
        super().tearDown()
        self.blackhole.close()

    def test_p2p_through_nats_and_relay_fallback(self):
        with tempfile.TemporaryDirectory() as cwd:
            proc = subprocess.run([find_exe(), "--master", "127.0.0.1:%d" % self.srv.tcp_port, "--prefix", "nat",
                                   "--stun", self.stun.address, "--nat-a", self.nats[0].address,
                                   "--nat-b", self.nats[1].address, "--nat-c", self.nats[2].address],
                                  cwd=cwd, capture_output=True, text=True, timeout=180)
        out = proc.stdout + proc.stderr
        if proc.returncode != 0 or "AIRNET TEST PASSED" not in out:
            self.fail("airnet_test (NAT) failed (exit %d):\n%s" % (proc.returncode, out))
        for line in out.splitlines():
            if "relay round trip" in line or line.startswith("nat_"):
                print("\n    " + line.strip(), end="")
        self.assertGreater(self.stun.answered, 0)
        self.assertGreater(self.nats[2].dropped, 0)  # the symmetric NAT turned the punching away
