#!/usr/bin/env python3
"""bench_index.py - Compare indexed vs linear-scan local query times.

This is the empirical companion to the new Indices module. It loads one
partition CSV the same way a node would, then runs the same QuerySpec
twice: once with the secondary indices in use (the new path) and once
with the indices forcibly cleared so the engine falls back to the pure
linear scan path (the original mini1 / first-cut-mini2 behavior).

It writes results as a small JSON document to stdout, so the report can
quote real numbers instead of estimates.

Usage:
    python3 scripts/bench_index.py --csv data/part_A.csv --reps 5
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Callable, Dict, List

# Allow running from the repo root without an install step.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "python"))

from partition_store import PartitionStore  # noqa: E402


def make_spec(**kwargs) -> SimpleNamespace:
    """Mimic the tiny subset of overlay_pb2.QuerySpec that scan() reads.
    Keeps this script free of any generated protobuf dependency so it
    runs without scripts/gen_proto.sh."""
    return SimpleNamespace(
        borough=kwargs.get("borough", 0),
        complaint_type=kwargs.get("complaint_type", ""),
        date_from=kwargs.get("date_from", 0),
        date_to=kwargs.get("date_to", 0),
        use_geo_box=kwargs.get("use_geo_box", False),
        min_lat=kwargs.get("min_lat", 0.0),
        max_lat=kwargs.get("max_lat", 0.0),
        min_lon=kwargs.get("min_lon", 0.0),
        max_lon=kwargs.get("max_lon", 0.0),
        max_rows=kwargs.get("max_rows", 0),
    )


def time_scan(store: PartitionStore, spec, reps: int, force_linear: bool) -> Dict[str, float]:
    """Run the same scan `reps` times, returning timing stats. The
    `force_linear` flag flips the new code path off so we get a fair
    apples-to-apples comparison against the pre-index implementation."""
    times_ms: List[float] = []
    rows_returned = 0
    for _ in range(reps):
        rows_returned = 0
        t0 = time.perf_counter()
        for chunk, _done in store.scan(spec, 4096, lambda: False, force_linear=force_linear):
            rows_returned += len(chunk)
        t1 = time.perf_counter()
        times_ms.append((t1 - t0) * 1000.0)
    return {
        "rows": rows_returned,
        "mean_ms":   statistics.mean(times_ms),
        "stdev_ms":  statistics.pstdev(times_ms) if len(times_ms) > 1 else 0.0,
        "min_ms":    min(times_ms),
        "max_ms":    max(times_ms),
    }


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--csv", required=True, help="partition CSV file")
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--out", default="-")
    args = p.parse_args()

    store = PartitionStore.empty()
    store.load(args.csv)
    n_rows = len(store)
    if n_rows == 0:
        sys.exit(f"empty store from {args.csv!r}")

    # The five scenarios mirror those the prof's "multi-field queries"
    # remark called out, plus the give-me-everything baseline.
    scenarios = [
        ("baseline_full",            make_spec()),
        ("borough_brooklyn",         make_spec(borough=2)),
        ("date_2024",                make_spec(date_from=20240101, date_to=20241231)),
        ("geo_box_small",            make_spec(use_geo_box=True,
                                               min_lat=40.65, max_lat=40.75,
                                               min_lon=-74.02, max_lon=-73.92)),
        ("borough_x_date_x_geo",     make_spec(borough=2,
                                               date_from=20240101, date_to=20241231,
                                               use_geo_box=True,
                                               min_lat=40.55, max_lat=40.75,
                                               min_lon=-74.05, max_lon=-73.85)),
    ]

    results = {"csv": args.csv, "rows_in_store": n_rows, "reps": args.reps,
               "scenarios": []}

    for name, spec in scenarios:
        with_idx = time_scan(store, spec, args.reps, force_linear=False)
        no_idx   = time_scan(store, spec, args.reps, force_linear=True)
        speedup = (no_idx["mean_ms"] / with_idx["mean_ms"]) if with_idx["mean_ms"] > 0 else 0.0
        results["scenarios"].append({
            "name": name,
            "rows_returned": with_idx["rows"],
            "with_indices_ms":  with_idx,
            "linear_scan_ms":   no_idx,
            "speedup_x":        speedup,
        })

    text = json.dumps(results, indent=2)
    if args.out == "-":
        print(text)
    else:
        Path(args.out).write_text(text)


if __name__ == "__main__":
    main()
