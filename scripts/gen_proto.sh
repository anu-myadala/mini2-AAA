#!/usr/bin/env bash
# Regenerate Python protobuf stubs into python/_gen.
# (C++ stubs are produced by CMake during the C++ build.)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/python/_gen"
mkdir -p "$OUT"
touch "$OUT/__init__.py"

python3 -m grpc_tools.protoc \
    -I "$ROOT/proto" \
    --python_out="$OUT" \
    --grpc_python_out="$OUT" \
    "$ROOT/proto/overlay.proto"

# grpc_tools emits "import overlay_pb2 as overlay__pb2" which only resolves
# when _gen is on sys.path. We add a tiny shim so node_server.py works
# whether or not the user has installed it as a package.
echo "[gen_proto] wrote stubs to $OUT"
