#!/usr/bin/env bash
# Run a single node by id. Picks the C++ or Python implementation based on
# `lang=` from the topology.conf.
#
# Usage:  scripts/run_node.sh A
#         scripts/run_node.sh G config/topology.conf
set -euo pipefail

ID="${1:?usage: run_node.sh <NODE_ID> [config]}"
CFG="${2:-$(cd "$(dirname "$0")/.." && pwd)/config/topology.conf}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

LANG_LINE="$(grep -E "^node[[:space:]]+${ID}[[:space:]]" "$CFG" || true)"
if [ -z "$LANG_LINE" ]; then
    echo "node $ID not found in $CFG" >&2
    exit 2
fi

LANG="cpp"
if echo "$LANG_LINE" | grep -q 'lang=python'; then LANG="python"; fi

case "$LANG" in
    python)
        export PYTHONPATH="$ROOT/python:${PYTHONPATH:-}"
        exec python3 "$ROOT/python/node_server.py" --id "$ID" --config "$CFG"
        ;;
    cpp)
        BIN="$ROOT/cpp/build/node_server"
        if [ ! -x "$BIN" ]; then
            echo "C++ node_server binary not found at $BIN" >&2
            echo "Run scripts/build_cpp.sh first." >&2
            exit 3
        fi
        exec "$BIN" --id "$ID" --config "$CFG"
        ;;
esac
