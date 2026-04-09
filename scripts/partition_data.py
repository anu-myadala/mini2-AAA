#!/usr/bin/env python3
"""Partition a NYC 311 CSV (or any of the per-month CSVs from mini1) into N
output files, one per node. The default split is hash(unique_key) % N which
gives a roughly even distribution and zero overlap.

Output schema (consumed by both the C++ and Python PartitionStores):
    unique_key,created_ymd,borough,latitude,longitude,complaint_type

Usage:
    python scripts/partition_data.py \
        --input  data/raw/311_2024.csv data/raw/311_2023.csv \
        --outdir data \
        --nodes  A,B,C,D,E,F,G,H,I
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
from typing import Dict, List, Optional


_HEADER_CANON = ["unique_key", "created_ymd", "borough", "latitude", "longitude", "complaint_type"]


def parse_ymd(s: str) -> int:
    if not s:
        return 0
    if len(s) >= 8 and s.isdigit():
        return int(s[:8])
    if len(s) >= 10 and s[4] == "-" and s[7] == "-":
        try:
            return int(s[0:4]) * 10000 + int(s[5:7]) * 100 + int(s[8:10])
        except ValueError:
            return 0
    return 0


def detect_columns(header: List[str]) -> Dict[str, int]:
    idx = {h.strip(): i for i, h in enumerate(header)}
    out = {}
    for canon, names in {
        "unique_key": ("unique_key", "Unique Key"),
        "created_date": ("created_date", "Created Date"),
        "complaint_type": ("complaint_type", "Complaint Type"),
        "borough": ("borough", "Borough"),
        "latitude": ("latitude", "Latitude"),
        "longitude": ("longitude", "Longitude"),
    }.items():
        for n in names:
            if n in idx:
                out[canon] = idx[n]
                break
    return out


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--input", nargs="+", required=True, help="raw NYC 311 CSV files")
    p.add_argument("--outdir", required=True)
    p.add_argument("--nodes", required=True, help="comma-separated node ids")
    p.add_argument("--limit", type=int, default=0, help="optional cap on rows per input")
    args = p.parse_args()

    nodes = [n.strip() for n in args.nodes.split(",") if n.strip()]
    if not nodes:
        print("no node ids", file=sys.stderr)
        return 2

    os.makedirs(args.outdir, exist_ok=True)

    handles = {}
    writers = {}
    for nid in nodes:
        path = os.path.join(args.outdir, f"part_{nid}.csv")
        fh = open(path, "w", newline="")
        handles[nid] = fh
        w = csv.writer(fh)
        w.writerow(_HEADER_CANON)
        writers[nid] = w

    counts = {nid: 0 for nid in nodes}
    n = len(nodes)
    total = 0

    try:
        for in_path in args.input:
            with open(in_path, newline="") as ih:
                reader = csv.reader(ih)
                hdr = next(reader, None)
                if not hdr:
                    continue
                cols = detect_columns(hdr)
                if "unique_key" not in cols:
                    print(f"warn: {in_path} missing unique_key column, skipping", file=sys.stderr)
                    continue

                rows_in = 0
                for row in reader:
                    try:
                        key_str = row[cols["unique_key"]] if cols["unique_key"] < len(row) else ""
                        if not key_str:
                            continue
                        key = int(key_str)
                    except ValueError:
                        continue
                    if key == 0:
                        continue

                    bucket = key % n
                    nid = nodes[bucket]

                    date = parse_ymd(row[cols["created_date"]] if "created_date" in cols and cols["created_date"] < len(row) else "")
                    complaint = row[cols["complaint_type"]] if "complaint_type" in cols and cols["complaint_type"] < len(row) else ""
                    boro = row[cols["borough"]] if "borough" in cols and cols["borough"] < len(row) else ""
                    lat = row[cols["latitude"]] if "latitude" in cols and cols["latitude"] < len(row) else ""
                    lon = row[cols["longitude"]] if "longitude" in cols and cols["longitude"] < len(row) else ""

                    writers[nid].writerow([key, date, boro, lat, lon, complaint])
                    counts[nid] += 1
                    total += 1
                    rows_in += 1
                    if args.limit and rows_in >= args.limit:
                        break
            print(f"[partition] {in_path}: {rows_in} rows", file=sys.stderr)
    finally:
        for fh in handles.values():
            fh.close()

    print(f"[partition] total rows partitioned: {total}", file=sys.stderr)
    for nid in nodes:
        print(f"  {nid}: {counts[nid]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
