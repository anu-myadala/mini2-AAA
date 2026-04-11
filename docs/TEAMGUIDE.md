# Team Guide — What We Built, How to Demo It, What to Present

---

## ELI5: What is Mini 2?

Imagine a library with 9 branches. Each branch has a different set of books (NYC 311 complaint records). A customer walks up to the main branch (node A) and says "give me all noise complaints in Brooklyn." The main branch can't answer alone — it needs to ask the other 8 branches and collect their answers.

Mini 2 is exactly that, but with rules:
- The 9 branches are connected in a tree (not all-to-all) — think of a phone tree
- Communication is plain gRPC calls, no streaming API allowed
- The customer might be slow (reading books one page at a time), or fast. The system has to handle both without crashing

The 3 interesting problems we solved:
1. **Fan-out + collect**: node A broadcasts the query down the tree, each node sends back what it found
2. **Backpressure**: if the client is slow, node A stops asking for more rows so memory doesn't explode
3. **Caching**: if someone asks the same question twice, skip the whole fan-out and return instantly

---

## What Was Built to Meet Each Requirement

| Requirement | What we did |
|---|---|
| 9-node tree overlay | Nodes A–I wired per the assignment spec (AB, BC, BD, BE, EF, ED, EG, AH, AG, AI). Topology lives in `config/topology.conf`, not hardcoded. |
| gRPC unary only, no streaming | All 8 RPCs are unary. We fake streaming with repeated PushChunk + FetchChunk calls. |
| Client paces delivery | FetchChunk returns `suggested_next_max` — the client uses that number on the next call. EWMA adapts: fast client gets bigger chunks, slow client gets smaller ones. |
| Cross-language | C++17 for nodes A,B,C,D,E,F,H. Python 3 for nodes G and I. Same proto, same config. |
| No hardcoded IDs/ports | Binary reads `--id X --config topology.conf`. Config file is the single source of truth. |
| Cycle avoidance | BD + ED edges form a triangle. A `visited` set propagated in ForwardQueryRequest prevents duplicate fan-out. |
| Cancellation | Client sends CancelQuery. Propagates downstream as Abort. Janitor thread cleans up abandoned requests after TTL. |
| Mini1 feedback: reserve() | PartitionStore estimates row count from file size (filesize ÷ 60 bytes), calls `reserve()` on all vectors before reading. No realloc during load. |
| Mini1 feedback: typed fields | `unique_key` is uint64, date is uint32 YYYYMMDD, borough is a 1-byte enum, lat/lon are double. No strings-for-everything. |
| Mini1 feedback: cache | LRU result cache at node A. Key = hash of all QuerySpec fields. Hit = skip 9-node fan-out entirely. |

---

## What the Numbers Show (from `docs/measurements.json`)

These were measured on one machine (all-Python cluster, 50,000 synthetic rows):

| Scenario | Rows | Wall time | Throughput | Per-fetch latency |
|---|---|---|---|---|
| Fast client (baseline) | 50,000 | **81 ms** | 614,000 rows/sec | 3.8 ms |
| Slow client (100ms sleep) | 50,000 | **10,252 ms** | 4,877 rows/sec | 3.9 ms |
| Brooklyn filter | 10,050 | 19 ms | 525,000 rows/sec | 1.7 ms |
| Date range 2024 | 24,944 | 45 ms | 559,000 rows/sec | 2.9 ms |
| Geo box query | 9,332 | 17 ms | 540,000 rows/sec | 1.6 ms |

**The interesting finding**: per-fetch compute is identical (3.8ms fast vs 3.9ms slow), but total wall time differs by 126×. The bottleneck is entirely the consumer, not the server. Our adaptive chunk sizing proves this — chunk size shrinks from ~2,345 rows (fast) to ~510 rows (slow) automatically, with zero manual tuning.

Cache numbers (re-run borough query):
- First run: ~19ms (full 9-node fan-out)
- Second run: ~4ms (cache hit, no fan-out)

---

## Before Meeting: One-Time Setup (Each Laptop)

```bash
# 1. Install Python deps
cd mini2-AAA
python3 -m pip install -r python/requirements.txt

# 2. Build C++ (needs cmake + grpc installed)
scripts/build_cpp.sh

# 3. Generate Python proto stubs
scripts/gen_proto.sh
```

If grpc is not installed:
- macOS: `brew install grpc`
- Ubuntu: `sudo apt install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc`

---

## Getting Fresh Results Before Class

### Step 1: Generate data

```bash
# Synthetic (quick, works without real dataset)
python3 scripts/generate_synthetic.py --rows 50000 --outdir data \
    --nodes A,B,C,D,E,F,G,H,I

# OR real NYC 311 data (if you have the CSV)
python3 scripts/partition_data.py \
    --input data/raw/311_Service_Requests_from_2010_to_Present.csv \
    --outdir data --nodes A,B,C,D,E,F,G,H,I
```

### Step 2: Start all 9 nodes (one command)

```bash
scripts/run_all_local.sh
# Wait until all 9 print "listening on ..."
```

### Step 3: Run the measurement script

```bash
python3 scripts/measure.py --reps 3 --slow-ms 100 --out docs/measurements.json
```

This overwrites `docs/measurements.json` with fresh numbers. Takes about 2–3 minutes.

### Step 4: Tear down

```bash
scripts/kill_all.sh
```

---

## What to Present in Class (Step-by-Step)

### Slide: "The Bottleneck is the Consumer, Not the Computation"

Show the table above. Say:

> "We ran the same 50,000-row query with a fast client and a slow client. Per-fetch compute was identical — 3.8ms vs 3.9ms. But total wall time differed by 126×. The server is not the bottleneck. When the client slows down, our chunk buffer fills up and producers throttle automatically. No manual tuning needed — the EWMA feedback loop does it."

Then show the cache line:

> "For repeated queries — same borough, same date range — the second call takes 4ms instead of 19ms. The cache skips the entire 9-node fan-out."

### Demo order (10 minutes)

**1. Start all nodes** — each terminal shows `loaded N rows` and `listening on ...`

**2. Run a borough query** — show results printing chunk by chunk:
```bash
scripts/run_client.sh --borough BROOKLYN --max 500
```

**3. Run the same query again** — point to `cache hit` in node A's log:
```bash
scripts/run_client.sh --borough BROOKLYN --max 500
```

**4. Run with `--slow`** — show chunk size shrinking in the output:
```bash
scripts/run_client.sh --borough BROOKLYN --slow --max 500
```

**5. GetStats** — show the counters:
```bash
scripts/run_client.sh --stats
```
Should show `cache_hits`, `cache_misses`, `total_rows_served`.

**6. Kill a node, re-run** — show partial results returned (fault tolerance):
```bash
# In a separate terminal, kill node F
kill $(lsof -ti:50056)
# Run query again — still returns results from the other nodes
scripts/run_client.sh --borough BROOKLYN --max 500
```

---

## Who Does What During Demo

| Person | Role |
|---|---|
| **Anukrithi** | Runs node A + drives client queries from terminal; explains caching |
| **Ali** | Runs nodes B, C, D in terminal tabs; explains data partitioning and backpressure |
| **Asim** | Runs nodes E, F, G, H, I; explains overlay fan-out and cross-language Python nodes |

---

## What to Say for Each Feature

**Overlay (tree fan-out)**:
> "Node A gets the query. It forwards to its neighbors — B, H, G, I — which forward further down. The visited set in each message prevents the B-D-E triangle from being visited twice."

**Backpressure**:
> "Node A's chunk buffer holds at most 65,536 rows. When it fills up, PushChunk returns a short-count and producers sleep before retrying. The slow-client demo shows this in action."

**Adaptive chunk sizing**:
> "Each FetchChunk response includes `suggested_next_max`. Node A tracks inter-fetch intervals with an exponential moving average. Fast client: chunk grows by 25% each time. Slow client: chunk shrinks by 25%. The 126× wall time difference in our measurements is entirely from this mechanism."

**Cache**:
> "When FetchChunk drains the last chunk, it stores the full result in an LRU cache keyed on the query spec hash. Second identical query: instant return, no fan-out."

**Cross-language**:
> "G and I are Python nodes. They speak the same proto, same topology config. You can mix C++ and Python nodes freely."
