"""mini2 Python node server.

Implements the same Portal + Overlay services as the C++ server. Used by
the cross-language nodes (G and I in the default topology), but can run any
node id from the config. Speaks the same proto, so a Python node can be a
peer of a C++ node and vice versa.

Run:
    python -m grpc_tools.protoc -I proto --python_out python/_gen \\
        --grpc_python_out python/_gen proto/overlay.proto
    python python/node_server.py --id G --config config/topology.conf
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import threading
import time
import uuid
from concurrent import futures
from typing import Dict, List, Optional

# Make the generated protobuf module discoverable.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "_gen"))

import grpc

import overlay_pb2 as pb
import overlay_pb2_grpc as pbg

from config_loader import Config, NodeEntry, load as load_config
from partition_store import PartitionStore, borough_to_str

LOG = logging.getLogger("mini2.node")

_SELF_ROUTE = "__self__"


class _BoundedBuffer:
    """Bounded queue used by the portal node only."""

    def __init__(self, capacity: int, expected_producers: int):
        self._cap = capacity
        self._expected = expected_producers
        self._cv = threading.Condition()
        self._q: List[tuple] = []
        self._started: set[str] = set()
        self._done: set[str] = set()
        self._canceled = False
        self._rows_served = 0

    def push(self, rows: List[tuple]) -> int:
        with self._cv:
            if self._canceled:
                return len(rows)
            avail = max(0, self._cap - len(self._q))
            take = min(len(rows), avail)
            if take:
                self._q.extend(rows[:take])
                self._cv.notify_all()
            return take

    def pop_up_to(self, n: int, block_ms: int) -> List[tuple]:
        if n <= 0:
            return []
        with self._cv:
            if not self._q and not self._canceled and len(self._done) < self._expected and block_ms > 0:
                self._cv.wait(timeout=block_ms / 1000.0)
            out = self._q[:n]
            del self._q[:n]
            self._rows_served += len(out)
            if out:
                self._cv.notify_all()
            return out

    def mark_started(self, producer: str) -> None:
        with self._cv:
            self._started.add(producer)

    def mark_done(self, producer: str) -> None:
        with self._cv:
            self._done.add(producer)
            self._cv.notify_all()

    def cancel(self) -> None:
        with self._cv:
            self._canceled = True
            self._q.clear()
            self._cv.notify_all()

    @property
    def canceled(self) -> bool:
        with self._cv:
            return self._canceled

    @property
    def is_final(self) -> bool:
        with self._cv:
            if self._canceled:
                return True
            return not self._q and len(self._done) >= self._expected

    @property
    def buffered(self) -> int:
        with self._cv:
            return len(self._q)

    @property
    def pending_producers(self) -> int:
        with self._cv:
            if len(self._done) >= self._expected:
                return 0
            return self._expected - len(self._done)

    @property
    def rows_served(self) -> int:
        with self._cv:
            return self._rows_served


class _RequestState:
    def __init__(self, request_id: str, client_id: str, spec, buf: _BoundedBuffer,
                 init_chunk: int, min_chunk: int, max_chunk: int):
        self.request_id = request_id
        self.client_id = client_id
        self.spec = spec
        self.buffer = buf
        self.suggested_chunk = init_chunk
        self.min_chunk = min_chunk
        self.max_chunk = max_chunk
        self.created_at = time.monotonic()
        self.last_fetch_at = self.created_at
        self.ewma_fetch_ms = 100.0

    def record_fetch(self) -> None:
        now = time.monotonic()
        dt_ms = max(0.1, (now - self.last_fetch_at) * 1000.0)
        self.last_fetch_at = now
        self.ewma_fetch_ms = 0.7 * self.ewma_fetch_ms + 0.3 * dt_ms
        if self.ewma_fetch_ms < 20.0:
            self.suggested_chunk = min(self.max_chunk, self.suggested_chunk + self.suggested_chunk // 4 + 1)
        elif self.ewma_fetch_ms > 500.0:
            self.suggested_chunk = max(self.min_chunk, self.suggested_chunk - self.suggested_chunk // 4 - 1)


class _Route:
    __slots__ = ("upstream", "visited", "fanout_to", "spec", "canceled", "created_at")

    def __init__(self, upstream: str, visited: List[str], spec):
        self.upstream = upstream
        self.visited = list(visited)
        self.fanout_to: Dict[str, str] = {}
        self.spec = spec
        self.canceled = False
        self.created_at = time.monotonic()


class NodeServer(pbg.PortalServicer, pbg.OverlayServicer):
    def __init__(self, my_id: str, cfg: Config):
        self.my_id = my_id
        self.cfg = cfg
        self.entry: NodeEntry = cfg.nodes[my_id]
        self.store = PartitionStore.empty()

        path = cfg.partition_path(my_id)
        if path:
            LOG.info("[%s] loading partition: %s", my_id, path)
            self.store.load(path)
            LOG.info("[%s] loaded %d rows", my_id, len(self.store))

        # Peer stubs (overlay neighbors)
        self.peer_stubs: Dict[str, pbg.OverlayStub] = {}
        self.peer_addrs: Dict[str, str] = {}
        for nb in cfg.neighbors(my_id):
            ne = cfg.nodes[nb]
            addr = f"{ne.host}:{ne.port}"
            chan = grpc.insecure_channel(addr, options=[
                ("grpc.max_send_message_length", 64 * 1024 * 1024),
                ("grpc.max_receive_message_length", 64 * 1024 * 1024),
            ])
            self.peer_stubs[nb] = pbg.OverlayStub(chan)
            self.peer_addrs[nb] = addr
            LOG.info("[%s] peer %s @ %s", my_id, nb, addr)

        # Portal-only state
        self._reqs: Dict[str, _RequestState] = {}
        self._reqs_lock = threading.Lock()

        # Per-request route bookkeeping (non-portal nodes need this to find upstream)
        self._routes: Dict[str, _Route] = {}
        self._routes_lock = threading.Lock()

        self._req_counter = 0
        self._stop = threading.Event()
        self._start_time = time.monotonic()
        self._total_rows_served = 0

        self._exec_pool = futures.ThreadPoolExecutor(max_workers=8)

    # ----- helpers -----

    def _new_request_id(self) -> str:
        self._req_counter += 1
        return f"{self.my_id}-{self._req_counter}-{uuid.uuid4().hex[:8]}"

    def _get_route(self, rid: str) -> Optional[_Route]:
        with self._routes_lock:
            return self._routes.get(rid)

    def _set_route(self, rid: str, route: _Route) -> None:
        with self._routes_lock:
            self._routes[rid] = route

    def _is_canceled(self, rid: str) -> bool:
        r = self._get_route(rid)
        return bool(r and r.canceled)

    # ----- Portal service -----

    def SubmitQuery(self, request, context):
        if not self.entry.is_root:
            return pb.SubmitAck(accepted=False, error="this node is not the portal")

        rid = self._new_request_id()
        all_ids = self.cfg.all_node_ids()
        buf = _BoundedBuffer(
            self.cfg.param_int("chunk_buffer_cap", 65536),
            len(all_ids),
        )
        for nid in all_ids:
            buf.mark_started(nid)

        st = _RequestState(
            rid, request.client_id, request.spec, buf,
            self.cfg.param_int("initial_chunk_rows", 512),
            self.cfg.param_int("min_chunk_rows", 64),
            self.cfg.param_int("max_chunk_rows", 4096),
        )
        with self._reqs_lock:
            self._reqs[rid] = st

        route = _Route(_SELF_ROUTE, [self.my_id], request.spec)
        self._set_route(rid, route)

        # Fan out to neighbors
        for nb, stub in self.peer_stubs.items():
            f = pb.ForwardQueryRequest(
                request_id=rid,
                spec=request.spec,
                origin_node=self.my_id,
                from_node=self.my_id,
                visited=route.visited,
                ttl=16,
                issued_at_unix_us=int(time.time() * 1_000_000),
            )
            try:
                ack = stub.ForwardQuery(f, timeout=5.0)
                if ack.accepted:
                    route.fanout_to[nb] = nb
                else:
                    buf.mark_done(nb)
            except grpc.RpcError as e:
                LOG.warning("[%s] forward to %s failed: %s", self.my_id, nb, e)
                buf.mark_done(nb)

        # Local execution in a worker thread
        self._exec_pool.submit(self._execute_local, rid, request.spec, _SELF_ROUTE)

        return pb.SubmitAck(accepted=True, request_id=rid)

    def FetchChunk(self, request, context):
        with self._reqs_lock:
            st = self._reqs.get(request.request_id)
        if st is None:
            return pb.ChunkResponse(request_id=request.request_id, final=True, canceled=True)

        st.record_fetch()
        want = request.max_rows or st.suggested_chunk
        want = min(want, st.max_chunk)
        block_ms = self.cfg.param_int("fetch_block_ms", 150) if request.block else 0
        rows = st.buffer.pop_up_to(want, block_ms)

        rsp = pb.ChunkResponse(
            request_id=request.request_id,
            suggested_next_max=st.suggested_chunk,
            rows_so_far=st.buffer.rows_served,
            pending_producers=st.buffer.pending_producers,
            canceled=st.buffer.canceled,
            final=st.buffer.is_final,
        )
        for r in rows:
            rsp.rows.add(
                unique_key=r[0],
                created_ymd=r[1],
                borough=r[2],
                latitude=r[3],
                longitude=r[4],
                complaint_type=r[5],
            )
        self._total_rows_served += len(rows)
        if rsp.final:
            with self._reqs_lock:
                self._reqs.pop(request.request_id, None)
        return rsp

    def CancelQuery(self, request, context):
        with self._reqs_lock:
            st = self._reqs.get(request.request_id)
        if st:
            st.buffer.cancel()
        r = self._get_route(request.request_id)
        if r:
            r.canceled = True
        for nb, stub in self.peer_stubs.items():
            try:
                stub.Abort(pb.AbortRequest(
                    request_id=request.request_id, from_node=self.my_id, reason=request.reason),
                    timeout=2.0)
            except grpc.RpcError:
                pass
        return pb.CancelAck(accepted=True)

    def GetStats(self, request, context):
        with self._reqs_lock:
            active = len(self._reqs)
            buffered = sum(s.buffer.buffered for s in self._reqs.values())
        return pb.StatsResponse(
            node_id=self.my_id,
            active_requests=active,
            buffered_rows=buffered,
            total_rows_served=self._total_rows_served,
            peer_count=len(self.peer_stubs),
            partition_rows=len(self.store),
        )

    # ----- Overlay service -----

    def ForwardQuery(self, request, context):
        if self.my_id in request.visited:
            return pb.ForwardQueryAck(accepted=False, node_id=self.my_id, error="loop")

        # Dedupe by request_id: once we've accepted a forward for a given
        # request, every subsequent forward (e.g. arriving via a different
        # path in a cyclic overlay) is a no-op. Without this check the same
        # leaf can produce its rows multiple times.
        with self._routes_lock:
            if request.request_id in self._routes:
                return pb.ForwardQueryAck(accepted=False, node_id=self.my_id, error="dup")

        visited = list(request.visited) + [self.my_id]
        route = _Route(request.from_node, visited, request.spec)
        self._set_route(request.request_id, route)

        for nb, stub in self.peer_stubs.items():
            if nb == request.from_node or nb in visited:
                continue
            f = pb.ForwardQueryRequest(
                request_id=request.request_id,
                spec=request.spec,
                origin_node=request.origin_node,
                from_node=self.my_id,
                visited=visited,
                ttl=max(0, request.ttl - 1),
                issued_at_unix_us=request.issued_at_unix_us,
            )
            try:
                ack = stub.ForwardQuery(f, timeout=5.0)
                if ack.accepted:
                    route.fanout_to[nb] = nb
            except grpc.RpcError as e:
                LOG.warning("[%s] forward to %s failed: %s", self.my_id, nb, e)

        self._exec_pool.submit(self._execute_local, request.request_id, request.spec, request.from_node)
        return pb.ForwardQueryAck(accepted=True, node_id=self.my_id)

    def PushChunk(self, request, context):
        # Portal: deposit into local buffer.
        if self.entry.is_root:
            with self._reqs_lock:
                st = self._reqs.get(request.request_id)
            if st is None:
                return pb.PushChunkAck(ok=False, canceled=True)
            if st.buffer.canceled:
                return pb.PushChunkAck(ok=False, canceled=True)
            rows_native = [
                (r.unique_key, r.created_ymd, int(r.borough), r.latitude, r.longitude, r.complaint_type)
                for r in request.rows
            ]
            took = st.buffer.push(rows_native)
            if request.done:
                st.buffer.mark_done(request.producer_node)
            ack = pb.PushChunkAck(ok=True, buffered_rows=st.buffer.buffered)
            if took < len(rows_native):
                ack.suggested_delay_ms = self.cfg.param_int("push_backoff_ms", 25)
            return ack

        # Internal node: forward upstream.
        route = self._get_route(request.request_id)
        if route is None:
            return pb.PushChunkAck(ok=False, canceled=True)
        upstream_stub = self.peer_stubs.get(route.upstream)
        if upstream_stub is None:
            return pb.PushChunkAck(ok=False)
        f = pb.PushChunkRequest()
        f.CopyFrom(request)
        f.from_node = self.my_id
        try:
            up_ack = upstream_stub.PushChunk(f, timeout=10.0)
            return pb.PushChunkAck(
                ok=up_ack.ok,
                buffered_rows=up_ack.buffered_rows,
                canceled=up_ack.canceled,
                suggested_delay_ms=up_ack.suggested_delay_ms,
            )
        except grpc.RpcError as e:
            LOG.warning("[%s] upstream push to %s failed: %s", self.my_id, route.upstream, e)
            return pb.PushChunkAck(ok=False)

    def Abort(self, request, context):
        r = self._get_route(request.request_id)
        if r:
            r.canceled = True
        for nb, stub in self.peer_stubs.items():
            if nb == request.from_node:
                continue
            f = pb.AbortRequest(
                request_id=request.request_id,
                from_node=self.my_id,
                reason=request.reason,
            )
            try:
                stub.Abort(f, timeout=2.0)
            except grpc.RpcError:
                pass
        return pb.AbortAck(ok=True)

    def Ping(self, request, context):
        return pb.PingResponse(
            node_id=self.my_id,
            unix_us=int(time.time() * 1_000_000),
            uptime_s=int(time.monotonic() - self._start_time),
            partition_rows=len(self.store),
        )

    # ----- internals -----

    def _execute_local(self, rid: str, spec, upstream_peer: str) -> None:
        chunk_rows = self.cfg.param_int("initial_chunk_rows", 512)
        seq = 0
        for rows, done in self.store.scan(spec, chunk_rows, lambda: self._is_canceled(rid)):
            seq += 1
            if upstream_peer == _SELF_ROUTE:
                with self._reqs_lock:
                    st = self._reqs.get(rid)
                if not st:
                    return
                st.buffer.push([
                    (r[0], r[1], r[2], r[3], r[4], r[5]) for r in rows
                ])
                if done:
                    st.buffer.mark_done(self.my_id)
                continue

            stub = self.peer_stubs.get(upstream_peer)
            if not stub:
                return
            req = pb.PushChunkRequest(
                request_id=rid,
                from_node=self.my_id,
                producer_node=self.my_id,
                done=done,
                seq=seq,
                produced_at_us=int(time.time() * 1_000_000),
            )
            for r in rows:
                req.rows.add(
                    unique_key=r[0],
                    created_ymd=r[1],
                    borough=r[2],
                    latitude=r[3],
                    longitude=r[4],
                    complaint_type=r[5],
                )
            for _ in range(5):
                try:
                    ack = stub.PushChunk(req, timeout=10.0)
                    if ack.ok:
                        break
                    if ack.canceled:
                        return
                    time.sleep(max(0.001, ack.suggested_delay_ms / 1000.0))
                except grpc.RpcError as e:
                    LOG.warning("[%s] push to %s failed: %s", self.my_id, upstream_peer, e)
                    time.sleep(0.05)


def _serve(my_id: str, cfg_path: str) -> None:
    logging.basicConfig(
        level=os.environ.get("MINI2_LOG", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    cfg = load_config(cfg_path)
    if my_id not in cfg.nodes:
        raise SystemExit(f"node {my_id} not in config")

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16), options=[
        ("grpc.max_send_message_length", 64 * 1024 * 1024),
        ("grpc.max_receive_message_length", 64 * 1024 * 1024),
    ])
    node = NodeServer(my_id, cfg)
    pbg.add_PortalServicer_to_server(node, server)
    pbg.add_OverlayServicer_to_server(node, server)

    addr = f"{cfg.nodes[my_id].host}:{cfg.nodes[my_id].port}"
    server.add_insecure_port(addr)
    server.start()
    LOG.info("[%s] python node listening on %s root=%s",
             my_id, addr, cfg.nodes[my_id].is_root)

    stop_evt = threading.Event()
    def _on_sig(_signo, _frame):
        LOG.info("[%s] shutdown signal", my_id)
        stop_evt.set()
    signal.signal(signal.SIGINT, _on_sig)
    signal.signal(signal.SIGTERM, _on_sig)
    stop_evt.wait()
    server.stop(grace=1.0)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--id", required=True)
    p.add_argument("--config", required=True)
    args = p.parse_args()
    _serve(args.id, args.config)


if __name__ == "__main__":
    main()
