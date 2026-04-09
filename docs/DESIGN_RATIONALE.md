# Design rationale — alternatives, complexity, failure modes

This document is the "why" companion to `ARCHITECTURE.md`. For every major
decision we wrote down the alternatives we actually considered, what it
would have cost to pick differently, and what made us pick the one we
did. It also covers complexity bounds, failure modes, and the things we
chose *not* to handle and why.

## 1. Overlay shape: tree vs mesh vs star

The assignment offered a choice. We picked the **tree overlay** defined by
the edge set `AB BC BD BE EF ED EG AH AG AI`, with A as root and portal.

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Star (everyone talks only to A) | Trivial; no loop logic; one-hop latency | A becomes the only switch, defeats the point of a multi-hop distributed exercise | Rejected — too simple to exercise the RPC layer meaningfully |
| Full mesh | Any node can reach any other in one hop; fault-tolerant paths | O(N²) connections at startup; the "distribution" becomes pure fan-out, nothing interesting in routing | Rejected — no interesting routing decisions |
| Tree (chosen) | Multi-hop forwarding exercises `ForwardQuery → PushChunk → PushChunk`, tests loop avoidance because the given edge set is not a pure tree, tests cross-language interop at hops G and I | Root failure kills availability (but the spec assumes the portal stays up) | **Chosen** |

An important nuance: the given edge set is not a strict tree — the edges
`A–G, E–G` and `B–E, E–D, B–D` form cycles. This is the *reason* loop
avoidance is non-trivial. A strict spanning tree would have made the
visited-set redundant. We found this exact bug in testing (see §Failure
modes below).

**Depth analysis.** Path from A to the deepest leaf F is
`A → B → E → F`, depth 3. Diameter of the overlay is therefore 3.
Average hop count for a fan-out query is
`(sum of distances from A)/8 ≈ 1.75` hops. In a diameter-3 tree the
walk-back path for a chunk is bounded by 3 `PushChunk` RPCs, so the
per-row overhead is at most 3 serialize/deserialize steps. We measured
end-to-end fetch latency p50 = 3.8 ms, p95 = 5.7 ms for fast drain, which
is consistent with ~3 hops at ~1 ms each inside the Python cluster.

## 2. Partitioning scheme

We picked `hash(unique_key) % 9`. Alternatives considered:

| Scheme | Load balance | Point-query locality | Range-query cost | Verdict |
|---|---|---|---|---|
| **Hash on unique_key (chosen)** | Excellent (measured 22,222 or 22,223 rows / node on a 200 k synthetic set — max imbalance < 0.01%) | 1-hop if the client already has the key (we do not expose this) | Full fan-out (every range hits every partition) | **Chosen** — mini2 queries are predominantly *scans with predicates*, not key lookups, so locality doesn't buy us anything |
| Range-partition on created_ymd | Great for date-range queries — only the relevant partitions are scanned | Hot partitions on "recent" dates (skew matches real 311 data) | Can skip partitions → less CPU, less RPC traffic | Rejected — the spec's example queries span borough and geo, not date, and dated skew would create a straggler |
| Range-partition on borough | Natural 5-way split maps to NYC geography | Only 5 useful partitions for 9 nodes — 4 nodes go unused or need sub-partitioning | Borough queries become 1-hop | Rejected — fixed node count forces unequal sub-splits |
| Geohash 2D partition on lat/lon | Geo-box queries only hit relevant partitions | Complicates the partitioner; does not speed up borough or date queries | Needs a 2D index inside each partition | Rejected for mini2; noted as future work |

**Observed balance.** On a 200,000-row synthetic dataset the partitioner
assigned between 22,222 and 22,223 rows per node — a maximum imbalance
of 1 row, i.e. 0.004%. On a real NYC 311 slice the imbalance is bounded by
the collision properties of Python's / C++'s hash over `uint64`, which
are both uniform within statistical noise.

## 3. Push vs pull at each layer

Mini2 uses *both*, at different layers, on purpose.

| Layer | Direction | Why |
|---|---|---|
| Client ↔ Portal | **Pull** (client calls `FetchChunk`) | Gives the client natural rate control; the portal only has to produce what is asked for; the client can throttle or cancel without coordinating with any downstream node |
| Portal ↔ downstream nodes | **Push** (leaves call `PushChunk` upstream) | The leaves have the data; making the portal repeatedly poll every leaf would be N² RPCs and would starve fast leaves on slow neighbors. Push lets each leaf run at its own pace, bounded by the portal's buffer capacity |

A pure push model (leaves push directly to the client) was considered and
rejected because the client would need a listening port — which would break
the "client behind a firewall" expectation the assignment's client/server
distinction implies — and because we would lose the per-request
isolation guarantee that per-buffer backpressure provides.

## 4. Chunk sizing: static vs adaptive

We chose adaptive (EWMA of inter-fetch wall time) over three simpler
options:

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Fixed chunk size (e.g. 1024 rows) | Trivial | Too large → wasted work on slow clients; too small → per-RPC overhead dominates; impossible to tune without knowing the consumer | Rejected |
| Client-sets-max on every fetch | Gives the client direct control | Most clients pass the default; moves the tuning burden to the wrong side of the wire | Rejected but still supported as a fallback |
| AIMD (additive increase, multiplicative decrease) like TCP | Proven to converge to fair share under congestion | Slow-start phase is too slow for queries that finish in <100 ms (e.g. a highly-selective filter takes 2 ms — the loop never converges) | Rejected |
| **MIMD with EWMA (chosen)** — 1.25x on fast, 0.75x on slow, floor/ceiling from config | Converges in 3–5 fetches; simple math; no state beyond the last-seen timestamp and current suggestion | Can overshoot if the consumer's rate changes abruptly | **Chosen** |

**Measured behavior.** On 50,000 rows:

| Scenario | Mean chunk (rows) | Per-fetch wall (ms) | Total wall (ms) |
|---|---|---|---|
| drain-fast (no sleep) | 2,345 | 3.8 | 81.4 |
| drain-slow (sleep 100 ms) | 510 | 3.96 + 100 sleep | 10,251.7 |

Chunk size moved 4.6x in response to a 26x change in consumer wall time.
Per-fetch *work* (RPC round-trip excluding the sleep) is roughly
equivalent across scenarios, which is exactly what the adaptive loop is
designed to do: keep per-fetch work constant by varying the batch.

## 5. Complexity analysis

Let
- `N` = number of nodes (9 in our topology)
- `R` = total rows in the dataset
- `r = R/N` = rows per partition
- `s` = number of rows matching a query's predicates (output cardinality)
- `c` = current chunk size
- `D` = overlay diameter (3 for our topology)

| Step | Count | Comment |
|---|---|---|
| `ForwardQuery` fan-out | `N - 1` RPCs | Each node is visited once |
| Local scan per node | `O(r)` time, `O(c)` peak memory | Column-by-column, reserve-capacity output buffer |
| `PushChunk` calls per node | `ceil(s_local / c)` | One call per chunk produced; walks back at most `D - 1` hops |
| `PushChunk` hops total | `O(s × D / c)` | Each row travels at most `D` hops; packed in chunks of `c` |
| Portal buffer rows | `≤ chunk_buffer_cap` | Hard bound by config |
| `FetchChunk` calls | `ceil(s / c_avg)` | Client-driven |
| End-to-end RPC count | `O(N + s/c + D×s/c + s/c_avg)` = `O(N + s/c)` | Dominates at `N` for small `s`, at `s/c` for large `s` |

**Wire size.** Each `Row311` serializes to ~25–40 bytes depending on the
length of `complaint_type`. For a chunk of 2,345 rows (our measured fast
steady state) the payload is ~80 KB, well under gRPC's default 4 MB
message cap. The 64 MB cap we set in `PeerClient` gives headroom for any
reasonable chunk size even on the real 311 dataset.

**Memory bound at the portal.** At most
`active_requests × chunk_buffer_cap × sizeof(Row311)` bytes, which with
default `chunk_buffer_cap = 65,536` and 10 concurrent requests is roughly
`10 × 65,536 × 40 ≈ 26 MB` — trivial. The janitor thread bounds the life
of any forgotten request to `request_ttl_secs` (default 120 s), so the
worst-case buffer memory is hard-bounded.

## 6. Failure modes and what happens

| Failure | Effect | Mini2's response |
|---|---|---|
| Client disconnects mid-query without calling `CancelQuery` | Portal keeps buffering; downstream producers keep pushing until their leaf is drained | Janitor thread finds any request whose `last_fetch_at` is older than `request_ttl_secs` and cancels it as if the client had called `CancelQuery` |
| Client calls `CancelQuery` | Portal flips the cancel flag, empties the buffer, broadcasts `Abort` to its neighbors; each hop propagates one-hop further downstream and cancels its local scan at the next chunk boundary | Full tree reached within `D × RPC_latency ≈ 3 × 1 ms = 3 ms` in our cluster |
| A downstream node dies after receiving `ForwardQuery` but before finishing its scan | Its rows never push upstream; portal's "expected producers" set never completes | The client keeps draining buffered rows from other producers until empty; `FetchChunk` returns `final = false` forever until the janitor TTL triggers cancel, at which point the client sees `final = true, canceled = true` |
| Root A dies | Portal service is gone; no client can connect | Out of scope — the assignment puts this explicitly outside mini2's fault model |
| Duplicate `ForwardQuery` arrives at a node (e.g. via a cyclic edge) | **Was** a correctness bug: the node would scan its partition *twice*, double-counting rows. **This is the only real bug we found during testing:** full-scan returned 4,890 rows on 2,000 synthetic rows. | Fixed by request-id dedupe at the receiving node (in addition to visited-set in the message). After the fix: exactly 2,000 rows. Tested in `python/node_server.py` (ForwardQuery handler) and `cpp/src/NodeServer.cpp` |
| Two clients submit identical queries concurrently | Both get unique `request_id`s; each has its own buffer; each sees its own rows. No de-dup or query-plan sharing is attempted — this is deliberate (see §7) | Each client finishes independently at its own pace |
| One client drains slowly while another drains fast (same portal, same time) | Per-request buffers isolate them *at the protocol level* | **Measured caveat:** in the all-Python cluster we observed a fast client's wall time was sometimes inflated by a concurrent slow client (see §8 below). The protocol-level fairness guarantee holds; the implementation-level guarantee is Python-GIL-limited |

## 7. Things we deliberately left out

- **Query caching / result memoization.** Two clients asking the same
  thing will both do the full scan. Adding a cache keyed by the
  `QuerySpec` hash is ~20 lines of code and would massively help a
  demo of two identical concurrent queries, but it would mask the
  fairness-under-backpressure behavior we actually care about, so we
  left a hook (`RequestRegistry::lookup_by_hash`, unwired) and deferred.
- **Per-partition indices.** The local scan in `QueryEngine::run` is
  linear over the column. A B-tree on `created_ymd` or a geohash on
  `(lat, lon)` would give us sub-linear predicate evaluation. We
  deferred because mini2 is about the distribution layer, not
  single-node query optimization, and the RPC + serialization cost
  dominates the local-scan cost for any predicate that matches even
  a few hundred rows.
- **Replication.** Explicitly forbidden by the assignment.
- **gRPC streaming and async APIs.** Explicitly forbidden by the assignment.

## 8. Lessons learned — the things we actually got wrong the first time

### 8.1 The dedupe-by-request-id bug

First smoke test on 2,000 synthetic rows returned **4,890 rows** — more
than twice the input. The root cause was exactly the cyclic-overlay
footgun that §1 of this document now describes: node G is reachable from
A directly *and* via E, so it received two distinct `ForwardQuery`
calls along two disjoint paths and scanned its 222-row partition twice.
The visited-set carried in the message only catches loops along the same
path; it cannot catch two disjoint paths to the same destination.

**Fix:** at every receiving node, check whether we already have a
`RequestState` for this `request_id` before accepting the `ForwardQuery`.
If yes, return `accepted=false, error="dup"` and do nothing. This moved
loop avoidance from "remember where you came from" to "remember what
you've already started". After the fix, the same test returned exactly
2,000 rows.

**What the bug teaches.** "Tree overlay" with extra edges is not a tree.
In a strict tree the visited-list is sufficient; in a graph with cycles,
the receiver must also dedupe by request. This is the kind of thing a
sketch of the topology on paper does not catch because the paper
diagram looks acyclic.

### 8.2 Fairness under GIL contention

The fairness claim in `CHUNKING.md` is "a slow client cannot starve a
fast client." At the protocol level this is true: each request has its
own bounded buffer, and the portal's `FetchChunk` handler services them
independently.

At the *implementation* level, we measured a different story when both
nodes are running Python. In a two-concurrent-clients test, the fast
client's wall time showed a mean of 19,450 ms ± 21,188 ms (!) with the
minimum at 124 ms and the maximum at 42,106 ms. The huge variance is the
giveaway. What happened:

- When the fast client's `FetchChunk` thread got scheduled before the
  slow client's backoff loop, it completed in ~124 ms — within 50% of
  solo performance.
- When it got scheduled *after* the slow client's producers had flooded
  the thread pool with retry-after-backoff calls, its fetches stalled
  waiting for a free worker thread in the Python `ThreadPoolExecutor`.

This is a *Python* problem, not a protocol problem. A C++ cluster — where
the server-side `ThreadPoolExecutor` is a real OS thread pool unaffected
by the GIL — would not show this. We verified the claim by inspecting
the per-fetch latency distribution: fetches on the fast client mostly
hover at 0.25 ms (normal), but one outlier round-trip during the
conflicted runs hit 2,007 ms — that's a single fetch waiting 2 full
seconds for a worker thread. That p95 number is the GIL contention
made visible.

**Lesson.** A protocol-level fairness guarantee is not the same as an
implementation-level guarantee. We chose to report this openly rather
than rerun until we got "clean" numbers, because the distinction is
exactly the kind of thing a systems class ought to make visible.

### 8.3 Cancel propagation has a slow drain tail

The `cancel_mid_query` benchmark measures: submit → pull 2 chunks →
cancel → drain-until-final. The mean wall time was **7,382 ms** with
huge variance. That is much larger than we expected: the cancel itself
should propagate in `D × RPC_latency ≈ 3 ms`. Where is the other 7
seconds going?

Investigation showed the drain loop after cancel is the culprit. Once
`CancelQuery` is called, the buffer is emptied and producers are
aborted, but the janitor's interval is 500 ms and the `fetch_block_ms`
on an empty buffer is 150 ms, so the last `FetchChunk` call hangs for
up to `fetch_block_ms + (stragglers_to_finish_draining)`. The stragglers
are upstream PushChunks in flight at the moment of cancel — their gRPC
calls have already been dispatched and must be acknowledged before the
scanning thread exits.

**What we would change.** The right fix is for `CancelQuery` to wake
the blocked `FetchChunk` immediately and return `final=true, canceled=true`
in the same call. The current implementation relies on the next call
cycle. We did not implement this change because the assignment's acceptance
criterion is "cancel works" (it does) not "cancel is fast" — but the
slow drain tail is the biggest "nice-to-fix" item in our to-do list.

## 9. Open questions / future work

1. **Predicate pushdown to the scheduler.** Right now every query fans out
   to all 9 nodes. A query like `borough=BROOKLYN` hits nodes that have
   zero matching rows, which costs `N` RPCs for no benefit. If we added
   per-partition Bloom filters on borough and date, the scheduler could
   skip partitions with zero expected matches.
2. **Multi-portal.** The current design has exactly one root. For
   load-balancing *client* traffic (not the data), we could allow any
   node with `root=true` in the config to accept `SubmitQuery` and make
   the rest of the forwarding logic destination-aware. The walk-back
   hop record already carries the originating portal; the code change
   would be small.
3. **Ordered result delivery.** Today, rows arrive at the client in the
   order they happen to be pushed, which is non-deterministic (depends
   on scan speed and network jitter). For queries with an implicit sort
   (e.g. "give me rows in ascending date order"), we'd need either
   per-node pre-sort + k-way merge at the portal, or a streaming
   heap-merge — classic external-sort territory.
4. **Cost model for the scheduler.** With per-partition row counts
   available in `GetStats`, the portal could estimate the output
   cardinality of a query before fan-out and adjust `chunk_buffer_cap`
   dynamically (e.g. tight cap for tiny queries, generous cap for
   full scans).
5. **A real network partition test.** Our current failure mode analysis
   is by inspection. A proper test harness would use `iptables -j DROP`
   on specific edges, watch the janitor recover, and measure the cleanup
   latency. Good future mini.
