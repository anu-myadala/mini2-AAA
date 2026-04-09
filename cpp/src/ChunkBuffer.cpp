// ChunkBuffer.cpp
#include "ChunkBuffer.hpp"

#include <algorithm>
#include <chrono>

namespace mini2 {

std::size_t ChunkBuffer::pushRows(std::vector<Row311>&& rows) {
    if (rows.empty()) return 0;
    std::unique_lock<std::mutex> lk(mu_);
    if (canceled_) return rows.size();      // pretend accepted; just drop

    // Cap how many we accept this call so producers feel backpressure
    // instead of buffer growing unbounded.
    std::size_t avail = (q_.size() >= capacity_) ? 0 : (capacity_ - q_.size());
    std::size_t take  = std::min(rows.size(), avail);
    for (std::size_t i = 0; i < take; ++i) q_.push_back(std::move(rows[i]));
    if (take > 0) cv_not_empty_.notify_all();
    return take;
}

std::vector<Row311> ChunkBuffer::popUpTo(std::uint32_t max_rows, int block_ms) {
    std::vector<Row311> out;
    if (max_rows == 0) return out;
    out.reserve(max_rows);

    std::unique_lock<std::mutex> lk(mu_);
    if (q_.empty() && !canceled_ &&
        !(done_.size() >= expected_producers_) &&
        block_ms > 0) {
        cv_not_empty_.wait_for(lk, std::chrono::milliseconds(block_ms),
            [this] {
                return !q_.empty() || canceled_ ||
                       done_.size() >= expected_producers_;
            });
    }
    while (!q_.empty() && out.size() < max_rows) {
        out.emplace_back(std::move(q_.front()));
        q_.pop_front();
    }
    rows_served_ += out.size();
    if (!out.empty()) cv_not_full_.notify_all();
    return out;
}

void ChunkBuffer::markProducerStarted(const std::string& producer) {
    std::lock_guard<std::mutex> lk(mu_);
    started_.insert(producer);
}

void ChunkBuffer::markProducerDone(const std::string& producer) {
    std::lock_guard<std::mutex> lk(mu_);
    done_.insert(producer);
    cv_not_empty_.notify_all();
}

void ChunkBuffer::cancel() {
    std::lock_guard<std::mutex> lk(mu_);
    canceled_ = true;
    q_.clear();
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
}

bool ChunkBuffer::isCanceled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return canceled_;
}

bool ChunkBuffer::isFinal() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (canceled_) return true;
    return q_.empty() && done_.size() >= expected_producers_;
}

std::size_t ChunkBuffer::bufferedRows() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

std::size_t ChunkBuffer::pendingProducers() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (done_.size() >= expected_producers_) return 0;
    return expected_producers_ - done_.size();
}

std::uint64_t ChunkBuffer::rowsServed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rows_served_;
}

} // namespace mini2
