// PartitionStore.cpp - tiny CSV loader feeding the SoA columns.
//
// We expect partition CSVs produced by scripts/partition_data.py, which
// always emit a fixed header in this order:
//   unique_key,created_ymd,borough,latitude,longitude,complaint_type
//
// (We accept the original NYC OpenData header too, in case the user wants
// to point a node directly at a raw 311 dump.)
#include "PartitionStore.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mini2 {

namespace {

// Splits a CSV line. Handles double-quoted fields containing commas. We do
// not support escaped quotes inside quotes for the partition CSVs (those
// are produced by us); for the OpenData fallback we tolerate it.
void splitCsv(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        if (c == ',' && !in_quotes) { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    out.push_back(cur);
}

struct ColMap {
    int unique_key = -1, created = -1, complaint = -1;
    int borough = -1, lat = -1, lon = -1;
    bool created_is_ymd = false;

    static ColMap detect(const std::vector<std::string>& hdr) {
        ColMap m;
        for (int i = 0; i < static_cast<int>(hdr.size()); ++i) {
            const auto& h = hdr[i];
            if      (h == "unique_key"  || h == "Unique Key")     m.unique_key = i;
            else if (h == "created_ymd")                          { m.created = i; m.created_is_ymd = true; }
            else if (h == "created_date" || h == "Created Date")  m.created   = i;
            else if (h == "complaint_type" || h == "Complaint Type") m.complaint = i;
            else if (h == "borough"      || h == "Borough")       m.borough = i;
            else if (h == "latitude"     || h == "Latitude")      m.lat = i;
            else if (h == "longitude"    || h == "Longitude")     m.lon = i;
        }
        return m;
    }
};

uint32_t parseDateAny(const std::string& s, bool already_ymd) {
    if (s.empty()) return 0;
    if (already_ymd) return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    return parseYmd(s);
}

double parseDoubleSafe(const std::string& s) {
    if (s.empty()) return 0.0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return (end == s.c_str()) ? 0.0 : v;
}

uint64_t parseU64Safe(const std::string& s) {
    if (s.empty()) return 0;
    return std::strtoull(s.c_str(), nullptr, 10);
}

} // namespace

void PartitionStore::load(const std::string& csv_path, std::size_t row_hint) {
    std::ifstream in(csv_path);
    if (!in) {
        std::cerr << "[PartitionStore] no file at '" << csv_path << "'\n";
        return;
    }

    // Estimate row count from file size to pre-reserve (avoids repeated
    // realloc + malloc overhead — prof's mini1 feedback).
    if (row_hint == 0) {
        in.seekg(0, std::ios::end);
        auto fsz = static_cast<std::streamoff>(in.tellg());
        in.seekg(0, std::ios::beg);
        if (fsz > 0) row_hint = static_cast<std::size_t>(fsz / 60);
    }
    if (row_hint > 0) {
        unique_keys_.reserve(row_hint);
        created_ymds_.reserve(row_hint);
        boroughs_.reserve(row_hint);
        latitudes_.reserve(row_hint);
        longitudes_.reserve(row_hint);
        complaint_types_.reserve(row_hint);
    }

    std::string line;
    if (!std::getline(in, line)) return;

    std::vector<std::string> fields;
    splitCsv(line, fields);
    ColMap cm = ColMap::detect(fields);
    if (cm.unique_key < 0 || cm.created < 0) {
        throw std::runtime_error("PartitionStore: header missing required columns in " + csv_path);
    }

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        splitCsv(line, fields);
        const int n = static_cast<int>(fields.size());
        if (cm.unique_key >= n || cm.created >= n) continue;

        uint64_t key  = parseU64Safe(fields[cm.unique_key]);
        if (key == 0) continue;
        uint32_t date = parseDateAny(fields[cm.created], cm.created_is_ymd);

        Borough b = Borough::UNSPECIFIED;
        if (cm.borough >= 0 && cm.borough < n) b = boroughFromString(fields[cm.borough]);

        double lat = (cm.lat >= 0 && cm.lat < n) ? parseDoubleSafe(fields[cm.lat]) : 0.0;
        double lon = (cm.lon >= 0 && cm.lon < n) ? parseDoubleSafe(fields[cm.lon]) : 0.0;

        std::string complaint;
        if (cm.complaint >= 0 && cm.complaint < n) complaint = fields[cm.complaint];

        unique_keys_.push_back(key);
        created_ymds_.push_back(date);
        boroughs_.push_back(b);
        latitudes_.push_back(lat);
        longitudes_.push_back(lon);
        complaint_types_.push_back(std::move(complaint));
    }

    // Build secondary indices once. Immutable thereafter, so concurrent
    // queries can read them lock-free. Total cost is dominated by the
    // O(n log n) sort of the date index; for ~250k rows per partition
    // this is well under 100 ms in release builds.
    indices_.build(*this);
    std::cerr << "[PartitionStore] indices: borough_buckets="
              << indices_.boroughBuckets()
              << " complaint_buckets=" << indices_.complaintBuckets()
              << " date_entries="      << indices_.dateEntries()
              << " grid_cells="        << indices_.gridCells()
              << " est_bytes="         << indices_.estimateBytes()
              << "\n";
}

} // namespace mini2
