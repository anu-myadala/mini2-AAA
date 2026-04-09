// ChunkBuffer.hpp - Per-request, bounded, producer/consumer row buffer.
//
// Semantics:
//   * Producers push rows via pushRows(). If buffer is full, push blocks up
//     to backoff_ms and returns short-count; callers can retry. This is the
//     fairness knob: slow client => full buffer => producers throttle.
//   * The consumer (FetchChunk handler) drains rows via popUpTo().
//   * markProducerDone() notifies that a given leaf is finished.
//   * isFinal() becomes true once all known producers are done AND no rows
//     are buffered.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "overlay.pb.h"

namespace mini2 {

class ChunkBuffer {
public:
    ChunkBuffer(std::size_t capacity, std::size_t expected_producers)
      : capacity_(capacity), expected_producers_(expected_producers) {}

    // Push rows. Returns number of rows actually accepted (may be < rows.size()
    // if we'd exceed capacity). Thread-safe.
    std::size_t pushRows(std::vector<Row311>&& rows);

    // Drain up to max_rows. Will block up to block_ms if currently empty and
    // not yet final.
    std::vector<Row311> popUpTo(std::uint32_t max_rows, int block_ms);

    void markProducerStarted(const std::string& producer);
    void markProducerDone   (const std::string& producer);

    void cancel();
    bool isCanceled()       const;

    bool   isFinal()        const;
    std::size_t bufferedRows() const;
    std::size_t pendingProducers() const;
    std::uint64_t rowsServed()  const;

private:
    mutable std::mutex       mu_;
    std::condition_variable  cv_not_empty_;
    std::condition_variable  cv_not_full_;

    std::deque<Row311>       q_;
    std::size_t              capacity_;
    std::size_t              expected_producers_;
    std::unordered_set<std::string> started_;
    std::unordered_set<std::string> done_;
    std::uint64_t            rows_served_ = 0;
    bool                     canceled_ = false;
};

} // namespace mini2
