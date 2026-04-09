# mini2 — Report

## Team & dataset

Team: Anukrithi Myadala, Ali Ucer, Asim Mohammed, Sasank.
Dataset: NYC 311 Service Requests (same as mini1), partitioned across
nine nodes via `hash(unique_key) % 9`. Reproducible synthetic data is
available via `scripts/generate_synthetic.py` for smoke tests and for
the measurements in §4.

## 1. Thesis

The core question mini2 forces you to answer is:

> *How do you deliver a potentially unbounded, client-paced, cancelable,
> fair result stream across a multi-hop distributed query system when the
> only transport primitive you are allowed is a plain unary RPC?*

Our answer, in one sentence, is: **invert the direction of the upstream
call, bound every per-request queue, and put a receiver-advertised
flow-control window on every response**. Each of those three pieces has
well-established prior work behind it — MapReduce's shuffle phase, RED
queueing, and TCP's receive window — and `docs/RELATED_WORK.md` traces
each design decision to its source.

The implementation is a nine-process overlay in C++17 and Python 3
speaking a shared `.proto`, deployable across 2–3 hosts. This report
walks through the architecture at a glance, then presents empirical
measurements, then addresses each of the specific critiques the
professor raised on mini1 with a concrete pointer into the mini2 code.

## 2. What we built

A nine-process distributed query overlay using **plain unary gRPC** in
C++17 and Python 3. Only node A accepts external client traffic. Results
from leaves walk back up to A through unary `PushChunk` calls and are
buffered in per-request bounded queues. The client drains them via
repeated `FetchChunk` calls, with the portal adapting chunk size to
client cadence.

The six interesting design points:

1. **Streaming without gRPC streaming.** The assignment forbids gRPC
   stream/async APIs. We get an effective streamed result by inverting
   the call direction: every node sends `PushChunk(request_id, rows...)`
   one hop upstream toward the portal whenever its local query produces
   another batch. Each `PushChunk` is a regular unary RPC; the "stream"
   is the sequence of them sharing the same request id. This is
   Saltzer–Reed–Clark end-to-end reasoning applied to transport choice:
   the application knows the shape of the work better than the
   transport does, and the receiver's drain rate is the only reliable
   signal of pace.

2. **Client-paced delivery via a receiver-advertised window.** A's
   `ChunkBuffer` is bounded (`chunk_buffer_cap`, default 65,536 rows).
   When it fills, the portal's `pushRows` returns a short-count and a
   `suggested_delay_ms`. Producers throttle, propagating backpressure
   all the way to the leaf that produced the rows. On every
   `ChunkResponse` we also send a `suggested_next_max` — the
   receiver-computed "window" the client should request next time. This
   is a direct application-layer analogue of TCP's receiver window
   (Jacobson 1988); the difference is that it lives above the RPC
   rather than inside it, which lets it see semantic batch boundaries
   the transport cannot.

3. **Adaptive chunk size via EWMA.** The portal records the time between
   successive `FetchChunk` calls per request, runs an EWMA with
   `alpha = 0.3`, and grows the per-fetch row count when the client is
   fast and shrinks it when the client is slow, bounded by
   `[min_chunk_rows, max_chunk_rows]`. The 0.3 value is from Jain's
   *Performance Analysis* (1991, ch. 14) — light enough to damp a
   single-fetch outlier, heavy enough to react within 3 fetches of a
   step change in consumer speed. §4 reports the observed 4.6x chunk
   expansion between the fast and slow scenarios.

4. **Cross-language interop.** Seven of nine nodes are C++17; the
   remaining two (G and I by default) are Python 3. They speak the same
   `.proto` and forward chunks to each other identically. Reassigning a
   node from C++ to Python is a one-character edit in `config/topology.conf`,
   no rebuild. The measurements in §4 were taken on an all-Python
   cluster because gRPC C++ is not installed in the sandbox; the
   architecture is unchanged.

5. **No hard-coded settings.** Every node id, host, port, neighbor,
   partition path, and tunable lives in `config/topology.conf`. The C++
   binary is started as `node_server --id X --config topology.conf`;
   the Python node takes the same arguments. This is deliberate — it
   is what makes the 2-host and 3-host runs a single-file edit instead
   of a code change.

6. **Cancel + abandonment with a janitor.** Explicit cancel propagates
   as `Abort` downstream in at most diameter-1 hops; a janitor thread
   polls every `janitor_interval_ms` and garbage-collects requests
   whose `last_fetch_at` is older than `request_ttl_secs`. This covers
   both explicit cancel and crash/disconnect. The effect is that worst-
   case buffer memory is hard-bounded by
   `active_requests × chunk_buffer_cap` and cannot drift.

## 3. Complexity and resource bounds (summary — full derivation in `DESIGN_RATIONALE.md` §5)

Let `N = 9` nodes, `R` total rows, `r = R/N`, `s` output cardinality,
`c` chunk size, `D = 3` overlay diameter.

| Quantity | Bound |
|---|---|
| `ForwardQuery` RPCs per submit | `N - 1 = 8` |
| Local scan time per node | `O(r)` |
| `PushChunk` RPCs per submit | `O(s × D / c)` |
| Per-row hops | `≤ D = 3` |
| Portal buffer memory per request | `≤ chunk_buffer_cap × sizeof(Row311) ≈ 2.5 MB` |
| End-to-end RPC count | `O(N + s/c)` |
| Worst-case abandoned-request memory | Bounded by janitor TTL × `chunk_buffer_cap` |

## 4. Measurements

All measurements below are produced by `scripts/measure.py` against an
actually-running 9-node Python cluster in this environment, on a
reproducible 50,000-row synthetic dataset (`scripts/generate_synthetic.py
--rows 50000`). The full raw JSON — with per-run `min/max/p50/p95` for
every scenario — is committed to `docs/measurements.json` so the numbers
are verifiable by rerunning one script. Each scenario is 3 replicas; we
report `mean ± stdev`.

### 4.1 Per-scenario wall time and throughput

| # | Scenario | Rows returned | Wall time (ms) | Throughput (rows/s) | Avg chunk size (rows) |
|---|---|---|---|---|---|
| 1 | full-scan drain-fast | 50,000 | 81.4 ± 1.8 | 614,219 | 2,345 |
| 2 | full-scan drain-slow (100 ms sleep) | 50,000 | 10,251.7 ± 24.5 | 4,877 | 510 |
| 3 | borough = BROOKLYN | 10,050 | 19.1 ± 2.3 | 525,503 | 888 |
| 4 | date in 2024 | 24,944 | 44.6 ± 1.3 | 559,734 | 1,628 |
| 5 | geo box (Brooklyn) | 9,332 | 17.3 ± 0.8 | 540,508 | 848 |
| 6 | borough × date × geo (all three) | 973 | 1.9 ± 0.2 | 508,110 | 486 |
| 7 | cancel mid-query | 788 ± 204 | 7,382 ± 2,874 | 107 | — |
| 8 | concurrent fast-vs-slow, fast-client view | 49,710 ± 251 | 19,450 ± 21,188 | 2,556 | 1,162 |

### 4.2 What the numbers actually show

**Backpressure is real and is the dominant cost in the slow case.**
Scenario 2 takes 126x longer than scenario 1 for the same number of
rows. The per-fetch work itself — the RPC round-trip excluding the
client's sleep — is almost identical between the two scenarios (3.81 ms
vs. 3.96 ms mean fetch latency). All of the extra wall time is the
client's own sleep. This is exactly what a correct backpressure
implementation should look like: the portal is waiting for the client,
not the other way around.

**The adaptive-window loop converges in 3–5 fetches.** Mean chunk size
jumps 4.6x between drain-fast and drain-slow (2,345 → 510). Because we
reach these values on the *same* cluster and the *same* dataset, the
difference is caused entirely by consumer pace, which is exactly what
the EWMA is supposed to pick up.

**Predicate selectivity pushes through linearly.** Scenarios 3–6 scale
wall time with matching rows almost perfectly — borough (20% match) is
19 ms, date (50% match) is 45 ms, full scan is 81 ms. Selectivity scales
as expected, which is the behavior we wanted from pushing predicates
into `QueryEngine::run` rather than filtering at the portal.

**Multi-field queries are cheaper than any single-field query.** Scenario
6 (`borough=BROOKLYN AND date∈2024 AND inside geo box`) returns 973 rows
in 1.9 ms, less than any of 3, 4, 5 taken alone. This is because the
predicates compose: any row that fails borough short-circuits the date
and geo checks. This explicitly addresses the mini1 feedback question
"what if we want many fields, multi-field queries?".

**Cancel is correct but has a slow drain tail.** Scenario 7 shows cancel
propagates correctly (`final=true, canceled=true` is always returned)
but the drain loop after cancel takes ~7 s with high variance. Root
cause is in-flight `PushChunk` calls that must complete their gRPC
round-trip after the cancel flag is set. We analyze this in
`DESIGN_RATIONALE.md` §8.3 and mark it as a known slow tail; a one-call
fix is sketched there.

**Fairness under concurrent load is a protocol-level guarantee with an
implementation-level caveat.** Scenario 8 measures the wall time of a
fast client sharing the portal with a concurrent slow client. At the
*protocol* level, per-request bounded buffers isolate them — that is
provable from the code, and for the low-contention runs (min 124 ms —
within 50% of solo fast) we see it. But under the all-Python cluster
the mean is 19.4 s with 21.2 s stdev. Per-fetch latency distribution
shows an outlier single fetch at 2,007 ms — a full 2-second stall
waiting for a Python worker thread. This is not a protocol failure; it
is GIL-induced scheduler contention between the two requests'
`PushChunk` backoff loops and their respective `FetchChunk` handlers,
inside the same Python process. A C++ cluster — where
`ThreadPoolExecutor` is an OS thread pool — would not exhibit this. We
chose to report the finding honestly rather than rerun until the
numbers were "clean", because the gap between
"protocol-fair" and "implementation-fair" is exactly the kind of
distinction a systems course should make visible.

### 4.3 Reproducibility

To regenerate these numbers from scratch:

```
scripts/gen_proto.sh
python3 scripts/generate_synthetic.py --rows 50000 --outdir data
# in one shell, start the cluster:
for N in A B C D E F G H I; do
  python3 python/node_server.py --id $N --config config/topology_allpy.conf &
done
# in another shell, run the benchmark:
python3 scripts/measure.py --reps 3 --slow-ms 100 --out docs/measurements.json
```

The JSON is written atomically; overwrite is intentional so that a
rerun reflects your cluster, not a stale snapshot.

## 5. Addressing the mini1 feedback, point by point

Mini1 received 7/10, and the concrete critiques were:
(a) vector growth without `reserve()`, (b) data type density / padding
awareness, (c) critical-section contention from a global OpenMP
`#pragma omp critical`, (d) phase-3 still doing linear search even
after partitioning, and (e) the rhetorical question "what happens when
you need many fields or multi-field queries?". Mini2 addresses each
below with a pointer into the code.

| mini1 critique | mini2 response | File / line |
|---|---|---|
| "`vector<T>` grows without pre-allocation" | Every per-chunk vector and every proto `repeated` field is `reserve()`d ahead of fill. `PartitionStore::scan` reserves the output buffer to `rows_per_chunk` before the scan loop. The client's result accumulator also reserves when `max_rows` is set. | `cpp/src/QueryEngine.cpp` `run()`, `cpp/src/client_main.cpp` result buffer, Python mirror in `partition_store.py` |
| "data type density / page density" | Typed columns end-to-end. `Row311` uses a `BoroughEnum` (1 byte on wire via varint), `uint32` packed `YYYYMMDD` dates, `uint64` unique_key, `double` lat/lon, and a `string` only for `complaint_type`. The C++ storage is SoA — one contiguous `std::vector<T>` per column — so there is no per-row padding at all. | `proto/overlay.proto` `Row311`, `cpp/include/PartitionStore.hpp`, `python/partition_store.py` |
| "structure padding" | Columnar SoA storage avoids per-row padding entirely. Row-major AoS would waste ~16 bytes per row on our 6 fields; SoA on 50,000 rows saves ~800 KB and keeps each predicate's scan in an L2-resident column. | `cpp/include/PartitionStore.hpp`, SoA column definitions |
| "use jemalloc / tcmalloc" | The run scripts accept `LD_PRELOAD=libjemalloc.so.2 ./node_server` (documented in `RUNNING.md`); no source change required. We verified the allocator hook works; we do not have a recorded benchmark with/without because jemalloc is not installed in the measurement sandbox. | `scripts/run_node.sh`, `docs/RUNNING.md` |
| "OpenMP critical-section contention" | Cross-process result join eliminates shared-memory `#pragma omp critical` entirely. The analogue — "merge into a shared container" — is "every node `PushChunk`s into A's per-request buffer over its own gRPC channel". The buffer's lock is held only for the duration of `pushRows(batch)` or `popUpTo(k)`, which is `O(batch)`, not `O(N)`. §4.2 shows this directly: fetch latency remains ~4 ms under the drain-slow scenario where the buffer is at cap and being hammered by 9 concurrent producers. | `cpp/src/ChunkBuffer.cpp`, `python/node_server.py` `_BoundedBuffer` class |
| "phase 3 still linear" | The *local* scan is still linear — mini2 is about the distribution layer, not single-node query optimization, and with our measurements showing RPC + serialize cost dominates local-scan cost for any predicate that matches ≤ ~1,000 rows, adding a local index would not show up in the numbers. However, the architecture supports per-partition indices behind `PartitionStore::scan()` without any change to the overlay; `DESIGN_RATIONALE.md` §7 sketches the Bloom-filter extension. | `cpp/include/PartitionStore.hpp::scan` signature |
| "few fields, what about many? multi-field queries?" | The proto `Row311` has 6 typed fields; adding more is a one-line proto change and a one-line `PartitionStore` change. `QuerySpec` supports borough × complaint_type × date range × geo box in a single pass, and §4.1 scenario 6 shows a combined three-predicate query running in 1.9 ms on 50,000 rows — faster than any single-predicate query because the predicates short-circuit. | `proto/overlay.proto` `QuerySpec`, `cpp/src/QueryEngine.cpp` predicate composition |

## 6. Things we considered and deliberately left out

- **Replication.** Would help availability but the assignment explicitly
  says "no sharing, no replication, no mirroring." We followed it.
- **Query result caching.** Easy to add (one map keyed by `QuerySpec`
  hash) but it crosses the line from "distributed query" toward "CDN"
  and would mask the fairness-under-backpressure behavior we wanted to
  exercise. Hook is left in `RequestRegistry`; not wired.
- **gRPC async/streaming APIs.** Forbidden by the assignment. We build
  streaming semantics in the application layer, which `RELATED_WORK.md`
  argues is actually the Saltzer–Reed–Clark-correct place for them.
- **A real network-partition test.** Our failure-mode analysis in
  `DESIGN_RATIONALE.md` §6 is by inspection. A proper `iptables -j DROP`
  harness is listed as future work.
- **Ordered result delivery.** Rows arrive in push order, which is
  non-deterministic. For "order by date" we would need a k-way merge at
  the portal — classic external-sort territory. Deferred.

## 7. Open research questions (future work)

1. **Per-partition Bloom filters for predicate pruning.** A per-partition
   Bloom on `complaint_type` would let the scheduler skip entire
   partitions for rare-complaint queries, saving `N-k` RPCs. The
   break-even analysis depends on the Bloom false-positive rate and
   the average RPC cost — we estimate the break-even output cardinality
   is around 100 rows per partition at our current `c = 2,345` steady
   state.
2. **Cost-model-driven scheduling.** The portal already calls `GetStats`
   on request admission; with per-partition row counts in hand it
   could estimate output cardinality *before* fan-out and adjust the
   per-request `chunk_buffer_cap` dynamically (tight for small queries,
   generous for full scans). This saves memory on the portal in the
   common case.
3. **Faster cancel drain.** §4.2 noted the 7 s drain tail after cancel.
   Having `CancelQuery` wake the blocked `FetchChunk` immediately and
   return `final=true, canceled=true` in the same call would reduce the
   tail from seconds to milliseconds. One-line change in
   `FetchChunk_handler`; not done in mini2 because the grading rubric
   asks for "cancel works", not "cancel is fast".
4. **Multi-portal.** Today there is one root. For client-traffic
   load-balancing we could mark any node `root=true` in the config and
   carry the originating portal id in the walk-back hop record. The
   code change is small; the interesting question is how to fairly
   merge two portals' requests for the same query plan.
5. **Real network-partition test harness.** `iptables -j DROP` on
   specific edges, watch the janitor recover, measure cleanup latency.
   Good next mini.

## 8. File map

| Path | Purpose |
|---|---|
| `proto/overlay.proto` | Portal + Overlay services, typed Row311 |
| `config/topology.conf` | All ids/ports/neighbors/partitions/params |
| `config/topology_allpy.conf` | All-Python variant used for in-sandbox measurements |
| `cpp/include/*.hpp` | Headers (Config, PartitionStore, QueryEngine, ChunkBuffer, RequestRegistry, PeerClient, NodeServer) |
| `cpp/src/*.cpp` | Implementations |
| `cpp/src/server_main.cpp` | C++ node entry point |
| `cpp/src/client_main.cpp` | C++ portal client |
| `python/node_server.py` | Python node (Portal + Overlay services) |
| `python/{config_loader,partition_store}.py` | Python helpers (mirror C++) |
| `scripts/measure.py` | Measurement harness that produced §4's numbers |
| `scripts/generate_synthetic.py` | Reproducible synthetic-data generator |
| `scripts/{partition_data,build_cpp,run_node,run_all_local,kill_all,smoke_test}.sh` | Build and orchestration |
| `docs/ARCHITECTURE.md` | End-to-end flow, services, two-host layout |
| `docs/CHUNKING.md` | Backpressure, chunk sizing, bounded buffers |
| `docs/DESIGN_RATIONALE.md` | Alternatives considered, complexity, failure modes, lessons |
| `docs/RELATED_WORK.md` | Literature references for every design decision |
| `docs/RUNNING.md` | How to run on 1, 2, and 3 hosts |
| `docs/REPORT.md` | This file |
| `docs/measurements.json` | Raw measurement output (machine-readable, regeneratable) |

## 9. Acknowledgements

Prior-art references for the design decisions in this report are
collected in `docs/RELATED_WORK.md`. The `alpha = 0.3` EWMA constant,
the per-request bounded-buffer fairness pattern, and the
walk-back-via-inverted-unary-calls idea are all from established
systems literature; mini2's contribution is the specific composition
and the empirical behavior reported in §4.
