"""Runs a master server on localhost (ephemeral ports, a fresh database) in a background thread."""

import asyncio
import logging
import os
import shutil
import tempfile
import threading
import unittest

from airride_master import server as server_module
from airride_master.client import Client
from airride_master.server import Config, MasterServer


class LocalServer:
    def __init__(self, **config):
        self.dir = tempfile.mkdtemp(prefix="airride-test-")
        self.config = Config(host="127.0.0.1", tcp_port=0, udp_port=0, db_path=os.path.join(self.dir, "test.db"),
                             motd="test server")
        for key, value in config.items():
            setattr(self.config, key, value)
        self.loop = asyncio.new_event_loop()
        self.server = None
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.server = MasterServer(self.config)
        self.loop.run_until_complete(self.server.start())
        self._ready.set()
        self.loop.run_forever()

    def start(self):
        self._thread.start()
        if not self._ready.wait(10):
            raise RuntimeError("server did not start")
        return self

    def call(self, fn):
        """Runs `fn()` on the server's loop and returns its result (to inspect server state)."""
        fut = asyncio.run_coroutine_threadsafe(self._call(fn), self.loop)
        return fut.result(5)

    @staticmethod
    async def _call(fn):
        return fn()

    def stop(self):
        asyncio.run_coroutine_threadsafe(self.server.stop(), self.loop).result(10)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self._thread.join(10)
        self.loop.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    @property
    def tcp_port(self):
        return self.server.tcp_port

    @property
    def udp_port(self):
        return self.server.udp_port


class ServerTest(unittest.TestCase):
    """Each test gets its own server."""

    def setUp(self):
        logging.basicConfig(level=logging.WARNING)
        self._saved = (server_module.REGISTER_PER_IP, server_module.MAX_PER_IP)
        server_module.REGISTER_PER_IP = 1000  # every test account comes from 127.0.0.1
        server_module.MAX_PER_IP = 1000
        self.srv = LocalServer(**self.server_config()).start()
        self.clients = []

    def server_config(self):
        """Config fields to change for this test's server."""
        return {}

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.srv.stop()
        server_module.REGISTER_PER_IP, server_module.MAX_PER_IP = self._saved

    def connect(self, **kw):
        c = Client("127.0.0.1", self.srv.tcp_port, **kw)
        self.clients.append(c)
        return c

    def user(self, name, password="hunter22"):
        """A connected, registered, logged-in client."""
        c = self.connect()
        c.register(name, password)
        c.login(name, password)
        return c
