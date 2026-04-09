// RequestRegistry.cpp
#include "RequestRegistry.hpp"

#include <algorithm>

namespace mini2 {

void RequestState::recordFetch() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    auto dt_ms = std::chrono::duration<double, std::milli>(now - last_fetch_at).count();
    last_fetch_at = now;
    if (dt_ms < 0.1) dt_ms = 0.1;
    // EWMA, alpha = 0.3
    ewma_fetch_ms = 0.7 * ewma_fetch_ms + 0.3 * dt_ms;

    // Aim chunk so a fetch returns ~150ms worth of work; bound it.
    if (ewma_fetch_ms < 20.0) {
        suggested_chunk = std::min(max_chunk, suggested_chunk + suggested_chunk / 4 + 1);
    } else if (ewma_fetch_ms > 500.0) {
        suggested_chunk = std::max(min_chunk, suggested_chunk - suggested_chunk / 4 - 1);
    }
    if (suggested_chunk < min_chunk) suggested_chunk = min_chunk;
    if (suggested_chunk > max_chunk) suggested_chunk = max_chunk;
}

std::shared_ptr<RequestState> RequestRegistry::create(
    const std::string& request_id,
    const std::string& client_id,
    const QuerySpec&   spec,
    std::size_t        buffer_cap,
    std::size_t        expected_producers,
    std::uint32_t      initial_chunk,
    std::uint32_t      min_chunk,
    std::uint32_t      max_chunk)
{
    auto st = std::make_shared<RequestState>();
    st->request_id      = request_id;
    st->client_id       = client_id;
    st->spec            = spec;
    st->buffer          = std::make_shared<ChunkBuffer>(buffer_cap, expected_producers);
    st->suggested_chunk = initial_chunk;
    st->min_chunk       = min_chunk;
    st->max_chunk       = max_chunk;
    st->created_at      = std::chrono::steady_clock::now();
    st->last_fetch_at   = st->created_at;

    std::lock_guard<std::mutex> lk(mu_);
    reqs_[request_id] = st;
    return st;
}

std::shared_ptr<RequestState> RequestRegistry::get(const std::string& request_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = reqs_.find(request_id);
    return (it == reqs_.end()) ? nullptr : it->second;
}

void RequestRegistry::erase(const std::string& request_id) {
    std::lock_guard<std::mutex> lk(mu_);
    reqs_.erase(request_id);
}

std::vector<std::shared_ptr<RequestState>>
RequestRegistry::collectExpired(std::chrono::seconds ttl) const {
    std::vector<std::shared_ptr<RequestState>> out;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : reqs_) {
        if (now - kv.second->last_fetch_at > ttl) out.push_back(kv.second);
    }
    return out;
}

std::size_t RequestRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return reqs_.size();
}

std::uint64_t RequestRegistry::totalBufferedRows() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::uint64_t total = 0;
    for (const auto& kv : reqs_) total += kv.second->buffer->bufferedRows();
    return total;
}

} // namespace mini2
