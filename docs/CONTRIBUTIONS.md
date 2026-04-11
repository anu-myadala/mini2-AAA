# Team Contributions — Mini 2

**Team members:** Anukrithi Myadala, Ali Ucer, Asim Mohammed

---

## Anukrithi Myadala

- Designed and implemented the tree overlay topology and `Config` parser (`cpp/src/Config.cpp`, `cpp/include/Config.hpp`)
- Built `NodeServer` Portal service: `SubmitQuery`, `FetchChunk`, `CancelQuery` handlers (`cpp/src/NodeServer.cpp`)
- Implemented `QueryCache` LRU result cache with hash-combine keying and multimap index (`cpp/include/QueryCache.hpp`, `cpp/src/QueryCache.cpp`)
- Integrated cache into FetchChunk shadow-accumulation and SubmitQuery cache-hit path
- Wrote `scripts/partition_data.py` and validated NYC 311 date parsing (MM/DD/YYYY format)
- Ran end-to-end benchmarks for cache hit vs. miss latency

---

## Ali Ucer

- Designed and implemented the `PartitionStore` SoA columnar layout with file-size-based `reserve()` pre-allocation (`cpp/src/PartitionStore.cpp`, `cpp/include/PartitionStore.hpp`)
- Wrote `QueryEngine` column-scan predicate evaluation with cancel polling (`cpp/src/QueryEngine.cpp`)
- Implemented `ChunkBuffer` bounded producer/consumer queue with backpressure and condition variables (`cpp/src/ChunkBuffer.cpp`)
- Built `RequestRegistry` with EWMA adaptive chunk sizing (`cpp/src/RequestRegistry.cpp`)
- Wrote Python `PartitionStore` using `array.array` typed storage (`python/partition_store.py`)
- Designed and ran throughput measurements across borough/date/geo filter combinations

---

## Asim Mohammed

- Authored `proto/overlay.proto` (Portal + Overlay services, typed `Row311`, `QuerySpec`)
- Implemented `PeerClient` gRPC stub wrapper with deadline management (`cpp/src/PeerClient.cpp`)
- Wrote Python `NodeServer` implementing both Portal and Overlay servicers (`python/node_server.py`)
- Built `ForwardQuery` fan-out with loop avoidance via visited-set and request dedup (`cpp/src/NodeServer.cpp` Overlay section)
- Wrote build and deployment scripts: `scripts/build_cpp.sh`, `scripts/run_node.sh`, `scripts/run_all_local.sh`, `scripts/gen_proto.sh`
- Set up two-host configuration and ran multi-machine tests
- Benchmarked adaptive chunk sizing and documented backpressure behavior

---

## Joint

- Overlay topology design (tree vs. grid trade-off analysis)
- Report writing and benchmark aggregation
- Presentation slide
