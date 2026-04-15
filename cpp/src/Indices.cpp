// Indices.cpp - Build + query secondary indices.
//
// Build cost is roughly O(R log R) total per partition (the date index
// dominates because of its sort). At R = ~250k rows per partition for a
// 9-way split of a typical 311 dataset that is well under 100 ms even
// in debug builds, and it happens once at startup.
#include "Indices.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "PartitionStore.hpp"

namespace mini2 {

void Indices::build(const PartitionStore& store) {
    const auto& dates  = store.dates();
    const auto& boros  = store.boroughs();
    const auto& lats   = store.lats();
    const auto& lons   = store.lons();
    const auto& comps  = store.complaints();
    total_rows_ = store.size();

    // ---- borough bucket (5+1 distinct values; tiny) ----
    by_borough_.clear();
    by_borough_.reserve(8);
    for (RowId i = 0; i < total_rows_; ++i) {
        by_borough_[static_cast<std::uint8_t>(boros[i])].push_back(i);
    }
    // Already inserted in row-id order, so sorted by construction.

    // ---- complaint_type bucket (~200 distinct strings; small) ----
    by_complaint_.clear();
    // Reserve guess: assume avg 1000 rows / type. Conservative.
    by_complaint_.reserve(std::max<std::size_t>(64, total_rows_ / 1000));
    for (RowId i = 0; i < total_rows_; ++i) {
        by_complaint_[comps[i]].push_back(i);
    }

    // ---- sorted date index (one allocation, then std::sort) ----
    by_date_.clear();
    by_date_.reserve(total_rows_);
    for (RowId i = 0; i < total_rows_; ++i) {
        by_date_.emplace_back(dates[i], i);
    }
    // Sort ascending by date; ties resolved by row_id so equal-range scans
    // are deterministic.
    std::sort(by_date_.begin(), by_date_.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second < b.second;
              });

    // ---- spatial grid (cells of GRID_DEG on a side) ----
    by_cell_.clear();
    by_cell_.reserve(total_rows_ / 8 + 16);
    for (RowId i = 0; i < total_rows_; ++i) {
        const double la = lats[i];
        const double lo = lons[i];
        // Skip obvious junk (0,0) which often appears in NYC 311 data
        // when a complaint had no geocoded location. Keeping them in the
        // grid would create one absurdly heavy cell at (0,0).
        if (la == 0.0 && lo == 0.0) continue;
        by_cell_[cellKey(la, lo)].push_back(i);
    }
    // Cells were appended in row-id order, so sorted by construction.
}

const std::vector<Indices::RowId>* Indices::boroughRows(Borough b) const {
    auto it = by_borough_.find(static_cast<std::uint8_t>(b));
    return (it == by_borough_.end()) ? nullptr : &it->second;
}

const std::vector<Indices::RowId>* Indices::complaintRows(const std::string& s) const {
    auto it = by_complaint_.find(s);
    return (it == by_complaint_.end()) ? nullptr : &it->second;
}

std::vector<Indices::RowId>
Indices::dateRange(std::uint32_t from, std::uint32_t to) const {
    std::vector<RowId> out;
    if (by_date_.empty()) return out;

    // Convert "0 = unbounded" into actual numeric bounds.
    const std::uint32_t lo = (from == 0) ? 0u                : from;
    const std::uint32_t hi = (to   == 0) ? 0xFFFFFFFFu       : to;
    if (lo > hi) return out;

    // Binary-search the date-sorted vector for the [lo, hi] window.
    auto lo_it = std::lower_bound(
        by_date_.begin(), by_date_.end(), lo,
        [](const std::pair<std::uint32_t, RowId>& e, std::uint32_t v) {
            return e.first < v;
        });
    auto hi_it = std::upper_bound(
        by_date_.begin(), by_date_.end(), hi,
        [](std::uint32_t v, const std::pair<std::uint32_t, RowId>& e) {
            return v < e.first;
        });

    out.reserve(static_cast<std::size_t>(hi_it - lo_it));
    for (auto it = lo_it; it != hi_it; ++it) out.push_back(it->second);

    // The result is sorted by date (and within a date by row_id). For
    // set_intersection we need it sorted by row_id, so resort.
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Indices::RowId>
Indices::geoBox(double minLat, double maxLat,
                double minLon, double maxLon) const
{
    std::vector<RowId> out;
    if (by_cell_.empty()) return out;
    if (minLat > maxLat || minLon > maxLon) return out;

    // Iterate the rectangle of cells that overlaps the requested box.
    // Inclusive on both ends in row terms; the validate-against-actual-
    // box step happens in QueryEngine.
    const auto la0 = static_cast<std::int32_t>(std::floor(minLat / GRID_DEG));
    const auto la1 = static_cast<std::int32_t>(std::floor(maxLat / GRID_DEG));
    const auto lo0 = static_cast<std::int32_t>(std::floor(minLon / GRID_DEG));
    const auto lo1 = static_cast<std::int32_t>(std::floor(maxLon / GRID_DEG));

    // Reserve a guess: avg ~ R/cells * (cells in box) rows.
    std::size_t avg_per_cell =
        by_cell_.empty() ? 0 : (total_rows_ / by_cell_.size() + 1);
    std::size_t cells_in_box =
        static_cast<std::size_t>((la1 - la0 + 1)) *
        static_cast<std::size_t>((lo1 - lo0 + 1));
    out.reserve(avg_per_cell * cells_in_box);

    for (std::int32_t la = la0; la <= la1; ++la) {
        for (std::int32_t lo = lo0; lo <= lo1; ++lo) {
            const std::uint64_t hi = static_cast<std::uint32_t>(la + (1 << 30));
            const std::uint64_t lw = static_cast<std::uint32_t>(lo + (1 << 30));
            const std::uint64_t key = (hi << 32) | lw;
            auto it = by_cell_.find(key);
            if (it == by_cell_.end()) continue;
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
    }

    // We pulled from multiple cells; merge to a single sorted stream.
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t Indices::estimateBytes() const {
    std::size_t b = 0;
    for (const auto& kv : by_borough_)   b += sizeof(RowId) * kv.second.capacity();
    for (const auto& kv : by_complaint_) b += sizeof(RowId) * kv.second.capacity()
                                              + kv.first.capacity();
    b += by_date_.capacity() * sizeof(std::pair<std::uint32_t, RowId>);
    for (const auto& kv : by_cell_)      b += sizeof(RowId) * kv.second.capacity();
    return b;
}

} // namespace mini2
