#!/usr/bin/env bash
# End-to-end smoke test:
#   1. Generate synthetic data
#   2. Start all nine nodes in the background
#   3. Run a handful of client queries
#   4. Tear everything down
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 scripts/generate_synthetic.py --rows 5000 --outdir data --nodes A,B,C,D,E,F,G,H,I

scripts/run_all_local.sh
trap 'scripts/kill_all.sh' EXIT
sleep 1.0

scripts/run_client.sh --borough BROOKLYN --max 200
scripts/run_client.sh --from 20230101 --to 20231231
scripts/run_client.sh --geo 40.6,40.8,-74.0,-73.9
echo "[smoke_test] OK"
