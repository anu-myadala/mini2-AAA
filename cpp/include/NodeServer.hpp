// NodeServer.hpp - A single mini2 node. Hosts both the Portal service and
// the Overlay service on the same port; the service semantics are identical
// across every node, but only the root (A) ever gets real client traffic.
//
// Key responsibilities:
//   * ForwardQuery handling: fan out the query to neighbors that haven't been
//     visited yet, then execute locally and push results up via PushChunk.
//   * PushChunk handling: if we are the origin (portal), deposit rows in the
//     request's ChunkBuffer. Otherwise forward upward to the node we received
//     the ForwardQuery from.
//   * Portal handling (A only): SubmitQuery / FetchChunk / CancelQuery / GetStats.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "Config.hpp"
#include "PartitionStore.hpp"
#include "PeerClient.hpp"
#include "QueryCache.hpp"
#include "RequestRegistry.hpp"
#include "overlay.grpc.pb.h"

namespace mini2 {

// Tracks, per in-flight request, which neighbor we should push rows back to.
// For internal (non-root) nodes this is the node that forwarded the query to
// us. The portal uses a sentinel "__self__" value meaning "drop into the
// local RequestRegistry buffer".
struct RouteState {
    std::string upstream_peer; // id; "__self__" if we are the portal
    std::unordered_map<std::string /*peer_id*/, std::string> fanout_to; // who we forwarded to
    std::atomic<bool> canceled{false};
    std::vector<std::string> visited;
    QuerySpec spec;
    std::chrono::steady_clock::time_point created_at;
};

class NodeServer final : public Portal::Service, public Overlay::Service {
public:
    NodeServer(std::string my_id, Config cfg);
    ~NodeServer();

    // Start loading partition, build peer clients, start gRPC server, start
    // janitor. Blocks until Wait() returns (shutdown).
    void run();

    // Trigger shutdown (thread-safe).
    void shutdown();

private:
    // Portal service ------------------------------------------------------
    grpc::Status SubmitQuery(grpc::ServerContext*, const SubmitRequest*, SubmitAck*) override;
    grpc::Status FetchChunk (grpc::ServerContext*, const FetchRequest*,  ChunkResponse*) override;
    grpc::Status CancelQuery(grpc::ServerContext*, const CancelRequest*, CancelAck*) override;
    grpc::Status GetStats   (grpc::ServerContext*, const StatsRequest*,  StatsResponse*) override;

    // Overlay service -----------------------------------------------------
    grpc::Status ForwardQuery(grpc::ServerContext*, const ForwardQueryRequest*, ForwardQueryAck*) override;
    grpc::Status PushChunk   (grpc::ServerContext*, const PushChunkRequest*,    PushChunkAck*) override;
    grpc::Status Abort       (grpc::ServerContext*, const AbortRequest*,        AbortAck*) override;
    grpc::Status Ping        (grpc::ServerContext*, const PingRequest*,         PingResponse*) override;

    // Internals -----------------------------------------------------------
    void executeLocal(const std::string& request_id,
                      const QuerySpec&   spec,
                      const std::string& upstream_peer);
    void janitorLoop();
    std::string newRequestId();

    PeerClient* peer(const std::string& id);  // may return nullptr

    // Members -------------------------------------------------------------
    std::string       my_id_;
    Config            cfg_;
    QueryCache        cache_;              // portal-only: LRU result cache
    PartitionStore    store_;

    std::unordered_map<std::string, std::unique_ptr<PeerClient>> peer_clients_;

    RequestRegistry   registry_;           // portal-only (root node)
    mutable std::mutex routes_mu_;
    std::unordered_map<std::string, std::shared_ptr<RouteState>> routes_;
    std::atomic<std::uint64_t> req_counter_{0};
    std::atomic<std::uint64_t> total_rows_served_{0};

    std::unique_ptr<grpc::Server> server_;
    std::thread                   janitor_;
    std::atomic<bool>             stop_{false};
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace mini2
