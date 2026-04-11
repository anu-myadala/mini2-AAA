# Mini 2 — What We Built and Why

## The Problem

The assignment asks: given a 9-node distributed process network, how do you
query data spread across all nodes efficiently, control memory usage at the
caller, and be fair between concurrent clients?

Our answer is a scatter-gather engine with three properties:
1. **Pull-based delivery** — the client controls the rate by asking for chunks
2. **Adaptive chunk sizing** — chunk size adjusts to how fast the client consumes
3. **Result caching** — repeated identical queries skip the full fan-out

---

## File-by-File Breakdown

### `proto/overlay.proto`
Defines two gRPC services on every node. All RPCs are **unary** (no streaming)
by assignment requirement. Streaming semantics are implemented at the app layer
via repeated `PushChunk` + `FetchChunk` calls.

- `Portal` service: `SubmitQuery`, `FetchChunk`, `CancelQuery`, `GetStats` — only node A serves real clients
- `Overlay` service: `ForwardQuery`, `PushChunk`, `Abort`, `Ping` — all nodes expose this for node-to-node communication
- `Row311` message: typed fields — `uint64` unique key, `uint32` YYYYMMDD date, `BoroughEnum` (int32 on wire), `double` lat/lon, `string` complaint type
- `QuerySpec` message: predicate fields (0/empty = "any")

### `config/topology.conf`
Runtime topology. Nothing about node identity, ports, or partitions is compiled
into the binary. Each server reads this at startup and uses `--id` to find its
own entry.

Tree edges exactly match the assignment spec: AB, BC, BD, BE, EF, ED, EG, AH, AG, AI.

Tunable params: chunk sizes, buffer capacity, cache capacity, janitor TTL.

### `cpp/include/Record311.hpp`
Domain type for a single 311 complaint. Field order minimizes struct padding:
8-byte fields first (uint64, double×2), then 4-byte (uint32), then 1-byte (Borough enum),
then string. No hidden gaps from misaligned fields.

`parseYmd()` converts "YYYY-MM-DD..." to a packed `uint32_t YYYYMMDD` — avoids
storing date strings, saves ~10 bytes per row.

### `cpp/include/PartitionStore.hpp` / `cpp/src/PartitionStore.cpp`
Structure-of-Arrays (SoA) columnar storage. Six separate vectors — one per field.
Cache-friendly for predicate scans: a borough filter streams through only the
`boroughs_` vector, staying in L1/L2.

`load()` estimates row count from file size before reading, calls `reserve()` on
all vectors to prevent repeated realloc. (Mini 1 feedback addressed.)

Accepts both the raw NYC 311 OpenData CSV header ("Created Date", "Borough", etc.)
and the canonical partition header output by `partition_data.py`.

### `cpp/src/QueryEngine.cpp`
Single-pass column scan. Checks predicates in cheapest-first order
(borough enum = 1 byte, date = 4 bytes, geo = 2×double, string last).
Polls the cancel flag every 1024 rows to bound response time on cancellation.
Emits matching rows as chunks via callback.

### `cpp/include/ChunkBuffer.hpp` / `cpp/src/ChunkBuffer.cpp`
Bounded per-request producer/consumer queue. Capacity is capped at
`chunk_buffer_cap` (default 65536 rows). When full, `pushRows()` returns a
short-count — producers receive a `suggested_delay_ms` hint and sleep before
retrying. This is the backpressure mechanism.

`isFinal()` returns true when all expected producers have called `markProducerDone()`
AND the queue is empty. `FetchChunk` uses this to signal the client that the
result set is complete.

### `cpp/include/QueryCache.hpp` / `cpp/src/QueryCache.cpp`
LRU result cache at the portal. Key = hash of all `QuerySpec` fields
(hash-combine over ints, strings, doubles). Value = `shared_ptr<const vector<Row311>>` —
immutable, shared safely across concurrent readers.

`unordered_multimap` handles hash collisions: on lookup, all entries with the
same hash are checked with `specEqual()`. Capacity and max-rows-per-entry come
from `config/topology.conf`.

Cache is populated by `FetchChunk` (shadow-accumulates rows as it sends them to
the client). On `final=true` and not-canceled, the accumulated vector is moved
into the cache. Cache hits skip the full 9-node fan-out entirely.

### `cpp/include/RequestRegistry.hpp` / `cpp/src/RequestRegistry.cpp`
Portal-side per-request state. Each request has its own `ChunkBuffer` (fairness:
a slow client on request R1 does not block request R2).

`RequestState::recordFetch()` implements EWMA adaptive chunk sizing (α=0.3):
if inter-fetch time < 20ms, increase `suggested_chunk` by 25%; if > 500ms,
decrease by 25%. Bounded by `[min_chunk, max_chunk]`.

`collectExpired()` supports the janitor thread — requests whose `last_fetch_at`
is older than `request_ttl_secs` are considered abandoned and cleaned up.

### `cpp/include/NodeServer.hpp` / `cpp/src/NodeServer.cpp`
The main server class. Registers both Portal and Overlay services on the same
gRPC server.

Key flows:
- **SubmitQuery**: checks cache first. On miss, creates a `RequestState`,
  registers a route (upstream = `__self__` sentinel), fans out `ForwardQuery`
  to all neighbors, and spawns a local worker thread for the node's own partition.
- **ForwardQuery**: checks the visited set to break cycles (the tree has the edge
  ED creating a potential B→D←E cycle). Creates a `RouteState` recording the
  upstream peer. Fans out to unvisited neighbors. Spawns local worker.
- **PushChunk**: if we are the portal, deposit rows in the `ChunkBuffer`. Otherwise
  forward one hop upstream toward the portal using the `RouteState`.
- **FetchChunk**: drains the buffer, accumulates into `cache_accum`, updates EWMA.
  On final, stores result in `QueryCache`.
- **Janitor thread**: runs every `janitor_interval_ms`, cancels requests idle
  longer than `request_ttl_secs`.

### `cpp/src/server_main.cpp`
Entry point. Parses `--id` and `--config` flags. No defaults for identity —
forces explicit configuration at startup.

### `cpp/src/client_main.cpp`
Demo client. SubmitQuery → loop FetchChunk until final. Uses `suggested_next_max`
from each response to drive adaptive chunk requests. Supports `--slow` flag to
demonstrate backpressure.

### `python/node_server.py`
Python implementation of both services. Same proto, same topology config.
Nodes G and I run this by default. Interoperates with C++ nodes transparently
since all communication goes through the proto wire format.

### `python/partition_store.py`
SoA storage using `array.array` typed buffers (`'Q'` for uint64, `'I'` for
uint32, `'d'` for double) instead of Python lists. Avoids Python object overhead
per element.

### `scripts/partition_data.py`
Splits a raw NYC 311 CSV into 9 files using `hash(unique_key) % 9`. Handles the
NYC OpenData date format "MM/DD/YYYY HH:MM:SS AM" → packed YYYYMMDD. Output
header is canonical so both C++ and Python `PartitionStore` can load it.

### `scripts/build_cpp.sh` / `scripts/gen_proto.sh`
Build automation. `build_cpp.sh` runs CMake + make; proto stubs are generated
automatically by the CMake custom command. `gen_proto.sh` regenerates the Python
stubs separately.

### `scripts/run_node.sh`
Looks up the `lang=` field for the given node ID in the config file, then runs
either the C++ binary or the Python server. Source of identity is the `--id`
flag, not the script — consistent with the no-hardcoding requirement.

---

## Design Decisions

### Why unary RPCs instead of gRPC streaming?
Assignment constraint. The benefit: we control chunking, backpressure, and
cancellation explicitly, which makes the algorithm transparent and measurable.

### Why the tree + visited-set approach instead of a spanning tree?
The assignment-specified tree has the BD + ED edges (D connects to both B and E).
A naive fan-out would send duplicate queries to D. The visited set, propagated
in `ForwardQueryRequest.visited`, prevents this without needing a spanning tree
pre-computation.

### Why FetchChunk for cache accumulation instead of PushChunk?
FetchChunk is called sequentially by one client per request, so there is no
concurrent access to `cache_accum` — no mutex needed. PushChunk is called
concurrently by multiple producer threads, making safe accumulation there more
complex (producer retries would double-count rows on backpressure).

### Why hash partitioning?
Uniform load distribution. Each of the 9 nodes gets ~11% of the 2M+ row dataset.
Range partitioning by date would skew recent months; by borough gives uneven
partitions (5 boroughs, 9 nodes). Hash is simpler and fairer.
