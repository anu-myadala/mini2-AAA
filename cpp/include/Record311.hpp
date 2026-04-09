// Record311.hpp - Typed domain record for NYC 311 rows.
//
// Reused from mini1 with the enum kept as a 1-byte type for cache density,
// and date packed as a 4-byte YYYYMMDD unsigned int instead of a string.
#pragma once

#include <cstdint>
#include <string>

namespace mini2 {

enum class Borough : uint8_t {
    UNSPECIFIED   = 0,
    MANHATTAN     = 1,
    BROOKLYN      = 2,
    QUEENS        = 3,
    BRONX         = 4,
    STATEN_ISLAND = 5
};

inline Borough boroughFromString(const std::string& s) {
    if (s == "MANHATTAN")     return Borough::MANHATTAN;
    if (s == "BROOKLYN")      return Borough::BROOKLYN;
    if (s == "QUEENS")        return Borough::QUEENS;
    if (s == "BRONX")         return Borough::BRONX;
    if (s == "STATEN ISLAND") return Borough::STATEN_ISLAND;
    return Borough::UNSPECIFIED;
}

inline const char* boroughToString(Borough b) {
    switch (b) {
        case Borough::MANHATTAN:     return "MANHATTAN";
        case Borough::BROOKLYN:      return "BROOKLYN";
        case Borough::QUEENS:        return "QUEENS";
        case Borough::BRONX:         return "BRONX";
        case Borough::STATEN_ISLAND: return "STATEN ISLAND";
        default:                     return "UNSPECIFIED";
    }
}

// Parse "YYYY-MM-DD..." into a packed YYYYMMDD uint32. Returns 0 on error.
inline uint32_t parseYmd(const std::string& s) {
    if (s.size() < 10) return 0;
    auto d = [](char c) -> uint32_t { return static_cast<uint32_t>(c - '0'); };
    if (s[0] < '0' || s[0] > '9') return 0;
    uint32_t y = d(s[0]) * 1000 + d(s[1]) * 100 + d(s[2]) * 10 + d(s[3]);
    uint32_t m = d(s[5]) * 10 + d(s[6]);
    uint32_t dd = d(s[8]) * 10 + d(s[9]);
    return y * 10000u + m * 100u + dd;
}

} // namespace mini2
