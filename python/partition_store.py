"""Local SoA-style partition storage for the Python node server.

Same column layout as the C++ PartitionStore. Uses arrays of typed entries
(uint64 keys, uint32 dates, int8 boroughs, double lat/lon, intern'd strings)
to keep memory low and queries cache-friendly.
"""

from __future__ import annotations

import array
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Tuple


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

    def scan(
        self,
        spec,
        chunk_rows: int,
        is_canceled,
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

        produced_total = 0
        out: List[Tuple[int, int, int, float, float, str]] = []
        for i in range(n):
            if (i & 0x3FF) == 0 and is_canceled():
                break
            b = self.boroughs[i]
            if not any_b and b != want_b:
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
                break
            if len(out) >= chunk_rows:
                yield out, False
                out = []
        yield out, True
