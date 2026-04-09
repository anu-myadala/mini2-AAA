// QueryEngine.cpp - applies a QuerySpec column-by-column with one short pass.
#include "QueryEngine.hpp"

#include <cstdint>
#include <utility>

namespace mini2 {

namespace {

// Translate the proto enum into our local one. They share the same numeric
// values by design (see overlay.proto + Record311.hpp).
inline Borough fromProto(BoroughEnum b) {
    return static_cast<Borough>(static_cast<uint8_t>(b));
}
inline BoroughEnum toProto(Borough b) {
    return static_cast<BoroughEnum>(static_cast<int>(b));
}

} // namespace

void QueryEngine::run(
    const QuerySpec& spec,
    std::uint32_t chunk_rows,
    const std::function<bool()>& is_canceled,
    const std::function<void(std::vector<Row311>&&, bool done)>& sink) const
{
    if (chunk_rows == 0) chunk_rows = 512;

    const auto& keys      = store_.keys();
    const auto& dates     = store_.dates();
    const auto& boros     = store_.boroughs();
    const auto& lats      = store_.lats();
    const auto& lons      = store_.lons();
    const auto& complaints = store_.complaints();
    const std::size_t n = keys.size();

    const Borough want_b   = (spec.borough() == B_UNSPECIFIED)
                                ? Borough::UNSPECIFIED
                                : fromProto(spec.borough());
    const bool any_borough = (spec.borough() == B_UNSPECIFIED);
    const bool any_complaint = spec.complaint_type().empty();
    const std::uint32_t df = spec.date_from();
    const std::uint32_t dt = spec.date_to();
    const bool use_geo = spec.use_geo_box();
    const double mnLat = spec.min_lat(), mxLat = spec.max_lat();
    const double mnLon = spec.min_lon(), mxLon = spec.max_lon();
    const std::uint32_t cap = spec.max_rows();

    std::vector<Row311> chunk;
    chunk.reserve(chunk_rows); // address mini1 critique about vector growth
    std::uint32_t produced = 0;

    for (std::size_t i = 0; i < n; ++i) {
        if ((i & 0x3FFu) == 0 && is_canceled()) break;

        if (!any_borough && boros[i] != want_b) continue;
        if (df != 0 && dates[i] < df) continue;
        if (dt != 0 && dates[i] > dt) continue;
        if (use_geo) {
            const double la = lats[i], lo = lons[i];
            if (la < mnLat || la > mxLat || lo < mnLon || lo > mxLon) continue;
        }
        if (!any_complaint && complaints[i] != spec.complaint_type()) continue;

        Row311 r;
        r.set_unique_key(keys[i]);
        r.set_created_ymd(dates[i]);
        r.set_borough(toProto(boros[i]));
        r.set_latitude(lats[i]);
        r.set_longitude(lons[i]);
        r.set_complaint_type(complaints[i]);
        chunk.emplace_back(std::move(r));

        ++produced;
        if (cap != 0 && produced >= cap) break;

        if (chunk.size() >= chunk_rows) {
            sink(std::move(chunk), false);
            chunk.clear();
            chunk.reserve(chunk_rows);
        }
    }

    // Final emit (may be empty); always set done=true.
    sink(std::move(chunk), true);
}

} // namespace mini2
