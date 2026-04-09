# Chunking, fairness, and backpressure

The interesting work of mini2 is not the gRPC plumbing — it's the
**rate-control loop** that lets a single portal serve multiple clients
fairly without blowing up memory or wasting bandwidth.

## The actors

```
producers (every node)        portal A                      client(s)
   QueryEngine.run                ChunkBuffer                  FetchChunk
       |                              |                            |
       | PushChunk(rows)              |                            |
       |----------------------------->| pushRows -> bounded queue   |
       |                              |                            |
       |    [if push short-counts,    |                            |
       |     producer sleeps backoff] |                            |
       |                              |--------------------------->|
       |                              |   ChunkResponse(rows)      |
       |                              |   suggested_next_max       |
       |                              |   final / pending_producers|
```

## What "fair" means here

Mini2's fairness goal is between **client-end requests**, not between
producer nodes. The system promises:

1. A slow client cannot blow the portal's RAM by submitting a query
   that produces millions of rows then stalling.
2. A slow client cannot starve a fast client running concurrently.
3. A fast client can drain at full bandwidth and the system will scale
   the per-fetch payload up to amortize the per-call overhead.

We meet these by giving every request its own bounded `ChunkBuffer`,
combined with adaptive chunk sizing and a janitor for abandoned
requests.

## Bounded buffer + push backoff

`ChunkBuffer::pushRows(rows)` is the choke point. It:

* takes the lock,
* computes `take = min(rows.size(), cap - q.size())`,
* moves `take` rows into the queue,
* notifies waiters,
* returns `take`.

If `take < rows.size()` the producer received a `PushChunkAck` with a
non-zero `suggested_delay_ms` and sleeps that long before retrying. The
producer side keeps the unsent tail of its rows; nothing is dropped.

This maps backpressure cleanly across hops: an internal node's
`PushChunk` handler is itself just calling `up->pushChunk(...)`, so a
short-count at the portal propagates up the chain naturally without any
extra protocol.

## Per-request isolation

Concurrent requests live in `RequestRegistry`. Each one carries its own
`ChunkBuffer`. Producers tag every `PushChunkRequest` with a
`request_id`, so a slow drain on request `R1` only fills up `R1`'s
buffer; producers for `R2` keep flowing as long as `R2`'s consumer is
keeping up.

## Adaptive chunk size

The portal observes the wall time between successive `FetchChunk` calls
on a given request. It maintains an EWMA (`alpha=0.3`) and adjusts the
chunk-size hint:

```
fast (<20ms):  suggested *= 1.25, capped at max_chunk_rows
slow (>500ms): suggested *= 0.75, floored at min_chunk_rows
```

`suggested_next_max` is returned in every `ChunkResponse`. The client
passes `max_rows=0` and just uses whatever the portal suggests, which
turns each fetch into "give me approximately 150 ms worth of work
right-sized for what my consumer is actually keeping up with".

This is the closest you can get to dynamic stream-control with only
unary RPCs — the chunk size is the analogue of a flow-control window
that both sides agree on without ever opening a streaming channel.

## Anticipation (the side note)

The architecture leaves room for two cheap forms of anticipation:

1. **Fan-out as soon as Submit returns.** The portal kicks off
   `ForwardQuery` to all its peers and starts its own scan before the
   client has called `FetchChunk` even once. By the time the first
   fetch arrives the buffer is already filling.
2. **Suggested next size lookahead.** Because the portal computes the
   next chunk size from the EWMA at fetch time, the system anticipates
   the consumer's expected appetite for the *next* round, not the
   current one.

A more aggressive design could pre-cache popular query plans (e.g. by
borough) and reply from cache, but that would cross the line from
"distributed query" to "CDN" and was deliberately left out.

## Knobs

All in `config/topology.conf` under `param` directives. None of these
are compile-time constants.

| key | default | meaning |
|-----|---------|---------|
| `initial_chunk_rows` | 512 | starting chunk size suggestion |
| `min_chunk_rows`     | 64  | floor for adaptive chunk sizing |
| `max_chunk_rows`     | 4096 | ceiling for adaptive chunk sizing |
| `chunk_buffer_cap`   | 65536 | per-request bounded buffer |
| `request_ttl_secs`   | 120 | janitor TTL for abandoned requests |
| `fetch_block_ms`     | 150 | how long FetchChunk blocks if buffer empty |
| `push_backoff_ms`    | 25  | hint sent to producers when buffer is full |
| `janitor_interval_ms`| 500 | janitor poll period |
