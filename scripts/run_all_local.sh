#!/usr/bin/env bash
# Bring up all nine nodes locally in the background, with logs in ./logs/.
# Use scripts/kill_all.sh to tear them down.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="${1:-$ROOT/config/topology.conf}"
LOGDIR="$ROOT/logs"
PIDS="$ROOT/.pids"
mkdir -p "$LOGDIR"
: > "$PIDS"

NODES=$(grep -E '^node ' "$CFG" | awk '{print $2}')
for ID in $NODES; do
    LOG="$LOGDIR/$ID.log"
    echo "[run_all] starting $ID -> $LOG"
    "$ROOT/scripts/run_node.sh" "$ID" "$CFG" >"$LOG" 2>&1 &
    echo "$!" >> "$PIDS"
    sleep 0.05
done

echo "[run_all] all nodes started; pids in $PIDS"
echo "[run_all] tail -F $LOGDIR/A.log to watch the portal"
