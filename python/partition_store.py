"""Local SoA-style partition storage for the Python node server.

Same column layout as the C++ PartitionStore. Uses arrays of typed entries
(uint64 keys, uint32 dates, int8 boroughs, double lat/lon, intern'd strings)
to keep memory low and queries cache-friendly.
"""

from __future__ import annotations

import array
import bisect
import csv
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

# Grid cell width in degrees. Parity with cpp/include/Indices.hpp::GRID_DEG.
_GRID_DEG = 0.01


def _cell_key(lat: float, lon: float) -> int:
    """Pack (lat, lon) into a single int cell key. Parity with
    Indices::cellKey in C++ (same GRID_DEG, same bias)."""
    la = int(math.floor(lat / _GRID_DEG)) + (1 << 30)
    lo = int(math.floor(lon / _GRID_DEG)) + (1 << 30)
    return (la << 32) | lo


_BOROUGH_BY_NAME = {
    "MANHATTAN": 1,
    "BROOKLYN": 2,
    "QUEENS": 3,
    "BRONX": 4,
    "STATEN ISLAND": 5,
}
_NAME_BY_BOROUGH = {v: k for k, v in _BOROUGH_BY_NAME.items()}


def borough_from_str(s: str) -> int:
    return _BOROUGH_BY_NAME.get(s.upper().strip(), 0)


def borough_to_str(b: int) -> str:
    return _NAME_BY_BOROUGH.get(b, "UNSPECIFIED")


def parse_ymd_any(s: str) -> int:
    if not s:
        return 0
    if len(s) >= 8 and s.isdigit():
        return int(s)
    if len(s) >= 10 and s[4] == "-" and s[7] == "-":
        try:
            return int(s[0:4]) * 10000 + int(s[5:7]) * 100 + int(s[8:10])
        except ValueError:
            return 0
    return 0


@dataclass
class PartitionStore:
    keys: array.array  # 'Q' uint64
    dates: array.array  # 'I' uint32
    boroughs: array.array  # 'b' int8
    lats: array.array  # 'd' double
    lons: array.array  # 'd' double
    complaints: List[str]

    # Secondary indices. Parity with Indices.hpp in C++. Built once at
    # the end of load(). All row-id lists are kept sorted ascending so
    # that set-intersection is a linear merge.
    by_borough: Dict[int, List[int]] = field(default_factory=dict)
    by_complaint: Dict[str, List[int]] = field(default_factory=dict)
    # (date, row_id) sorted by date then row_id; bisect on the date column.
    by_date: List[Tuple[int, int]] = field(default_factory=list)
    by_cell: Dict[int, List[int]] = field(default_factory=dict)

    @classmethod
    def empty(cls) -> "PartitionStore":
        return cls(
            keys=array.array("Q"),
            dates=array.array("I"),
            boroughs=array.array("b"),
            lats=array.array("d"),
            lons=array.array("d"),
            complaints=[],
        )

    # ---- Index build + lookup ----

    def _build_indices(self) -> None:
        """Build all four indices in one pass (plus one sort for the
        date index). Called once at the end of load(); immutable after.
        Addresses the prof's mini1 critique that Phase 3 was still a
        linear search by letting QueryEngine cut to a candidate set."""
        self.by_borough.clear()
        self.by_complaint.clear()
        self.by_cell.clear()

        n = len(self.keys)
        date_entries: List[Tuple[int, int]] = [(0, 0)] * n
        for i in range(n):
            self.by_borough.setdefault(int(self.boroughs[i]), []).append(i)
            self.by_complaint.setdefault(self.complaints[i], []).append(i)
            date_entries[i] = (int(self.dates[i]), i)
            la = self.lats[i]
            lo = self.lons[i]
            if la == 0.0 and lo == 0.0:
                continue  # skip ungeocoded rows (parity with C++)
            self.by_cell.setdefault(_cell_key(la, lo), []).append(i)

        date_entries.sort()
        self.by_date = date_entries

    def _date_range(self, lo: int, hi: int) -> List[int]:
        if not self.by_date:
            return []
        # bisect by date (first column of the tuple)
        keys_only = [d for d, _ in self.by_date]
        left = bisect.bisect_left(keys_only, lo)
        right = bisect.bisect_right(keys_only, hi)
        out = [rid for _, rid in self.by_date[left:right]]
        out.sort()
        return out

    def _geo_box(self, mnla: float, mxla: float, mnlo: float, mxlo: float) -> List[int]:
        if not self.by_cell or mnla > mxla or mnlo > mxlo:
            return []
        la0 = int(math.floor(mnla / _GRID_DEG))
        la1 = int(math.floor(mxla / _GRID_DEG))
        lo0 = int(math.floor(mnlo / _GRID_DEG))
        lo1 = int(math.floor(mxlo / _GRID_DEG))
        out: List[int] = []
        for la in range(la0, la1 + 1):
            for lo in range(lo0, lo1 + 1):
                key = ((la + (1 << 30)) << 32) | (lo + (1 << 30))
                bucket = self.by_cell.get(key)
                if bucket:
                    out.extend(bucket)
        out.sort()
        return out

    def __len__(self) -> int:
        return len(self.keys)

    def load(self, csv_path: str | Path) -> None:
        p = Path(csv_path)
        if not p.exists():
            print(f"[partition] no file at {csv_path}, this node will be empty", flush=True)
            return
        with open(p, newline="") as fh:
            reader = csv.reader(fh)
            try:
                hdr = next(reader)
            except StopIteration:
                return

            # Map columns we care about (accept both raw 311 and our partition format).
            cols = {h.strip(): i for i, h in enumerate(hdr)}
            def col(*names: str) -> int:
                for n in names:
                    if n in cols:
                        return cols[n]
                return -1

            i_key = col("unique_key", "Unique Key")
            i_date = col("created_ymd", "created_date", "Created Date")
            i_complaint = col("complaint_type", "Complaint Type")
            i_boro = col("borough", "Borough")
            i_lat = col("latitude", "Latitude")
            i_lon = col("longitude", "Longitude")
            already_ymd = "created_ymd" in cols

            if i_key < 0 or i_date < 0:
                raise ValueError("partition CSV missing required columns")

            interned: dict[str, str] = {}

            for row in reader:
                if not row or len(row) <= max(i_key, i_date):
                    continue
                try:
                    k = int(row[i_key]) if row[i_key] else 0
                except ValueError:
                    continue
                if k == 0:
                    continue
                if already_ymd:
                    try:
                        d = int(row[i_date]) if row[i_date] else 0
                    except ValueError:
                        d = 0
                else:
                    d = parse_ymd_any(row[i_date])

                b = borough_from_str(row[i_boro]) if i_boro >= 0 and i_boro < len(row) else 0
                la = float(row[i_lat]) if i_lat >= 0 and i_lat < len(row) and row[i_lat] else 0.0
                lo = float(row[i_lon]) if i_lon >= 0 and i_lon < len(row) and row[i_lon] else 0.0
                ct = row[i_complaint] if i_complaint >= 0 and i_complaint < len(row) else ""
                ct_iv = interned.setdefault(ct, ct)

                self.keys.append(k)
                self.dates.append(d)
                self.boroughs.append(b)
                self.lats.append(la)
                self.lons.append(lo)
                self.complaints.append(ct_iv)

        # One-shot index build. See class docstring on _build_indices.
        self._build_indices()
        print(
            f"[partition] indices: borough_buckets={len(self.by_borough)} "
            f"complaint_buckets={len(self.by_complaint)} "
            f"date_entries={len(self.by_date)} "
            f"grid_cells={len(self.by_cell)}",
            flush=True,
        )

    def scan(
        self,
        spec,
        chunk_rows: int,
        is_canceled,
        force_linear: bool = False,
    ) -> Iterable[Tuple[List[Tuple[int, int, int, float, float, str]], bool]]:
        """Yield (rows, done) tuples. `rows` is a list of native tuples to keep
        per-row allocations low; the gRPC layer turns them into Row311 messages."""
        n = len(self)
        want_b = spec.borough  # int (proto enum)
        any_b = want_b == 0
        any_complaint = not spec.complaint_type
        df = spec.date_from
        dt = spec.date_to
        use_geo = bool(spec.use_geo_box)
        mn_la, mx_la = spec.min_lat, spec.max_lat
        mn_lo, mx_lo = spec.min_lon, spec.max_lon
        cap = spec.max_rows

        # ---- Plan: collect candidate row-id sets from indices ----
        # Parity with QueryEngine::run() in C++. Empty list = "no
        # indexable predicate, fall back to linear scan." A set that
        # comes back empty means the query has zero matches and we can
        # short-circuit without scanning at all.
        cands: List[List[int]] = []
        if not force_linear:
            # Plan from indices. force_linear=True skips this entirely
            # so the bench can measure the original cost. An empty bucket
            # for any equality predicate means zero matches; short-circuit.
            if not any_b:
                v = self.by_borough.get(int(want_b))
                if not v:
                    yield [], True
                    return
                cands.append(v)
            if not any_complaint:
                v = self.by_complaint.get(spec.complaint_type)
                if not v:
                    yield [], True
                    return
                cands.append(v)
            if df or dt:
                lo = df if df else 0
                hi = dt if dt else 0xFFFFFFFF
                v = self._date_range(lo, hi)
                if not v:
                    yield [], True
                    return
                cands.append(v)
            if use_geo:
                v = self._geo_box(mn_la, mx_la, mn_lo, mx_lo)
                if not v:
                    yield [], True
                    return
                cands.append(v)

        produced_total = 0
        out: List[Tuple[int, int, int, float, float, str]] = []

        def emit_one(i: int) -> bool:
            nonlocal produced_total, out
            if use_geo:
                la = self.lats[i]
                lo = self.lons[i]
                if la < mn_la or la > mx_la or lo < mn_lo or lo > mx_lo:
                    return True  # skip but keep going
            out.append((
                self.keys[i],
                self.dates[i],
                self.boroughs[i],
                self.lats[i],
                self.lons[i],
                self.complaints[i],
            ))
            produced_total += 1
            if cap and produced_total >= cap:
                return False
            return True

        if not cands:
            # Pure-linear fallback. Reached either when the query has no
            # indexable predicate (give-me-everything) or when the bench
            # passes force_linear=True. Predicates evaluated inline so
            # we can short-circuit row-by-row in the same pass; this is
            # the original mini1 scan logic, kept verbatim for honest
            # apples-to-apples comparison in the index benchmark.
            for i in range(n):
                if (i & 0x3FF) == 0 and is_canceled():
                    break
                if not any_b and self.boroughs[i] != want_b:
                    continue
                d = self.dates[i]
                if df and d < df:
                    continue
                if dt and d > dt:
                    continue
                if use_geo:
                    la = self.lats[i]
                    lo = self.lons[i]
                    if la < mn_la or la > mx_la or lo < mn_lo or lo > mx_lo:
                        continue
                if not any_complaint and self.complaints[i] != spec.complaint_type:
                    continue
                if not emit_one(i):
                    break
                if len(out) >= chunk_rows:
                    yield out, False
                    out = []
            yield out, True
            return

        # Smallest-first AND-of-postings intersection (parity with C++).
        cands.sort(key=len)
        work = list(cands[0])
        for k in range(1, len(cands)):
            if not work:
                break
            other = cands[k]
            # Linear merge: both sides are sorted ascending by row_id.
            merged: List[int] = []
            i = j = 0
            la_n = len(work)
            lb_n = len(other)
            while i < la_n and j < lb_n:
                a = work[i]
                b = other[j]
                if a == b:
                    merged.append(a)
                    i += 1
                    j += 1
                elif a < b:
                    i += 1
                else:
                    j += 1
            work = merged

        for idx, i in enumerate(work):
            if (idx & 0x3FF) == 0 and is_canceled():
                break
            if not emit_one(i):
                break
            if len(out) >= chunk_rows:
                yield out, False
                out = []
        yield out, True
