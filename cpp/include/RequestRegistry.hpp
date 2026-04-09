// RequestRegistry.hpp - Tracks all in-flight client requests at the portal
// (node A). Each request owns a ChunkBuffer and dynamic-chunk sizing state.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ChunkBuffer.hpp"
#include "overlay.pb.h"

namespace mini2 {

struct RequestState {
    std::string                    request_id;
    std::string                    client_id;
    QuerySpec                      spec;
    std::shared_ptr<ChunkBuffer>   buffer;

    // Dynamic chunk sizing (EWMA of client fetch cadence).
    std::uint32_t suggested_chunk = 512;
    std::uint32_t min_chunk       = 64;
    std::uint32_t max_chunk       = 4096;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_fetch_at;
    double        ewma_fetch_ms  = 100.0;

    // Update chunk hint based on observed fetch cadence.
    void recordFetch();
};

class RequestRegistry {
public:
    std::shared_ptr<RequestState>
    create(const std::string& request_id,
           const std::string& client_id,
           const QuerySpec&   spec,
           std::size_t        buffer_cap,
           std::size_t        expected_producers,
           std::uint32_t      initial_chunk,
           std::uint32_t      min_chunk,
           std::uint32_t      max_chunk);

    std::shared_ptr<RequestState> get(const std::string& request_id) const;
    void erase(const std::string& request_id);

    // Return all requests whose last_fetch_at is older than ttl.
    std::vector<std::shared_ptr<RequestState>>
    collectExpired(std::chrono::seconds ttl) const;

    std::size_t size() const;
    std::uint64_t totalBufferedRows() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<RequestState>> reqs_;
};

} // namespace mini2
