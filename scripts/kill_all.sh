#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIDS="$ROOT/.pids"
if [ ! -f "$PIDS" ]; then
    echo "no $PIDS file"
    exit 0
fi
while read -r p; do
    [ -z "$p" ] && continue
    kill "$p" 2>/dev/null || true
done < "$PIDS"
sleep 0.3
while read -r p; do
    [ -z "$p" ] && continue
    kill -9 "$p" 2>/dev/null || true
done < "$PIDS"
rm -f "$PIDS"
echo "[kill_all] done"
