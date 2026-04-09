#!/usr/bin/env bash
# Build the C++ node_server and portal_client.
# Requires: cmake >= 3.16, gRPC + protobuf installed (e.g., `brew install grpc`
# on macOS or distro packages on Linux).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/cpp/build"
mkdir -p "$BUILD"
cd "$BUILD"
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
echo "[build_cpp] binaries:"
echo "  $BUILD/node_server"
echo "  $BUILD/portal_client"
