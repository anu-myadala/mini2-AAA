#!/usr/bin/env python3
"""Generate a tiny synthetic 311-shaped dataset and partition it across the
nine nodes. Useful for smoke tests when you don't have the real CSV handy.

Usage:
    python scripts/generate_synthetic.py --rows 10000 --outdir data --nodes A,B,C,D,E,F,G,H,I
"""

from __future__ import annotations

import argparse
import csv
import os
import random
from typing import List


BOROUGHS = ["MANHATTAN", "BROOKLYN", "QUEENS", "BRONX", "STATEN ISLAND"]
COMPLAINTS = [
    "Noise - Residential",
    "Illegal Parking",
    "HEAT/HOT WATER",
    "Blocked Driveway",
    "Street Light Condition",
    "Water Leak",
    "Rodent",
    "Sanitation Condition",
]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--rows", type=int, default=10000)
    p.add_argument("--outdir", required=True)
    p.add_argument("--nodes", required=True)
    p.add_argument("--seed", type=int, default=42)
    args = p.parse_args()

    rng = random.Random(args.seed)
    os.makedirs(args.outdir, exist_ok=True)
    nodes: List[str] = [n for n in args.nodes.split(",") if n]
    n = len(nodes)
    assert n > 0

    handles = {}
    writers = {}
    counts = {}
    for nid in nodes:
        f = open(os.path.join(args.outdir, f"part_{nid}.csv"), "w", newline="")
        handles[nid] = f
        w = csv.writer(f)
        w.writerow(["unique_key", "created_ymd", "borough", "latitude", "longitude", "complaint_type"])
        writers[nid] = w
        counts[nid] = 0

    for i in range(args.rows):
        key = 1_000_000 + i
        ymd = rng.choice([20230101, 20230615, 20231201, 20240115, 20240701, 20240920])
        boro = rng.choice(BOROUGHS)
        lat = round(40.50 + rng.random() * 0.40, 5)
        lon = round(-74.20 + rng.random() * 0.40, 5)
        complaint = rng.choice(COMPLAINTS)
        nid = nodes[key % n]
        writers[nid].writerow([key, ymd, boro, lat, lon, complaint])
        counts[nid] += 1

    for f in handles.values():
        f.close()
    print("[gen] wrote", args.rows, "rows across", n, "nodes")
    for nid in nodes:
        print(f"  {nid}: {counts[nid]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
