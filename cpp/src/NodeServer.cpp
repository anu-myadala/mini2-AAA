// NodeServer.cpp
//
// One node, two services. Most of the interesting work is in:
//   * ForwardQuery: fan out to neighbors that haven't seen this request, then
//     spawn a worker thread to scan the local partition.
//   * executeLocal: invokes QueryEngine and pushes each chunk back along the
//     "upstream" link (toward the portal). The portal is upstream_peer ==
//     "__self__"; in that case the chunk goes straight into the local
//     RequestRegistry buffer instead of over gRPC.
//   * PushChunk: if we are the portal, deposit into the buffer; otherwise
//     forward up.
//   * Janitor: GC abandoned requests every janitor_interval_ms.
//
// Concurrency: gRPC sync API gives us one worker thread per active call,
// which is fine here because all our blocking work is bounded.

#include "NodeServer.hpp"
#include "QueryEngine.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>

namespace mini2 {

namespace {
constexpr const char* kSelfRoute = "__self__";

std::string randomHex(std::size_t n) {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(hex[rng() & 0xF]);
    return out;
}
} // namespace

NodeServer::NodeServer(std::string my_id, Config cfg)
  : my_id_(std::move(my_id)), cfg_(std::move(cfg)),
    cache_(static_cast<std::size_t>(cfg_.paramInt("cache_capacity",  256)),
           static_cast<std::size_t>(cfg_.paramInt("cache_max_rows", 50000))) {}

NodeServer::~NodeServer() { shutdown(); if (janitor_.joinable()) janitor_.join(); }

std::string NodeServer::newRequestId() {
    std::ostringstream os;
    os << my_id_ << "-" << ++req_counter_ << "-" << randomHex(8);
    return os.str();
}

PeerClient* NodeServer::peer(const std::string& id) {
    auto it = peer_clients_.find(id);
    return (it == peer_clients_.end()) ? nullptr : it->second.get();
}

void NodeServer::run() {
    start_time_ = std::chrono::steady_clock::now();

    // 1) Load this node's data partition.
    std::string part = cfg_.partitionPath(my_id_);
    if (!part.empty()) {
        std::cerr << "[" << my_id_ << "] loading partition: " << part << std::endl;
        store_.load(part);
        std::cerr << "[" << my_id_ << "] loaded " << store_.size() << " rows" << std::endl;
    } else {
        std::cerr << "[" << my_id_ << "] no partition path; pure forwarder" << std::endl;
    }

    // 2) Build a stub for every neighbor.
    for (const auto& nb : cfg_.neighborsOf(my_id_)) {
        peer_clients_[nb] = std::make_unique<PeerClient>(cfg_.node(nb));
        std::cerr << "[" << my_id_ << "] peer " << nb << " @ "
                  << cfg_.node(nb).host << ":" << cfg_.node(nb).port << std::endl;
    }

    // 3) Start the gRPC server, hosting both services on the same port.
    const auto& me = cfg_.node(my_id_);
    std::string addr = me.host + ":" + std::to_string(me.port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);
    builder.SetMaxSendMessageSize  (64 * 1024 * 1024);
    builder.RegisterService(static_cast<Portal::Service*>(this));
    builder.RegisterService(static_cast<Overlay::Service*>(this));
    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("[" + my_id_ + "] failed to bind " + addr);
    }
    std::cerr << "[" << my_id_ << "] listening on " << addr
              << "  root=" << (me.is_root ? "yes" : "no") << std::endl;

    // 4) Janitor thread for abandoned requests.
    janitor_ = std::thread(&NodeServer::janitorLoop, this);

    server_->Wait();
}

void NodeServer::shutdown() {
    if (stop_.exchange(true)) return;
    if (server_) server_->Shutdown(std::chrono::system_clock::now() +
                                   std::chrono::milliseconds(200));
}

void NodeServer::janitorLoop() {
    auto interval = std::chrono::milliseconds(cfg_.paramInt("janitor_interval_ms", 500));
    auto ttl      = std::chrono::seconds(cfg_.paramInt("request_ttl_secs", 120));
    while (!stop_.load()) {
        std::this_thread::sleep_for(interval);
        auto expired = registry_.collectExpired(ttl);
        for (auto& st : expired) {
            std::cerr << "[" << my_id_ << "] janitor: abandoning request "
                      << st->request_id << std::endl;
            st->buffer->cancel();
            // Best-effort downstream abort
            for (auto& kv : peer_clients_) {
                AbortRequest req; req.set_request_id(st->request_id);
                req.set_from_node(my_id_); req.set_reason("ttl");
                AbortAck ack; (void)kv.second->abort(req, &ack);
            }
            registry_.erase(st->request_id);
        }
    }
}

// ---------- Portal service ----------

grpc::Status NodeServer::SubmitQuery(grpc::ServerContext*,
                                     const SubmitRequest* req, SubmitAck* ack) {
    if (!cfg_.node(my_id_).is_root) {
        ack->set_accepted(false);
        ack->set_error("this node is not the portal");
        return grpc::Status::OK;
    }

    // Cache check: serve repeated queries from the LRU cache without re-scanning.
    auto cached_rows = cache_.get(req->spec());
    if (cached_rows) {
        std::string rid = newRequestId();
        auto st = registry_.create(
            rid, req->client_id(), req->spec(),
            static_cast<std::size_t>(cfg_.paramInt("chunk_buffer_cap", 65536)),
            1,   // one virtual producer: the cache
            cfg_.paramInt("initial_chunk_rows", 512),
            cfg_.paramInt("min_chunk_rows", 64),
            cfg_.paramInt("max_chunk_rows", 4096));
        st->cache_collect = false;  // already in cache, don't re-store
        st->buffer->markProducerStarted("__cache__");
        std::vector<Row311> prefill(*cached_rows);
        st->buffer->pushRows(std::move(prefill));
        st->buffer->markProducerDone("__cache__");
        ack->set_accepted(true);
        ack->set_request_id(rid);
        ack->set_cached(true);
        std::cerr << "[" << my_id_ << "] cache hit rid=" << rid
                  << " rows=" << cached_rows->size() << "\n";
        return grpc::Status::OK;
    }

    std::string rid = newRequestId();
    std::size_t expected = cfg_.allNodeIds().size();
    auto st = registry_.create(
        rid, req->client_id(), req->spec(),
        static_cast<std::size_t>(cfg_.paramInt("chunk_buffer_cap", 65536)),
        expected,
        cfg_.paramInt("initial_chunk_rows", 512),
        cfg_.paramInt("min_chunk_rows", 64),
        cfg_.paramInt("max_chunk_rows", 4096));
    st->cache_max_rows = static_cast<std::size_t>(cfg_.paramInt("cache_max_rows", 50000));
    st->cache_collect  = true;

    // Mark every node as "expected to start" so the buffer correctly tracks
    // how many producer-done events we still need to see.
    for (const auto& nid : cfg_.allNodeIds()) st->buffer->markProducerStarted(nid);

    // Build a route entry: portal is its own upstream sentinel.
    auto route = std::make_shared<RouteState>();
    route->upstream_peer = kSelfRoute;
    route->visited.push_back(my_id_);
    route->spec = req->spec();
    route->created_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        routes_[rid] = route;
    }

    // Fan out to neighbors immediately.
    for (auto& kv : peer_clients_) {
        ForwardQueryRequest f;
        f.set_request_id(rid);
        *f.mutable_spec() = req->spec();
        f.set_origin_node(my_id_);
        f.set_from_node(my_id_);
        for (const auto& v : route->visited) f.add_visited(v);
        f.set_ttl(16);
        f.set_issued_at_unix_us(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        ForwardQueryAck fa;
        bool ok = kv.second->forwardQuery(f, &fa);
        if (ok && fa.accepted()) {
            route->fanout_to[kv.first] = kv.first;
        } else {
            std::cerr << "[" << my_id_ << "] forwardQuery to "
                      << kv.first << " failed; marking it done early" << std::endl;
            // If a peer rejects, mark its subtree's contribution as "done"
            // so we don't wait forever. Coarse-grained but safe.
            st->buffer->markProducerDone(kv.first);
        }
    }

    // Also kick off our own local execution in a worker thread.
    std::thread([this, rid, spec = req->spec()] {
        executeLocal(rid, spec, kSelfRoute);
    }).detach();

    ack->set_accepted(true);
    ack->set_request_id(rid);
    return grpc::Status::OK;
}

grpc::Status NodeServer::FetchChunk(grpc::ServerContext*,
                                    const FetchRequest* req, ChunkResponse* rsp) {
    auto st = registry_.get(req->request_id());
    rsp->set_request_id(req->request_id());
    if (!st) {
        rsp->set_final(true);
        rsp->set_canceled(true);
        return grpc::Status::OK;
    }

    st->recordFetch();
    std::uint32_t want = req->max_rows();
    if (want == 0) want = st->suggested_chunk;
    if (want > st->max_chunk) want = st->max_chunk;

    int block_ms = req->block() ? cfg_.paramInt("fetch_block_ms", 150) : 0;
    auto rows = st->buffer->popUpTo(want, block_ms);

    // Shadow-accumulate for result cache (single-threaded FetchChunk path per request).
    if (st->cache_collect) {
        if (st->cache_accum.size() + rows.size() <= st->cache_max_rows) {
            for (const auto& r : rows) st->cache_accum.push_back(r);
        } else {
            st->cache_collect = false;
            st->cache_accum.clear();
            st->cache_accum.shrink_to_fit();
        }
    }

    for (auto& r : rows) *rsp->add_rows() = std::move(r);
    rsp->set_suggested_next_max(st->suggested_chunk);
    rsp->set_rows_so_far(st->buffer->rowsServed());
    rsp->set_pending_producers(static_cast<std::uint32_t>(st->buffer->pendingProducers()));
    rsp->set_canceled(st->buffer->isCanceled());
    rsp->set_final(st->buffer->isFinal());
    total_rows_served_ += rsp->rows().size();

    if (rsp->final()) {
        if (st->cache_collect && !rsp->canceled()) {
            cache_.put(st->spec, std::move(st->cache_accum));
            std::cerr << "[" << my_id_ << "] cached result rid=" << req->request_id() << "\n";
        }
        registry_.erase(req->request_id());
    }
    return grpc::Status::OK;
}

grpc::Status NodeServer::CancelQuery(grpc::ServerContext*,
                                     const CancelRequest* req, CancelAck* ack) {
    auto st = registry_.get(req->request_id());
    if (st) st->buffer->cancel();

    // Propagate downward
    AbortRequest a; a.set_request_id(req->request_id());
    a.set_from_node(my_id_); a.set_reason(req->reason());
    AbortAck aa;
    for (auto& kv : peer_clients_) (void)kv.second->abort(a, &aa);

    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        auto it = routes_.find(req->request_id());
        if (it != routes_.end()) it->second->canceled.store(true);
    }
    ack->set_accepted(true);
    return grpc::Status::OK;
}

grpc::Status NodeServer::GetStats(grpc::ServerContext*,
                                  const StatsRequest*, StatsResponse* rsp) {
    rsp->set_node_id(my_id_);
    rsp->set_active_requests(static_cast<std::uint32_t>(registry_.size()));
    rsp->set_buffered_rows(registry_.totalBufferedRows());
    rsp->set_total_rows_served(total_rows_served_.load());
    rsp->set_peer_count(static_cast<std::uint32_t>(peer_clients_.size()));
    rsp->set_partition_rows(store_.size());
    rsp->set_cache_hits(cache_.hits());
    rsp->set_cache_misses(cache_.misses());
    return grpc::Status::OK;
}

// ---------- Overlay service ----------

grpc::Status NodeServer::ForwardQuery(grpc::ServerContext*,
                                      const ForwardQueryRequest* req,
                                      ForwardQueryAck* ack) {
    // Loop avoidance: if we're already in the visited set, ignore.
    for (const auto& v : req->visited()) {
        if (v == my_id_) {
            ack->set_accepted(false);
            ack->set_node_id(my_id_);
            ack->set_error("loop");
            return grpc::Status::OK;
        }
    }

    // Dedupe by request_id: once we've accepted a forward for a given
    // request, every subsequent forward (e.g. arriving via a different
    // path in a cyclic overlay) is a no-op. Without this check the same
    // leaf can produce its rows multiple times.
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        if (routes_.find(req->request_id()) != routes_.end()) {
            ack->set_accepted(false);
            ack->set_node_id(my_id_);
            ack->set_error("dup");
            return grpc::Status::OK;
        }
    }

    // Cache the route state so PushChunk replies know how to walk back up.
    auto route = std::make_shared<RouteState>();
    route->upstream_peer = req->from_node();
    route->visited.assign(req->visited().begin(), req->visited().end());
    route->visited.push_back(my_id_);
    route->spec = req->spec();
    route->created_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        routes_[req->request_id()] = route;
    }

    // Fan out to neighbors that aren't already visited.
    for (auto& kv : peer_clients_) {
        const std::string& nb = kv.first;
        if (nb == req->from_node()) continue;
        bool already = false;
        for (const auto& v : route->visited) if (v == nb) { already = true; break; }
        if (already) continue;

        ForwardQueryRequest f = *req;
        f.set_from_node(my_id_);
        f.clear_visited();
        for (const auto& v : route->visited) f.add_visited(v);
        f.set_ttl(req->ttl() == 0 ? 0 : req->ttl() - 1);

        ForwardQueryAck fa;
        if (kv.second->forwardQuery(f, &fa) && fa.accepted()) {
            route->fanout_to[nb] = nb;
        }
        // If a downstream subtree refuses we don't try to mark it done from
        // here -- only the portal owns the registry. The portal handles its
        // own subtree refusals via the SubmitQuery loop.
    }

    // Run our own local query asynchronously.
    std::string rid = req->request_id();
    QuerySpec spec = req->spec();
    std::string upstream = req->from_node();
    std::thread([this, rid, spec, upstream] {
        executeLocal(rid, spec, upstream);
    }).detach();

    ack->set_accepted(true);
    ack->set_node_id(my_id_);
    return grpc::Status::OK;
}

grpc::Status NodeServer::PushChunk(grpc::ServerContext*,
                                   const PushChunkRequest* req,
                                   PushChunkAck* ack) {
    // If we are the portal, deposit into the buffer.
    if (cfg_.node(my_id_).is_root) {
        auto st = registry_.get(req->request_id());
        if (!st) {
            ack->set_ok(false);
            ack->set_canceled(true);
            return grpc::Status::OK;
        }
        if (st->buffer->isCanceled()) {
            ack->set_canceled(true);
            ack->set_ok(false);
            return grpc::Status::OK;
        }
        std::vector<Row311> rows;
        rows.reserve(req->rows_size());
        for (const auto& r : req->rows()) rows.push_back(r);
        std::size_t took = st->buffer->pushRows(std::move(rows));
        if (req->done()) st->buffer->markProducerDone(req->producer_node());
        ack->set_ok(true);
        ack->set_buffered_rows(static_cast<std::uint32_t>(st->buffer->bufferedRows()));
        if (took < static_cast<std::size_t>(req->rows_size())) {
            ack->set_suggested_delay_ms(cfg_.paramInt("push_backoff_ms", 25));
        }
        return grpc::Status::OK;
    }

    // Otherwise we are an internal node: forward the chunk one hop upstream.
    std::shared_ptr<RouteState> route;
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        auto it = routes_.find(req->request_id());
        if (it != routes_.end()) route = it->second;
    }
    if (!route) {
        ack->set_ok(false);
        ack->set_canceled(true);
        return grpc::Status::OK;
    }
    PeerClient* up = peer(route->upstream_peer);
    if (!up) {
        ack->set_ok(false);
        return grpc::Status::OK;
    }

    PushChunkRequest f = *req;
    f.set_from_node(my_id_);
    PushChunkAck fa;
    bool ok = up->pushChunk(f, &fa);
    ack->set_ok(ok && fa.ok());
    ack->set_canceled(fa.canceled());
    ack->set_buffered_rows(fa.buffered_rows());
    ack->set_suggested_delay_ms(fa.suggested_delay_ms());
    return grpc::Status::OK;
}

grpc::Status NodeServer::Abort(grpc::ServerContext*,
                               const AbortRequest* req, AbortAck* ack) {
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        auto it = routes_.find(req->request_id());
        if (it != routes_.end()) it->second->canceled.store(true);
    }
    // Propagate further (best-effort)
    for (auto& kv : peer_clients_) {
        if (kv.first == req->from_node()) continue;
        AbortRequest f = *req;
        f.set_from_node(my_id_);
        AbortAck fa;
        (void)kv.second->abort(f, &fa);
    }
    ack->set_ok(true);
    return grpc::Status::OK;
}

grpc::Status NodeServer::Ping(grpc::ServerContext*,
                              const PingRequest*, PingResponse* rsp) {
    rsp->set_node_id(my_id_);
    rsp->set_unix_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    rsp->set_uptime_s(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_).count()));
    rsp->set_partition_rows(store_.size());
    return grpc::Status::OK;
}

// ---------- Local execution ----------

void NodeServer::executeLocal(const std::string& request_id,
                              const QuerySpec&   spec,
                              const std::string& upstream_peer) {
    std::shared_ptr<RouteState> route;
    {
        std::lock_guard<std::mutex> lk(routes_mu_);
        auto it = routes_.find(request_id);
        if (it != routes_.end()) route = it->second;
    }
    auto canceled_fn = [route]() {
        return route ? route->canceled.load() : false;
    };

    QueryEngine eng(store_);
    std::uint32_t initial = static_cast<std::uint32_t>(cfg_.paramInt("initial_chunk_rows", 512));
    std::uint32_t seq = 0;

    eng.run(spec, initial, canceled_fn,
        [&](std::vector<Row311>&& rows, bool done) {
            // Build a PushChunk and send upstream.
            PushChunkRequest p;
            p.set_request_id(request_id);
            p.set_from_node(my_id_);
            p.set_producer_node(my_id_);
            for (auto& r : rows) *p.add_rows() = std::move(r);
            p.set_done(done);
            p.set_seq(++seq);
            p.set_produced_at_us(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            if (upstream_peer == kSelfRoute) {
                // We are the portal. Deposit straight into our own registry.
                auto st = registry_.get(request_id);
                if (!st) return;
                std::vector<Row311> v;
                v.reserve(p.rows_size());
                for (const auto& r : p.rows()) v.push_back(r);
                st->buffer->pushRows(std::move(v));
                if (done) st->buffer->markProducerDone(my_id_);
                return;
            }

            PeerClient* up = peer(upstream_peer);
            if (!up) return;
            PushChunkAck ack;
            int retries = 5;
            while (retries-- > 0) {
                if (up->pushChunk(p, &ack) && ack.ok()) break;
                if (ack.canceled()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::max(1u, ack.suggested_delay_ms())));
            }
        });
}

} // namespace mini2
