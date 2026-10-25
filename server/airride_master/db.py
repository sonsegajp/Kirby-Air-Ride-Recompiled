"""Accounts, profiles and sessions in SQLite.

One connection, used from one worker thread (the server runs every call through Database.run, so
disk waits never stall the event loop and with it the relay). WAL mode: readers never block, a
commit is an append.
"""

import asyncio
import concurrent.futures
import secrets
import sqlite3
import time

from . import protocol as P

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER PRIMARY KEY,
    username    TEXT NOT NULL,             -- as typed
    username_key TEXT NOT NULL UNIQUE,     -- lower case: names are unique regardless of case
    salt        BLOB NOT NULL,
    scrypt_n    INTEGER NOT NULL,
    scrypt_r    INTEGER NOT NULL,
    scrypt_p    INTEGER NOT NULL,
    verifier    BLOB NOT NULL,             -- scrypt(password, salt): see crypto.py
    colour      TEXT NOT NULL DEFAULT 'pink',
    machine     TEXT NOT NULL DEFAULT 'warp_star',
    played      INTEGER NOT NULL DEFAULT 0,
    wins        INTEGER NOT NULL DEFAULT 0,
    created     INTEGER NOT NULL,
    last_seen   INTEGER NOT NULL,
    banned      INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sessions (
    token    BLOB PRIMARY KEY,
    user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created  INTEGER NOT NULL,
    expires  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);
"""

PROFILE_COLUMNS = "username, colour, machine, played, wins, created, last_seen"


def profile_dict(row):
    return {"username": row[0], "colour": row[1], "machine": row[2], "played": row[3],
            "wins": row[4], "created": row[5], "last_seen": row[6]}


class Database:
    def __init__(self, path):
        self.path = path
        self._pool = concurrent.futures.ThreadPoolExecutor(max_workers=1, thread_name_prefix="db")
        self._conn = None
        self._pool.submit(self._open).result()

    def _open(self):
        self._conn = sqlite3.connect(self.path, isolation_level=None, check_same_thread=False)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._conn.execute("PRAGMA foreign_keys=ON")
        self._conn.executescript(SCHEMA)

    async def run(self, fn, *args):
        """Runs `fn(*args)` (a method of this class) on the database thread."""
        return await asyncio.get_running_loop().run_in_executor(self._pool, fn, *args)

    def call(self, fn, *args):
        """... the same, blocking (startup, maintenance, tests)."""
        return self._pool.submit(fn, *args).result()

    def close(self):
        def _close():
            if self._conn:
                self._conn.close()
                self._conn = None
        self._pool.submit(_close).result()
        self._pool.shutdown()

    # ---- everything below runs on the database thread ---------------------------------------------

    def secret(self):
        """A random per-installation secret (fake salts for unknown usernames)."""
        row = self._conn.execute("SELECT value FROM meta WHERE key='secret'").fetchone()
        if row:
            return bytes(row[0])
        value = secrets.token_bytes(32)
        self._conn.execute("INSERT INTO meta(key, value) VALUES('secret', ?)", (value,))
        return value

    def create_user(self, username, salt, n, r, p, verifier):
        """True if created; False if the name is taken (in any case)."""
        now = int(time.time())
        try:
            self._conn.execute(
                "INSERT INTO users(username, username_key, salt, scrypt_n, scrypt_r, scrypt_p, verifier,"
                " created, last_seen) VALUES(?,?,?,?,?,?,?,?,?)",
                (username, P.username_key(username), salt, n, r, p, verifier, now, now))
        except sqlite3.IntegrityError:
            return False
        return True

    def credentials(self, key):
        """(id, username, salt, n, r, p, verifier, banned) or None."""
        row = self._conn.execute(
            "SELECT id, username, salt, scrypt_n, scrypt_r, scrypt_p, verifier, banned FROM users"
            " WHERE username_key=?", (key,)).fetchone()
        if not row:
            return None
        return (row[0], row[1], bytes(row[2]), row[3], row[4], row[5], bytes(row[6]), bool(row[7]))

    def set_verifier(self, user_id, salt, n, r, p, verifier):
        with self._conn:
            self._conn.execute("BEGIN")
            self._conn.execute("UPDATE users SET salt=?, scrypt_n=?, scrypt_r=?, scrypt_p=?, verifier=?"
                               " WHERE id=?", (salt, n, r, p, verifier, user_id))
            self._conn.execute("DELETE FROM sessions WHERE user_id=?", (user_id,))

    def profile(self, key):
        row = self._conn.execute("SELECT " + PROFILE_COLUMNS + " FROM users WHERE username_key=?",
                                 (key,)).fetchone()
        return profile_dict(row) if row else None

    def set_profile(self, user_id, colour, machine):
        self._conn.execute("UPDATE users SET colour=?, machine=? WHERE id=?", (colour, machine, user_id))

    def add_match(self, user_id, won):
        self._conn.execute("UPDATE users SET played=played+1, wins=wins+? WHERE id=?",
                           (1 if won else 0, user_id))

    def touch(self, user_id):
        self._conn.execute("UPDATE users SET last_seen=? WHERE id=?", (int(time.time()), user_id))

    def set_banned(self, key, banned):
        cur = self._conn.execute("UPDATE users SET banned=? WHERE username_key=?", (1 if banned else 0, key))
        if banned:
            self._conn.execute("DELETE FROM sessions WHERE user_id=(SELECT id FROM users WHERE username_key=?)",
                               (key,))
        return cur.rowcount > 0

    def create_session(self, user_id):
        token = secrets.token_bytes(P.TOKEN_BYTES)
        now = int(time.time())
        expires = now + P.TOKEN_LIFETIME
        self._conn.execute("INSERT INTO sessions(token, user_id, created, expires) VALUES(?,?,?,?)",
                           (token, user_id, now, expires))
        # An account keeps its 8 newest sessions (8 PCs); older ones lapse.
        self._conn.execute("DELETE FROM sessions WHERE user_id=? AND token NOT IN (SELECT token FROM sessions"
                           " WHERE user_id=? ORDER BY created DESC LIMIT 8)", (user_id, user_id))
        return token, expires

    def sessions_of(self, user_id):
        """[(token, expires)] still valid."""
        now = int(time.time())
        return [(bytes(t), e) for t, e in self._conn.execute(
            "SELECT token, expires FROM sessions WHERE user_id=? AND expires>?", (user_id, now))]

    def delete_session(self, token):
        self._conn.execute("DELETE FROM sessions WHERE token=?", (token,))

    def purge(self):
        self._conn.execute("DELETE FROM sessions WHERE expires<=?", (int(time.time()),))

    def user_count(self):
        return self._conn.execute("SELECT COUNT(*) FROM users").fetchone()[0]

    def backup(self, dest_path):
        """A consistent copy of the live database (safe while the server runs)."""
        dest = sqlite3.connect(dest_path)
        try:
            self._conn.backup(dest)
        finally:
            dest.close()
