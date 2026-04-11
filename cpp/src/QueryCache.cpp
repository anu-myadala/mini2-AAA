#include "QueryCache.hpp"
#include <functional>

namespace mini2 {

static std::size_t hc(std::size_t seed, std::size_t v) {
    return seed ^ (v + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}

std::size_t QueryCache::hashSpec(const QuerySpec& s) {
    std::size_t h = 0;
    h = hc(h, std::hash<int>{}(static_cast<int>(s.borough())));
    h = hc(h, std::hash<std::string>{}(s.complaint_type()));
    h = hc(h, std::hash<std::uint32_t>{}(s.date_from()));
    h = hc(h, std::hash<std::uint32_t>{}(s.date_to()));
    h = hc(h, std::hash<bool>{}(s.use_geo_box()));
    if (s.use_geo_box()) {
        h = hc(h, std::hash<double>{}(s.min_lat()));
        h = hc(h, std::hash<double>{}(s.max_lat()));
        h = hc(h, std::hash<double>{}(s.min_lon()));
        h = hc(h, std::hash<double>{}(s.max_lon()));
    }
    h = hc(h, std::hash<std::uint32_t>{}(s.max_rows()));
    return h;
}

bool QueryCache::specEqual(const QuerySpec& a, const QuerySpec& b) {
    if (a.borough()        != b.borough())        return false;
    if (a.complaint_type() != b.complaint_type()) return false;
    if (a.date_from()      != b.date_from())      return false;
    if (a.date_to()        != b.date_to())        return false;
    if (a.use_geo_box()    != b.use_geo_box())    return false;
    if (a.max_rows()       != b.max_rows())       return false;
    if (a.use_geo_box()) {
        if (a.min_lat() != b.min_lat()) return false;
        if (a.max_lat() != b.max_lat()) return false;
        if (a.min_lon() != b.min_lon()) return false;
        if (a.max_lon() != b.max_lon()) return false;
    }
    return true;
}

QueryCache::QueryCache(std::size_t capacity, std::size_t max_rows_per_entry)
    : capacity_(capacity), max_rows_(max_rows_per_entry) {}

std::shared_ptr<const std::vector<Row311>> QueryCache::get(const QuerySpec& spec) {
    if (capacity_ == 0) { misses_.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    std::size_t h = hashSpec(spec);
    std::lock_guard<std::mutex> lk(mu_);
    auto range = index_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        if (specEqual(it->second->spec, spec)) {
            lru_.splice(lru_.begin(), lru_, it->second);
            hits_.fetch_add(1, std::memory_order_relaxed);
            return it->second->rows;
        }
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void QueryCache::put(const QuerySpec& spec, std::vector<Row311> rows) {
    if (capacity_ == 0 || rows.size() > max_rows_) return;
    std::size_t h = hashSpec(spec);
    auto shared = std::make_shared<const std::vector<Row311>>(std::move(rows));
    std::lock_guard<std::mutex> lk(mu_);

    auto range = index_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        if (specEqual(it->second->spec, spec)) {
            it->second->rows = shared;
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }
    }

    if (lru_.size() >= capacity_) {
        auto back = std::prev(lru_.end());
        auto er = index_.equal_range(back->hash);
        for (auto it = er.first; it != er.second; ++it) {
            if (it->second == back) { index_.erase(it); break; }
        }
        lru_.pop_back();
    }

    lru_.push_front({h, spec, shared});
    index_.emplace(h, lru_.begin());
}

std::size_t QueryCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

} // namespace mini2
