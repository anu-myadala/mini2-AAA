# mini2 architecture

This document explains how a query travels through the system, end to end.

## Topology

We chose the **tree overlay** option from the assignment:

```
edges: AB BC BD BE EF ED EG AH AG AI
```

That gives the adjacency

| node | neighbors            | role        |
|------|----------------------|-------------|
| A    | B, H, G, I           | portal/root |
| B    | A, C, D, E           | inner       |
| C    | B                    | leaf        |
| D    | B, E                 | inner       |
| E    | B, F, D, G           | inner       |
| F    | E                    | leaf        |
| G    | E, A                 | leaf (py)   |
| H    | A                    | leaf        |
| I    | A                    | leaf (py)   |

The overlay is technically not a strict tree (the edges contain cycles
through `B-D-E` and `A-B-E-G-A`), so we carry a `visited` set on every
forward to break loops without requiring a spanning-tree precomputation.
The depth from A to the furthest leaf is 3 hops (e.g. `A → B → E → F`),
which exercises multi-hop forwarding and result-walk-back.

The Python language is assigned to nodes G and I in the default config;
the rest run the C++ implementation. The two implementations share the
same `.proto` so they interoperate transparently.

## Data partition

Each node owns a disjoint slice of the data. We partition on
`hash(unique_key) % 9` because:

1. it produces a near-uniform split with no replication, and
2. a leaf can run a query over its partition with zero cross-node coordination.

The partitioner (`scripts/partition_data.py`) takes raw NYC 311 CSVs and
emits one canonical partition file per node:

```
unique_key,created_ymd,borough,latitude,longitude,complaint_type
```

`unique_key` is `uint64`, `created_ymd` is the packed `uint32` YYYYMMDD
(4 bytes instead of a 24-byte ISO string), `borough` is a 1-byte enum,
`lat`/`lon` are doubles, and `complaint_type` is the only legitimately
string-typed field. This addresses the mini1 feedback that string-for-
everything wastes both memory and cache density.

## Services

There are two services on every node, defined in `proto/overlay.proto`.

### Portal — used only by external clients

* `SubmitQuery(SubmitRequest) → SubmitAck` — register a query, return a
  request id immediately. **The portal never returns rows from this
  call.** This is one of the "two-way without using request-response"
  patterns the assignment asks for: the result will be delivered later
  through a different RPC in the opposite direction.
* `FetchChunk(FetchRequest) → ChunkResponse` — client pulls rows. The
  portal blocks for up to `fetch_block_ms` if the buffer is empty and
  there are still pending producers, then returns whatever it has and
  a `final` flag plus a `suggested_next_max` chunk-size hint.
* `CancelQuery(CancelRequest) → CancelAck` — abort an in-flight request.
* `GetStats(StatsRequest) → StatsResponse` — observability.

Only node A is marked `root=true` in the config and so is the only node
that ever accepts external `SubmitQuery` calls. (The other nodes still
implement the service so the binary is symmetric.)

### Overlay — used between nodes

* `ForwardQuery(ForwardQueryRequest) → ForwardQueryAck` — fan-out the
  query downstream. The ack carries no rows; the callee starts a worker
  thread to scan its partition, then forwards to its own neighbors that
  aren't already in `visited`.
* `PushChunk(PushChunkRequest) → PushChunkAck` — push a batch of rows
  one hop upstream toward the portal. This is just a unary RPC, but
  it travels in the *reverse* direction of the original `ForwardQuery`,
  which is how we get streaming semantics without using gRPC streaming.
  The ack returns a `buffered_rows` count and an optional
  `suggested_delay_ms` so producers can throttle when the portal's
  buffer is filling up.
* `Abort(AbortRequest) → AbortAck` — propagate a cancel.
* `Ping(PingRequest) → PingResponse` — health/uptime.

## End-to-end query flow

```
client                A                  internal nodes              leaves
  | SubmitQuery(spec) |                                                |
  |------------------>| (1) create request, return request_id          |
  |<-- SubmitAck -----|                                                |
  |                   | (2) ForwardQuery -> {B,H,G,I}                  |
  |                   |---------------------->|                        |
  |                   |                       | (3) ForwardQuery onward|
  |                   |                       |----------------------->|
  |                   |                       |                        |
  |                   |             (4) every node spawns a worker     |
  |                   |             that scans its partition           |
  |                   |                                                |
  |                   |        PushChunk (rows...)                     |
  |                   |<--------------------------------------|        |
  |                   |        PushChunk (rows...)                     |
  |                   |<-------------------------------------- (forward)|
  | FetchChunk(want)  |                                                |
  |------------------>| (5) drain bounded buffer up to `want`,         |
  |<- ChunkResponse --|     return rows + suggested_next_max + final?  |
  | (loop until final)|                                                |
```

Numbered steps:

1. **Submit.** The portal (node A) creates a `RequestState` with a fresh
   request id, allocates a per-request `ChunkBuffer`, and seeds an
   "expected producers" set with all node ids. It returns the id to the
   client immediately so the call doesn't block on result production.
2. **Fan-out.** A sends a `ForwardQuery` to each of its overlay
   neighbors, with `from_node=A` and `visited=[A]`.
3. **Recursive forward.** Each callee adds itself to `visited` and
   forwards to its own neighbors (excluding `from_node` and any node in
   `visited`). The depth limit is also enforced by a hop TTL field in
   the proto. Loop edges and bidirectional links are handled by the
   visited check.
4. **Local execution.** Every node, including the portal itself, runs
   `QueryEngine::run` against its `PartitionStore` in a worker thread
   created from a pool. The engine passes rows in batches of
   `initial_chunk_rows` to a sink. The sink either deposits them into
   the local buffer (if we are the portal) or sends them upstream via
   `PushChunk`.
5. **Walk back up.** Each `PushChunk` travels exactly one hop. An
   internal node receives a chunk, looks up the route entry it stored
   during `ForwardQuery` (which records `upstream_peer`), and forwards
   the chunk one hop further toward the portal. The portal deposits the
   rows into the bounded `ChunkBuffer`. When a leaf finishes, it sends
   `done=true`; the portal flips the producer's bit in the "expected
   producers" set.
6. **Pull.** The client repeatedly calls `FetchChunk(request_id, want)`.
   The portal pops up to `want` rows (or `suggested_chunk` if the client
   passed 0), returns them, plus the `pending_producers` count and the
   `final` flag, which is true iff the buffer is empty and every
   expected producer has acknowledged done (or the request was canceled).

## Fairness, backpressure, and dynamic chunk sizing

* **Bounded buffers.** Each request's `ChunkBuffer` has a fixed
  `chunk_buffer_cap` (default 65,536 rows). When `pushRows` would exceed
  the cap, it accepts what fits and returns a short-count plus a
  `suggested_delay_ms` in the ack. The producer then sleeps for that
  long and retries. This is the primary fairness/backpressure mechanism:
  a slow consumer naturally throttles its producers without dropping
  rows.
* **Per-request isolation.** Concurrent requests own separate buffers,
  so a slow client cannot starve a fast one.
* **Adaptive chunk size.** The portal records the time between
  successive `FetchChunk` calls per request, runs an EWMA, and adjusts
  `suggested_chunk` upward when the client is fast (`<20ms` between
  fetches) and downward when the client is slow (`>500ms`). The hint is
  bounded by `min_chunk_rows` and `max_chunk_rows`. The client honors it
  by passing `max_rows=0`. This effectively tunes payload size to match
  the consumer's appetite.

## Cancellation and abandonment

* **Explicit cancel.** `CancelQuery` flips a flag in the
  `RequestState`, cancels the buffer (which clears any queued rows and
  wakes the consumer), and broadcasts `Abort` to all neighbors. Each
  internal node propagates the abort one hop further downstream and
  marks its own route as canceled. Worker threads observe the flag via
  the `is_canceled` callback in `QueryEngine::run` and stop scanning at
  the next chunk boundary.
* **Abandonment.** A `janitor` thread runs every `janitor_interval_ms`
  and finds requests whose `last_fetch_at` is older than
  `request_ttl_secs`. Those requests are canceled exactly the same way
  as an explicit cancel — so a client that crashes or just stops
  pulling doesn't leak buffers indefinitely.

## Two-host run

The topology file is the single source of truth. To run on two hosts,
edit `host=` on each `node` line so e.g. nodes A, B, C, D, E live on
`host1` and F, G, H, I live on `host2`. Start each node with
`scripts/run_node.sh <ID>` on the host where it belongs. No code change
or recompile is required — every binary picks up its identity, peers,
and partition assignment from the config at startup.

## What is *not* in here on purpose

* No gRPC streaming, async, or completion-queue API anywhere. The only
  template type used from gRPC is the synchronous `Service` base class
  and `Server::Wait`.
* No shared memory for results. The `RouteState` and `RequestRegistry`
  are in-process per node only.
* No CLI menu, no chat-style interaction, no UI.
* No web service, no JSON or YAML deserializer dependency. The config
  parser is a 60-line key-value tokenizer.
* No third-party JSON, no Boost. The C++ side links only `gRPC` and
  `protobuf`. The Python side imports `grpc` and the standard library.
