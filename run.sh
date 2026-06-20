#!/bin/bash
# lyghtd startup script. Runs the built server, passing through any flags.
#
# Usage:
#   ./run.sh                       # read-only frontend, default bind/data-dir
#   ./run.sh --ingest              # full drop-in: also ingest from ycashd
#   ./run.sh --bind 0.0.0.0:9067   # custom listen address
#   ./run.sh --help                # see all lyghtd options
#
# Any flags are forwarded straight to the lyghtd binary:
#   --bind <addr>   --data-dir <path>   --chain <name>   --ingest   --conf <path>

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

BIN="$SCRIPT_DIR/build/lyghtd"
if [ ! -x "$BIN" ]; then
    echo "lyghtd not built yet — run ./build.sh first." >&2
    exit 1
fi

echo "Starting lyghtd..."
exec "$BIN" "$@"
