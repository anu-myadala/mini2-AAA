#!/usr/bin/env python3
"""Measurement harness for mini2.

Runs a battery of queries against an already-running cluster (node A at the
host/port given via --portal) and prints a reproducible JSON report. No
mocks. The same harness is used to populate docs/REPORT.md.

Scenarios measured:
  1. full-scan drain-fast   — client pulls as fast as possible, no sleep
  2. full-scan drain-slow   — client sleeps slow_ms between fetches
  3. borough filter         — filter=BROOKLYN, drain-fast
  4. date range filter      — 2024-01-01..2024-12-31, drain-fast
  5. geo box filter         — small NYC box, drain-fast
  6. cancel mid-query       — submit, pull a little, cancel, measure shutdown time
  7. two-concurrent-clients — one fast, one slow, same query, same time

Each scenario is replicated N times (default 5) and we report
mean ± stdev for wall time, rows returned, peak buffered rows (from GetStats),
and effective chunk size distribution.

Usage:
    python3 scripts/measure.py --portal 127.0.0.1:50051 \
        --reps 5 --slow-ms 300 \
        --out docs/measurements.json
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field, asdict
from typing import Callable, Dict, List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "_gen"))

import grpc  # noqa: E402
import overlay_pb2 as pb  # noqa: E402
import overlay_pb2_grpc as pbg  # noqa: E402


def _stub(addr: str):
    ch = grpc.insecure_channel(
        addr,
        options=[
            ("grpc.max_send_message_length", 64 * 1024 * 1024),
            ("grpc.max_receive_message_length", 64 * 1024 * 1024),
        ],
    )
    return pbg.PortalStub(ch), ch


_BOROUGH_NAME_TO_ENUM = {
    "MANHATTAN":     pb.B_MANHATTAN,
    "BROOKLYN":      pb.B_BROOKLYN,
    "QUEENS":        pb.B_QUEENS,
    "BRONX":         pb.B_BRONX,
    "STATEN ISLAND": pb.B_STATEN_ISLAND,
}


def _spec(**kw) -> pb.QuerySpec:
    s = pb.QuerySpec()
    if "borough" in kw and kw["borough"] is not None:
        s.borough = _BOROUGH_NAME_TO_ENUM[kw["borough"]]
    if "complaint" in kw and kw["complaint"] is not None:
        s.complaint_type = kw["complaint"]
    if "from_ymd" in kw and kw["from_ymd"]:
        s.date_from = int(kw["from_ymd"])
    if "to_ymd" in kw and kw["to_ymd"]:
        s.date_to = int(kw["to_ymd"])
    if "geo" in kw and kw["geo"] is not None:
        lat_lo, lat_hi, lon_lo, lon_hi = kw["geo"]
        s.min_lat = lat_lo
        s.max_lat = lat_hi
        s.min_lon = lon_lo
        s.max_lon = lon_hi
        s.use_geo_box = True
    if "max_total" in kw and kw["max_total"]:
        s.max_rows = int(kw["max_total"])
    return s


@dataclass
class RunResult:
    rows: int = 0
    wall_ms: float = 0.0
    chunks: int = 0
    chunk_sizes: List[int] = field(default_factory=list)
    fetch_latencies_ms: List[float] = field(default_factory=list)
    peak_buffered: int = 0


def _drain(stub, rid: str, slow_ms: int = 0) -> RunResult:
    r = RunResult()
    t0 = time.perf_counter()
    peak = 0
    while True:
        f0 = time.perf_counter()
        resp = stub.FetchChunk(pb.FetchRequest(request_id=rid, max_rows=0), timeout=30.0)
        f1 = time.perf_counter()
        r.fetch_latencies_ms.append((f1 - f0) * 1000)
        n = len(resp.rows)
        if n:
            r.chunks += 1
            r.chunk_sizes.append(n)
            r.rows += n
        if resp.pending_producers > peak:
            peak = resp.pending_producers
        if resp.final:
            break
        if slow_ms > 0:
            time.sleep(slow_ms / 1000.0)
    r.wall_ms = (time.perf_counter() - t0) * 1000
    r.peak_buffered = peak
    return r


def _scenario(stub, spec: pb.QuerySpec, slow_ms: int = 0, client_id: str = "meas") -> RunResult:
    ack = stub.SubmitQuery(pb.SubmitRequest(spec=spec, client_id=client_id), timeout=5.0)
    return _drain(stub, ack.request_id, slow_ms=slow_ms)


def _summarize(runs: List[RunResult]) -> dict:
    def ms_stats(values):
        if not values:
            return {"mean": 0, "stdev": 0, "min": 0, "max": 0, "p50": 0, "p95": 0}
        values_sorted = sorted(values)
        return {
            "mean": round(statistics.mean(values), 2),
            "stdev": round(statistics.stdev(values), 2) if len(values) > 1 else 0.0,
            "min": round(min(values), 2),
            "max": round(max(values), 2),
            "p50": round(values_sorted[len(values_sorted) // 2], 2),
            "p95": round(values_sorted[max(0, int(len(values_sorted) * 0.95) - 1)], 2),
        }

    rows = [r.rows for r in runs]
    walls = [r.wall_ms for r in runs]
    chunk_means = [statistics.mean(r.chunk_sizes) if r.chunk_sizes else 0 for r in runs]
    fetch_lat = [l for r in runs for l in r.fetch_latencies_ms]
    return {
        "reps": len(runs),
        "rows_returned": {
            "mean": statistics.mean(rows),
            "stdev": statistics.stdev(rows) if len(rows) > 1 else 0.0,
            "min": min(rows),
            "max": max(rows),
        },
        "wall_ms": ms_stats(walls),
        "avg_chunk_rows_per_run": {
            "mean": round(statistics.mean(chunk_means), 1) if chunk_means else 0,
            "min": round(min(chunk_means), 1) if chunk_means else 0,
            "max": round(max(chunk_means), 1) if chunk_means else 0,
        },
        "fetch_latency_ms": ms_stats(fetch_lat),
        "throughput_rows_per_sec": round(
            (statistics.mean(rows) / (statistics.mean(walls) / 1000.0)) if statistics.mean(walls) > 0 else 0, 1
        ),
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--portal", default="127.0.0.1:50051")
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--slow-ms", type=int, default=300)
    p.add_argument("--out", default="docs/measurements.json")
    p.add_argument("--warm", type=int, default=1, help="warmup runs (discarded)")
    args = p.parse_args()

    stub, ch = _stub(args.portal)

    # Warmup
    for _ in range(args.warm):
        _scenario(stub, _spec())

    report: Dict[str, object] = {
        "portal": args.portal,
        "reps": args.reps,
        "slow_ms": args.slow_ms,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "scenarios": {},
    }

    def run(name: str, fn: Callable[[], RunResult], reps: int = args.reps):
        print(f"[measure] {name} x{reps}...", flush=True)
        runs = []
        for i in range(reps):
            runs.append(fn())
        report["scenarios"][name] = _summarize(runs)
        s = report["scenarios"][name]
        print(
            f"    rows={s['rows_returned']['mean']:.0f}  "
            f"wall={s['wall_ms']['mean']:.1f}±{s['wall_ms']['stdev']:.1f}ms  "
            f"tput={s['throughput_rows_per_sec']:.0f} rows/s  "
            f"chunk~{s['avg_chunk_rows_per_run']['mean']:.0f}"
        )

    # 1. Full scan, drain fast
    run("full_scan_drain_fast", lambda: _scenario(stub, _spec()))

    # 2. Full scan, drain slow (backpressure kicks in)
    run("full_scan_drain_slow", lambda: _scenario(stub, _spec(), slow_ms=args.slow_ms))

    # 3. Borough filter
    run("borough_brooklyn", lambda: _scenario(stub, _spec(borough="BROOKLYN")))

    # 4. Date range filter (2024)
    run("date_range_2024", lambda: _scenario(stub, _spec(from_ymd=20240101, to_ymd=20241231)))

    # 5. Geo box filter (rough Brooklyn box)
    run("geo_box", lambda: _scenario(stub, _spec(geo=(40.60, 40.75, -74.05, -73.85))))

    # 6. Multi-field combined query (borough + date + geo)
    run(
        "multi_field",
        lambda: _scenario(
            stub,
            _spec(
                borough="BROOKLYN",
                from_ymd=20240101,
                to_ymd=20241231,
                geo=(40.60, 40.75, -74.05, -73.85),
            ),
        ),
    )

    # 7. Cancel mid-query
    def cancel_run() -> RunResult:
        t0 = time.perf_counter()
        ack = stub.SubmitQuery(pb.SubmitRequest(spec=_spec(), client_id="meas-cancel"))
        rid = ack.request_id
        rows = 0
        # pull a couple chunks then cancel
        for _ in range(2):
            resp = stub.FetchChunk(pb.FetchRequest(request_id=rid, max_rows=0), timeout=5.0)
            rows += len(resp.rows)
            if resp.final:
                break
        stub.CancelQuery(pb.CancelRequest(request_id=rid))
        # drain until final
        while True:
            resp = stub.FetchChunk(pb.FetchRequest(request_id=rid, max_rows=0), timeout=5.0)
            if resp.final:
                break
        t1 = time.perf_counter()
        r = RunResult(rows=rows, wall_ms=(t1 - t0) * 1000)
        return r

    run("cancel_mid_query", cancel_run)

    # 8. Two concurrent clients, one slow one fast
    def concurrent_run() -> RunResult:
        results: Dict[str, RunResult] = {}

        def worker(name: str, slow: int):
            ack = stub.SubmitQuery(
                pb.SubmitRequest(spec=_spec(), client_id=f"meas-conc-{name}")
            )
            results[name] = _drain(stub, ack.request_id, slow_ms=slow)

        t0 = time.perf_counter()
        t1 = threading.Thread(target=worker, args=("fast", 0))
        t2 = threading.Thread(target=worker, args=("slow", args.slow_ms))
        t1.start()
        t2.start()
        t1.join()
        t2.join()
        t1_ms = results["fast"].wall_ms
        t2_ms = results["slow"].wall_ms
        # pack into a single RunResult for summarization of the FAST client
        return results["fast"]

    run("concurrent_fast_vs_slow_fast_half", concurrent_run)

    # stats sanity (whole-node = empty request_id)
    try:
        stats = stub.GetStats(pb.StatsRequest(request_id=""))
        report["node_a_stats"] = {
            "node_id": stats.node_id,
            "active_requests": stats.active_requests,
            "buffered_rows": stats.buffered_rows,
            "total_rows_served": stats.total_rows_served,
        }
    except Exception as e:
        report["node_a_stats"] = {"error": str(e)}

    out = os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(report, f, indent=2)
    print(f"[measure] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
