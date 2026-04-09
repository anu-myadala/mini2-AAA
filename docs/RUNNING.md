# Running mini2

## Prereqs

* Linux or macOS, bash shell. **Do not run any of this from inside an
  IDE-managed VM.** Open a real terminal.
* `python3` 3.10+
* `cmake` ≥ 3.16
* gRPC (with the C++ libs) and protobuf
  * macOS: `brew install grpc`
  * Ubuntu/Debian: `sudo apt install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc`
  * From source: see https://grpc.io/docs/languages/cpp/quickstart/

## One-time setup

```bash
cd mini2-claude
python3 -m pip install -r python/requirements.txt
scripts/gen_proto.sh        # produces python/_gen/{overlay_pb2,overlay_pb2_grpc}.py
scripts/build_cpp.sh        # produces cpp/build/{node_server,portal_client}
```

## Smoke test (single host)

```bash
python3 scripts/generate_synthetic.py --rows 50000 --outdir data \
        --nodes A,B,C,D,E,F,G,H,I
scripts/run_all_local.sh
sleep 1
scripts/run_client.sh --borough BROOKLYN --max 200
scripts/kill_all.sh
```

Each node logs to `logs/<id>.log`. The portal log shows `forwarded`,
`pushed`, and `served` counts.

## Two-host run

Pick a primary host (we'll call it `host1`) for nodes A, B, C, D, E and
a secondary host (`host2`) for nodes F, G, H, I. Edit
`config/topology.conf`:

```
node A host=host1.local port=50051 lang=cpp root=true
node B host=host1.local port=50052 lang=cpp
node C host=host1.local port=50053 lang=cpp
node D host=host1.local port=50054 lang=cpp
node E host=host1.local port=50055 lang=cpp
node F host=host2.local port=50056 lang=cpp
node G host=host2.local port=50057 lang=python
node H host=host2.local port=50058 lang=cpp
node I host=host2.local port=50059 lang=python
```

Make sure both hosts can reach each other on the listed ports
(`telnet host1.local 50051` etc.). Then on each host:

```bash
# host1
scripts/run_node.sh A &  scripts/run_node.sh B &  scripts/run_node.sh C &
scripts/run_node.sh D &  scripts/run_node.sh E &

# host2
scripts/run_node.sh F &  scripts/run_node.sh G &
scripts/run_node.sh H &  scripts/run_node.sh I &
```

The client only needs to know A:

```bash
scripts/run_client.sh --borough BROOKLYN --max 1000
```

## Three-host run

Same recipe, different `host=` assignments. Suggested split: A on host1,
B/C/D/E on host2, F/G/H/I on host3.

## Real NYC 311 data

Drop the raw OpenData CSV files into `data/raw/` (these are the same
files used in mini1) and run:

```bash
python3 scripts/partition_data.py \
    --input data/raw/311_2024.csv data/raw/311_2023.csv \
    --outdir data --nodes A,B,C,D,E,F,G,H,I
```

## Testing the system manually

Some useful client invocations:

```bash
# count rows in Manhattan in 2024
scripts/run_client.sh --borough MANHATTAN --from 20240101 --to 20241231 --max 0 | wc -l

# pull a geo box (lower Manhattan)
scripts/run_client.sh --geo 40.700,40.730,-74.020,-73.985

# demo backpressure: client sleeps between fetches, watch portal logs throttle producers
scripts/run_client.sh --borough QUEENS --slow

# cancel demo: hit Ctrl-C while a query is in flight
scripts/run_client.sh --max 1000000
^C
```

## Verifying basecamp

To prove every node is up and reachable from its peers, hit the
`Overlay.Ping` RPC. A tiny ping helper isn't shipped — instead, the
`portal_client --client-id ping` invocation submits an empty query
(matches everything, capped at 0 rows) which forces the full fan-out
and surfaces any forward failures in `logs/A.log`.
