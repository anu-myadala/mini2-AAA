// PartitionStore.hpp - Local SoA storage for one node's data partition.
//
// Columnar layout on purpose: filter queries stream over one column at a
// time, which is cache-friendly. The storage is immutable after load(), so
// concurrent reads from multiple worker threads need no locking.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Record311.hpp"

namespace mini2 {

class PartitionStore {
public:
    // Load a 311 CSV partition. Expects a header row. See CSVLoader for
    // column detection. Pre-reserves capacity if row_hint > 0 (addresses
    // prof's mini1 feedback about std::vector growth).
    void load(const std::string& csv_path, std::size_t row_hint = 0);

    std::size_t size() const { return unique_keys_.size(); }

    // Run a predicate over every row in the partition and emit matching
    // row indices into `out`. Works in range [begin,end) for thread-split.
    // The callback form is used to avoid allocating huge intermediate vectors
    // when producing streamed chunks.
    //
    // For each hit, `emit(idx)` is invoked. `emit` should be cheap.
    template <typename Emit, typename Pred>
    void scan(std::size_t begin, std::size_t end, Pred&& pred, Emit&& emit) const {
        const std::size_t n = unique_keys_.size();
        if (begin > n) begin = n;
        if (end > n)   end   = n;
        for (std::size_t i = begin; i < end; ++i) {
            if (pred(i)) emit(i);
        }
    }

    // Column accessors (const refs; no locking required post-load).
    const std::vector<uint64_t>&    keys()        const { return unique_keys_; }
    const std::vector<uint32_t>&    dates()       const { return created_ymds_; }
    const std::vector<Borough>&     boroughs()    const { return boroughs_; }
    const std::vector<double>&      lats()        const { return latitudes_; }
    const std::vector<double>&      lons()        const { return longitudes_; }
    const std::vector<std::string>& complaints()  const { return complaint_types_; }

private:
    std::vector<uint64_t>    unique_keys_;
    std::vector<uint32_t>    created_ymds_;
    std::vector<Borough>     boroughs_;
    std::vector<double>      latitudes_;
    std::vector<double>      longitudes_;
    std::vector<std::string> complaint_types_;
};

} // namespace mini2
