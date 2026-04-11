#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "overlay.pb.h"

namespace mini2 {

// LRU result cache keyed on QuerySpec. Entries larger than max_rows_per_entry
// are not stored. capacity=0 disables the cache entirely.
class QueryCache {
public:
    QueryCache() = default;
    QueryCache(std::size_t capacity, std::size_t max_rows_per_entry);

    std::shared_ptr<const std::vector<Row311>> get(const QuerySpec& spec);
    void put(const QuerySpec& spec, std::vector<Row311> rows);

    std::uint64_t hits()   const { return hits_.load(std::memory_order_relaxed); }
    std::uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }
    std::size_t   size()   const;

private:
    static std::size_t hashSpec(const QuerySpec& s);
    static bool        specEqual(const QuerySpec& a, const QuerySpec& b);

    struct Entry {
        std::size_t hash;
        QuerySpec   spec;
        std::shared_ptr<const std::vector<Row311>> rows;
    };

    mutable std::mutex mu_;
    std::size_t        capacity_ = 0;
    std::size_t        max_rows_ = 0;
    std::list<Entry>   lru_;
    std::unordered_multimap<std::size_t, std::list<Entry>::iterator> index_;

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
};

} // namespace mini2
