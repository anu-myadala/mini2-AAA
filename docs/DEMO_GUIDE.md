# Demo Guide — Mini 2 In-Class Presentation

**Setup:** Two laptops (or one for dev runs). A on host1, F/G/H/I on host2.

---

## Prerequisites (run once before class)

```bash
# 1. Build C++ (both laptops if using two)
cd mini2-claude/cpp && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)

# 2. Generate Python proto stubs
bash scripts/gen_proto.sh

# 3. Partition the 311 dataset
python3 scripts/partition_data.py \
    --input data/raw/311_Service_Requests_from_2010_to_Present.csv \
    --outdir data \
    --nodes A,B,C,D,E,F,G,H,I
```

---

## Single-machine run (dev / demo fallback)

Open **9 terminals**, one per node. Run each in its own shell tab.

**Terminals 1–7 (C++ nodes — A,B,C,D,E,F,H):**
```bash
# Replace X with the node id (A, B, C, D, E, F, or H)
./scripts/run_node.sh X config/topology.conf
```

**Terminal 8 (Python node G):**
```bash
./scripts/run_node.sh G config/topology.conf
```

**Terminal 9 (Python node I):**
```bash
./scripts/run_node.sh I config/topology.conf
```

Wait for all nodes to print `listening on ...`.

---

## Run queries from a 10th terminal

```bash
# Basic scan — all rows, no filter
./scripts/run_client.sh

# Borough filter
./scripts/run_client.sh --borough BROOKLYN --max 500

# Date range
./scripts/run_client.sh --from 20230101 --to 20231231 --max 1000

# Complaint type
./scripts/run_client.sh --complaint "Noise - Residential" --max 200

# Geo box (Manhattan roughly)
./scripts/run_client.sh --geo 40.70,40.85,-74.02,-73.93 --max 300

# Slow client — shows adaptive chunk sizing / backpressure
./scripts/run_client.sh --slow --max 500

# Run the SAME query twice — second call should hit the cache
./scripts/run_client.sh --borough QUEENS --max 300
./scripts/run_client.sh --borough QUEENS --max 300
```

For the cache demo, check node A's stderr — second call prints `cache hit`.

---

## Two-host run

**host1** runs nodes A, B, C, D, E.  
**host2** runs nodes F, G, H, I.

Edit `config/topology.conf`: change `host=127.0.0.1` to the actual IP for each node group. Then run as above, each set of nodes on the right machine.

---

## Teammate assignments during demo

| Person | Role during demo |
|---|---|
| **Anukrithi** | Runs node A + drives the client queries; explains caching |
| **Ali** | Runs nodes B, C, D on terminal tabs; explains data partitioning and backpressure |
| **Asim** | Runs nodes E, F, G, H, I; explains overlay fan-out and cross-language (Python nodes) |

---

## Stopping all nodes

```bash
bash scripts/kill_all.sh
```

---

## What to show / say

1. **Start all 9 nodes** — show each printing `loaded N rows` and `listening on ...`.
2. **Run a borough query** — show results streaming chunk by chunk, mention fan-out reaches all 9 nodes.
3. **Run with `--slow`** — show chunk size shrinking (adaptive backpressure).
4. **Run same query twice** — show cache hit on second run (nearly instant response, `cache hit` in log).
5. **`GetStats` call** — show `cache_hits`, `cache_misses`, `total_rows_served`.
6. **Kill node F, run query** — show partial results returned (subtree failure tolerance).
