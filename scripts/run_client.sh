#!/usr/bin/env bash
# Convenience wrapper around the C++ portal client.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cpp/build/portal_client"
CFG="$ROOT/config/topology.conf"
if [ ! -x "$BIN" ]; then
    echo "portal_client not built; run scripts/build_cpp.sh first" >&2
    exit 1
fi
exec "$BIN" --config "$CFG" "$@"
