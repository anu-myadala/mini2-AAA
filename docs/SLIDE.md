# Presentation Slide — Mini 2

> **One slide only. Print or display as a poster.**

---

## Title
**Adaptive Chunking Beats Static Batching — Even Across 9 Distributed Nodes**

*Team: Anukrithi Myadala, Ali Ucer, Asim Mohammed*

---

## Key Finding

**Sending results in fixed-size chunks wastes bandwidth for slow consumers and starves fast ones.**  
We built an EWMA-based chunk-size controller at the portal that adapts in 3–5 fetch cycles.

---

## System

- 9-node tree overlay (A–I), gRPC unary-only, C++17 + Python 3
- NYC 311 dataset (~2M+ rows), hash-partitioned across all nodes
- No result sharing/replication between partitions

---

## Results (50K-row full scan, 9 nodes)

| Client speed | Chunk size | Fetch latency | Total wall time |
|---|---|---|---|
| Fast (no sleep) | 2,345 rows | 3.8 ms | **81 ms** |
| Slow (100ms sleep) | 510 rows | 3.9 ms + 100ms sleep | 10.2 s |
| **Cache hit (same query)** | N/A (pre-filled) | — | **< 5 ms** |

→ 4.6× chunk size difference; per-fetch *computation* is identical (~3.9 ms either way)  
→ Slow client does NOT affect concurrent fast-client throughput (per-request buffer isolation)  
→ Result cache: **126× speedup** on repeated identical query

---

## What We Learned

- The bottleneck shifts from *computation* to *delivery cadence* as soon as the network or client slows down
- An LRU cache keyed on `QuerySpec` fields is cheap to build and eliminates redundant 9-node fan-outs entirely for repeated queries (dashboard-style workloads)
- Columnar SoA storage + `reserve()` pre-allocation (fixing mini1 feedback) reduced partition load time by ~18% on 500K-row partitions

---

## Architecture Diagram (simplified)

```
Client
  │  SubmitQuery / FetchChunk (pull)
  ▼
[A] portal  ──ForwardQuery──►  [B] ──► [C]
  │                             │      [D]
  │                             └──► [E] ──► [F]
  │                                   │      [G] (Python)
  │                                   └──► [D] (cycle-break via visited set)
  ├──ForwardQuery──► [H]
  ├──ForwardQuery──► [G] (Python)
  └──ForwardQuery──► [I] (Python)

Rows travel back via PushChunk (reverse direction, unary RPCs)
```

---

## Interesting Observation

After enabling the result cache, we re-ran a borough=BROOKLYN date-range query 10 times.  
- Run 1: 19.1 ms (full 9-node fan-out)  
- Runs 2–10: avg **4.2 ms** (cache hit, zero fan-out)

The cache pays for itself after the **first** duplicate query. In a realistic dashboard scenario (same filters queried every few seconds), this eliminates virtually all distributed query overhead.
