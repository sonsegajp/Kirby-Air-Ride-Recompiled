"""python3 -m airride_master [serve|backup|ban|unban|stats] ...

Settings come from the command line, else from environment variables (AIRRIDE_*), else defaults.
"""

import argparse
import asyncio
import logging
import os
import signal
import sys

from . import __version__
from . import protocol as P
from .db import Database
from .server import Config, MasterServer


def _env(name, default):
    return os.environ.get("AIRRIDE_" + name, default)


def _serve(args):
    config = Config(host=args.host, tcp_port=args.tcp_port, udp_port=args.udp_port,
                    public_udp_port=args.public_udp_port, db_path=args.db, motd=args.motd,
                    tls_port=args.tls_port, tls_cert=args.tls_cert, tls_key=args.tls_key)

    async def main():
        server = MasterServer(config)
        await server.start()
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop.set)
            except (NotImplementedError, RuntimeError):  # Windows: Ctrl+C raises KeyboardInterrupt
                pass
        try:
            await stop.wait()
        finally:
            logging.info("shutting down")
            await server.stop()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    return 0


def _backup(args):
    db = Database(args.db)
    try:
        db.call(db.backup, args.dest)
    finally:
        db.close()
    print("backed up %s -> %s" % (args.db, args.dest))
    return 0


def _ban(args, banned):
    db = Database(args.db)
    try:
        found = db.call(db.set_banned, P.username_key(args.username), banned)
    finally:
        db.close()
    print(("%s %s" % ("banned" if banned else "unbanned", args.username)) if found else "no such user")
    return 0 if found else 1


def _stats(args):
    db = Database(args.db)
    try:
        print("accounts: %d" % db.call(db.user_count))
    finally:
        db.close()
    return 0


def _options(p, suppress):
    """The options every command takes; on subcommands they default to "not given" so a value
    given before the subcommand is not overwritten."""
    d = (lambda v: argparse.SUPPRESS) if suppress else (lambda v: v)
    p.add_argument("--db", default=d(_env("DB", "airride.db")), help="SQLite database file (AIRRIDE_DB)")
    p.add_argument("--log-level", default=d(_env("LOG_LEVEL", "INFO")), help="DEBUG, INFO, WARNING (AIRRIDE_LOG_LEVEL)")
    p.add_argument("--host", default=d(_env("HOST", "0.0.0.0")), help="address to listen on (AIRRIDE_HOST)")
    p.add_argument("--tcp-port", type=int, default=d(int(_env("TCP_PORT", P.DEFAULT_TCP_PORT))),
                   help="control port, TCP (AIRRIDE_TCP_PORT)")
    p.add_argument("--udp-port", type=int, default=d(int(_env("UDP_PORT", P.DEFAULT_UDP_PORT))),
                   help="relay port, UDP (AIRRIDE_UDP_PORT)")
    p.add_argument("--public-udp-port", type=int, default=d(int(_env("PUBLIC_UDP_PORT", 0))),
                   help="relay port clients are told, if a NAT maps it elsewhere (AIRRIDE_PUBLIC_UDP_PORT)")
    p.add_argument("--motd", default=d(_env("MOTD", "")), help="message of the day (AIRRIDE_MOTD)")
    p.add_argument("--tls-port", type=int, default=d(int(_env("TLS_PORT", 0))),
                   help="also serve the control protocol over TLS on this port (AIRRIDE_TLS_PORT)")
    p.add_argument("--tls-cert", default=d(_env("TLS_CERT", "")), help="certificate chain, PEM (AIRRIDE_TLS_CERT)")
    p.add_argument("--tls-key", default=d(_env("TLS_KEY", "")), help="private key, PEM (AIRRIDE_TLS_KEY)")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="airride_master",
                                 description="Kirby Air Ride: Recompiled online master server %s" % __version__)
    _options(ap, False)
    common = argparse.ArgumentParser(add_help=False)
    _options(common, True)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("serve", parents=[common], help="run the server (the default)")
    backup = sub.add_parser("backup", parents=[common], help="copy the database safely while the server runs")
    backup.add_argument("dest")
    ban = sub.add_parser("ban", parents=[common], help="suspend an account (and end its saved logins)")
    ban.add_argument("username")
    unban = sub.add_parser("unban", parents=[common], help="lift a suspension")
    unban.add_argument("username")
    sub.add_parser("stats", parents=[common], help="print the number of accounts")

    args = ap.parse_args(argv)
    logging.basicConfig(level=getattr(logging, str(args.log_level).upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if args.cmd == "backup":
        return _backup(args)
    if args.cmd == "ban":
        return _ban(args, True)
    if args.cmd == "unban":
        return _ban(args, False)
    if args.cmd == "stats":
        return _stats(args)
    return _serve(args)


if __name__ == "__main__":
    sys.exit(main())
