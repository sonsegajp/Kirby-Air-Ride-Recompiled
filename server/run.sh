#!/bin/sh
# Runs the Air Ride master server from this directory (Python 3.10+, standard library only).
#   ./run.sh                          serve on TCP 27600 / UDP 27601, database ./airride.db
#   ./run.sh --db /var/lib/airride/airride.db --motd "Welcome!"
#   ./run.sh backup /path/to/copy.db  (and: ban NAME, unban NAME, stats; --help for all)
#   ./run.sh test                     the test suite
cd "$(dirname "$0")" || exit 1
if [ "$1" = "test" ]; then
    shift
    exec python3 -m unittest discover -s tests -t . "$@"
fi
exec python3 -m airride_master "$@"
