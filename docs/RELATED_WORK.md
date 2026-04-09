# Related work and intellectual lineage

This document places mini2 in the context of existing distributed-systems
literature. Nothing in mini2 is novel research; the point of this note is
to be explicit about *which* existing ideas each component borrows from,
so that design decisions are traceable to prior work rather than ad hoc.

## Distributed query execution

**Dean & Ghemawat, "MapReduce: Simplified Data Processing on Large Clusters"
(OSDI 2004).** The fan-out / walk-back shape of mini2 — one coordinator
(`A`), many partition workers, results folded back to the coordinator — is
the degenerate single-reducer form of MapReduce. Our `ForwardQuery` is the
scheduler dispatching map tasks; `PushChunk` is the shuffle phase carrying
intermediate results back; the "reducer" in our case is the client's own
drain loop, not a server-side reduce stage. We deliberately do **not**
implement speculative execution, locality-aware scheduling, or intermediate
persistence, all of which that paper introduced — those assume failures
that the assignment puts out of scope.

**Melnik et al., "Dremel: Interactive Analysis of Web-Scale Datasets"
(VLDB 2010).** Dremel's serving tree is the structural template we're
closest to. Intermediate nodes in Dremel aggregate and forward partial
answers upward along a tree; leaves scan column storage. Mini2's tree
overlay and intermediate-hop-aware routing map directly onto the Dremel
"serving tree" concept. The columnar, typed storage in `PartitionStore`
(uint8 borough, uint32 ymd, double lat/lon, plus one string) is a much
simpler descendant of Dremel's record-shredding into column stripes. We do
not implement repetition/definition levels because NYC 311 is flat.

**Kornacker et al., "Impala: A Modern, Open-Source SQL Engine for Hadoop"
(CIDR 2015)** and **Armbrust et al., "Spark SQL: Relational Data Processing
in Spark" (SIGMOD 2015)** contain the most readable modern descriptions of
per-partition predicate pushdown, which is what our `QueryEngine::run`
does: it evaluates the `QuerySpec`'s borough/date/geo predicates column by
column over the SoA store. The short-circuiting pattern — if a predicate
on `borough` rules a row out, we never touch the lat/lon columns for that
row — is straight out of Impala's vectorized scan design.

## Overlay networks and loop avoidance

**Plaxton, Rajaraman & Richa, "Accessing Nearby Copies of Replicated
Objects in a Distributed Environment" (SPAA 1997)**, via its descendants
(Pastry, Tapestry, Chord). Our partitioning — `hash(unique_key) % N` — is
consistent hashing reduced to its simplest form. Because mini2 does not
handle node join/leave, we use modular hashing rather than a full ring;
the spec is static. This is the same simplification DynamoDB took for the
bulk of its internal partitioners before elastic resharding became
important.

**Lua et al., "A Survey and Comparison of Peer-to-Peer Overlay Network
Schemes" (IEEE Comms 2005).** The visited-set-plus-dedupe-by-request-id
pattern in `ForwardQuery` is a direct application of flooding-with-TTL from
unstructured P2P overlays (Gnutella, Freenet). Our overlay has cycles —
edges `A–G` and `E–G` create one, `B–D`/`B–E`/`E–D` creates another — so a
pure tree-walk would double-count nodes reached via two disjoint paths. We
found this the hard way during smoke testing: a full scan returned 4,890
rows on a 2,000-row synthetic dataset. Adding request-id dedupe at the
receiving node (in addition to the visited-list in the message) fixed it
to exactly 2,000. Lua's §III-A describes this class of bug.

## Flow control without explicit windows

**Jacobson, "Congestion Avoidance and Control" (SIGCOMM 1988).** TCP's
contribution here is the idea that a receiver's drain rate governs a
sender's send rate, mediated by a window that shrinks and grows in response
to observed throughput. Mini2 does this at the application layer: the
`suggested_next_max` field on every `ChunkResponse` is a receiver-computed
window advertised back to the sender each round-trip, and it expands by
1.25x on fast fetches and contracts by 0.75x on slow ones (see
`RequestRegistry::recordFetch`). This is a multiplicative-increase /
multiplicative-decrease rule rather than TCP's AIMD — we do not need
slow-start because the dataset size is bounded and queries do not run
forever, so the transient cost of finding the right window is acceptable.

**Floyd & Fall, "Promoting the Use of End-to-End Congestion Control in the
Internet" (IEEE/ACM ToN 1999).** Supplies the vocabulary we use for
"backpressure with fairness" — our per-request bounded `ChunkBuffer` is an
application-level RED queue: it admits new rows up to `chunk_buffer_cap`
and then advertises a short-count + `suggested_delay_ms`, which is
equivalent to an ECN mark. Unlike TCP RED, there is no probability — the
cap is hard because we do not drop rows.

## EWMA for adaptive control

**Cohen, "Forecasting for Management Scientists" (1963).** The
exponentially-weighted moving average used in `RequestRegistry::recordFetch`
is the standard single-parameter smoother. Our `alpha = 0.3` matches the
default value recommended in Jain's *The Art of Computer Systems
Performance Analysis* (1991) ch. 14 for response-time smoothing — light
enough to damp single-fetch jitter, heavy enough to react within
~3 fetches of a step change in consumer speed. We verified this empirically
against our measurements: the slow-client run converges to
`suggested = 510 rows` (from an initial 512) within the first few fetches
and holds there for the rest of the run; the fast-client run climbs to the
ceiling (~2,345 of a 4,096 cap) by fetch #5 and also holds.

## Backpressure and fairness

**Demers, Keshav & Shenker, "Analysis and Simulation of a Fair Queueing
Algorithm" (SIGCOMM 1989).** Our per-request buffer is the simplest form
of weighted-fair queueing: each outstanding query gets its own FIFO, and
the scheduler (the portal's `FetchChunk` handler) round-robins among them
by client demand. We do not implement weights; all requests are equal.

**Fonseca et al., "X-Trace: A Pervasive Network Tracing Framework"
(NSDI 2007).** Our request-id that threads through every RPC across the
overlay is the minimal form of X-Trace. We do not ship a trace collector —
logs on each node carry the request id and can be grepped — but the
plumbing is the same: a correlation id injected at the portal and
propagated by every hop.

## Consistency and the absence of it

**Vogels, "Eventually Consistent" (CACM 2009).** Mini2 is explicitly a
single-writer / read-only store for the purposes of a query. There are no
writes at all once a query is submitted; every node's partition is immutable
for the query's duration. Consequently mini2 does not need any consistency
model beyond "read the snapshot that was there when `ForwardQuery`
arrived". If we were to add ingest, the partitioning scheme would force us
to think about per-partition commit and the lack of cross-partition
transactions — which is Vogels' whole point about why Amazon went eventual.

**Brewer, "Towards Robust Distributed Systems" (PODC 2000), i.e. CAP.**
Mini2 is in the {C, A} corner because we assume the network does not
partition. If A cannot reach an interior node, the request for the rows
that node owns will simply never complete, and the janitor thread will
garbage-collect the request after `request_ttl_secs`. We deliberately
prioritize consistency (we never return a partial result and claim it's
complete) and availability of the portal (the client can always submit and
cancel); we sacrifice partition tolerance, because the assignment forbids
replication. The PACELC refinement — "else, when partitioned, choose
latency or consistency" — would pick *consistency* for us: the portal will
wait until every expected producer has acked `done` before it flips
`final=true`, and if a producer never acks, the request hangs until the
janitor cleans it up.

## Why we did not use gRPC streaming

The assignment spec explicitly forbids the gRPC stream and async APIs. The
standard argument for gRPC server-streaming here would be
**Birrell & Nelson, "Implementing Remote Procedure Calls" (ACM ToCS 1984)**
§4 "Stream messages" — a long-running result set fits that pattern
exactly. We instead take the approach from **Saltzer, Reed & Clark,
"End-to-End Arguments in System Design" (ACM ToCS 1984)**: the
application knows the shape of the chunks better than the transport does,
and the receiver's drain rate is the only reliable signal of pace. Pushing
chunk sizing into the application layer lets us react to consumer speed in
a way that gRPC's built-in flow control — which only reflects network and
HTTP/2 window state — cannot. In practice, our adaptive chunk loop is
measurably responsive (chunk size moves 4.6x between the fast and slow
scenarios) and costs nothing beyond one EWMA update per fetch.

## What we did *not* borrow, and why

- **Paxos / Raft / any consensus.** No replication, single portal, no
  state to agree on.
- **Service discovery (Consul/etcd/ZooKeeper).** The topology is static
  and configured; discovery would be over-engineering.
- **mTLS / AuthN.** The assignment is explicit about running on trusted
  hosts; adding TLS would be a distraction from the interesting parts.
- **Bloom filters for predicate pruning.** Candidate for future work: a
  per-partition Bloom on `complaint_type` would let the scheduler skip
  entire partitions for rare complaint queries, saving a full RPC.
- **Arrow Flight.** It would be the "right" choice for a production
  distributed query engine built today, but Arrow Flight *is* gRPC
  streaming under the hood, which the assignment forbids.
