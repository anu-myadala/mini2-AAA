// QueryEngine.hpp - Applies a QuerySpec to a PartitionStore and produces
// result chunks. The engine is single-threaded per invocation on purpose:
// we want to characterize the inter-process behaviour, not intra-process
// parallelism (that was mini1). Multiple concurrent queries run as separate
// engine invocations.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "PartitionStore.hpp"
#include "overlay.pb.h"

namespace mini2 {

class QueryEngine {
public:
    explicit QueryEngine(const PartitionStore& store) : store_(store) {}

    // Execute the query, chunking output to `chunk_rows` rows per callback.
    // `sink` is invoked zero or more times with non-empty row sets and
    // finally once with `done=true` (possibly with an empty last chunk).
    //
    // `is_canceled` is polled between rows so the engine can stop early.
    void run(const QuerySpec& spec,
             std::uint32_t chunk_rows,
             const std::function<bool()>& is_canceled,
             const std::function<void(std::vector<Row311>&&, bool done)>& sink) const;

private:
    const PartitionStore& store_;
};

} // namespace mini2
