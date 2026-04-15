// Indices.hpp - Per-partition secondary indices for predicate pushdown.
//
// Built once after PartitionStore::load() is finished. All indices are
// immutable thereafter so multiple worker threads can read them
// concurrently without locking. Storage is row-id based (uint32_t) so
// the index footprint is roughly 4 bytes per indexed (column, row) pair.
//
// Why this file exists:
// The professor's mini1 critique on our team specifically called out:
//   "Phase 3 is still a linear search, with column storage, what could
//    the team have done to extract more performance?"
//   "How would you arrange the data structures to allow for multi-field
//    queries (e.g., lat/lon)?"
// This module is the response. QueryEngine now uses these indices to
// build a small candidate row-id set per query instead of scanning
// every row in the partition. See docs/DESIGN_RATIONALE.md §7.
//
// What is indexed (chosen because they are the predicates QuerySpec
// actually carries):
//   * borough        - hash bucket: BoroughEnum -> sorted vector<row_id>
//   * complaint_type - hash bucket: string      -> sorted vector<row_id>
//   * created_ymd    - sorted vector of (date, row_id) for range pruning
//                      (binary search to clip both ends of a date window)
//   * (lat, lon)     - 2D grid index. Cells are GRID_DEG degrees on a
//                      side; each cell holds a sorted vector<row_id>.
//                      For a geo box query we iterate only the cells
//                      that overlap the box, which is the textbook
//                      multi-field-query answer for spatial data.
//
// All bucket vectors are kept sorted by row_id so QueryEngine can run
// std::set_intersection on candidate sets without re-sorting.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Record311.hpp"

namespace mini2 {

class PartitionStore;  // fwd

class Indices {
public:
    using RowId = std::uint32_t;

    // Grid cell width in degrees. ~0.01 degrees ≈ 1.1 km at NYC's
    // latitude. NYC fits in <40 cells × <60 cells = <2,400 cells, which
    // is small enough for unordered_map without a custom hash.
    static constexpr double GRID_DEG = 0.01;

    // Encode a (lat, lon) into a single 64-bit grid cell key. Negative
    // floats are handled correctly by std::floor.
    static std::uint64_t cellKey(double lat, double lon) {
        auto la = static_cast<std::int32_t>(std::floor(lat / GRID_DEG));
        auto lo = static_cast<std::int32_t>(std::floor(lon / GRID_DEG));
        // Pack two int32 into one uint64. Bias to keep order positive.
        const std::uint64_t hi = static_cast<std::uint32_t>(la + (1 << 30));
        const std::uint64_t lw = static_cast<std::uint32_t>(lo + (1 << 30));
        return (hi << 32) | lw;
    }

    // Build all four indices from the column data of a PartitionStore.
    // The store columns must be loaded and stable for the lifetime of
    // this Indices object.
    void build(const PartitionStore& store);

    // ---- Accessors used by QueryEngine ----

    // Empty optional (returns nullptr) if the borough has no rows.
    const std::vector<RowId>* boroughRows(Borough b) const;

    // Empty optional if the complaint type has no rows.
    const std::vector<RowId>* complaintRows(const std::string& s) const;

    // Returns row ids whose date is in [from, to] inclusive. `from`/`to`
    // == 0 means unbounded on that side. Output is sorted ascending by
    // row_id (NOT by date) so it composes with set_intersection.
    std::vector<RowId> dateRange(std::uint32_t from, std::uint32_t to) const;

    // Returns row ids whose (lat, lon) falls inside the inclusive box.
    // Output is sorted ascending by row_id. The grid is approximate (it
    // returns cell-level supersets); QueryEngine still validates each
    // row against the actual box. This is the textbook two-stage
    // spatial filter: cheap-and-loose grid scan, then exact predicate.
    std::vector<RowId> geoBox(double minLat, double maxLat,
                              double minLon, double maxLon) const;

    // Total rows in the partition (denominator for selectivity stats).
    std::size_t totalRows() const { return total_rows_; }

    // Diagnostic: how many distinct values per index.
    std::size_t boroughBuckets()   const { return by_borough_.size(); }
    std::size_t complaintBuckets() const { return by_complaint_.size(); }
    std::size_t dateEntries()      const { return by_date_.size(); }
    std::size_t gridCells()        const { return by_cell_.size(); }

    // Memory footprint estimate, for the report. Counts only the index
    // overhead, not the underlying column storage.
    std::size_t estimateBytes() const;

private:
    std::size_t total_rows_ = 0;

    // borough -> sorted vector<row_id>
    std::unordered_map<std::uint8_t, std::vector<RowId>> by_borough_;

    // complaint_type -> sorted vector<row_id>
    std::unordered_map<std::string, std::vector<RowId>> by_complaint_;

    // (date, row_id) sorted by (date, row_id). Binary search by date
    // gives both range endpoints in O(log n). Output of dateRange()
    // re-sorts the resulting row_ids ascending.
    std::vector<std::pair<std::uint32_t, RowId>> by_date_;

    // grid cell -> sorted vector<row_id>
    std::unordered_map<std::uint64_t, std::vector<RowId>> by_cell_;
};

} // namespace mini2
