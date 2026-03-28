#pragma once

#include "types.h"
#include "store.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace nba {

/// Pre-aggregated prop line: median line/odds across bookmakers, deduped by player_id.
struct AggregatedProp {
    std::string player_name;
    int player_id = 0;
    double line = 0.0;
    double over_odds = 0.0;
    double under_odds = 0.0;
};

/// Pre-computed prop cache. Built once at startup from the DataStore.
/// Eliminates per-experiment aggregation (groupby + median + dedup) from the walkforward.
class PropCache {
public:
    /// Build the cache from a DataStore. Call once at startup.
    void build(const DataStore& store);

    /// Get pre-aggregated props for a (date, market) pair.
    /// Returns empty vector if no props exist.
    const std::vector<AggregatedProp>& get(const std::string& date,
                                            const std::string& market) const;

    /// All dates that have props (sorted).
    const std::vector<std::string>& dates() const { return dates_; }

    /// All markets seen.
    const std::vector<std::string>& markets() const { return markets_; }

    size_t size() const { return cache_.size(); }

private:
    // Key: "date|market" → vector of aggregated props
    std::unordered_map<std::string, std::vector<AggregatedProp>> cache_;
    std::vector<std::string> dates_;
    std::vector<std::string> markets_;

    static const std::vector<AggregatedProp> empty_;

    static std::string make_key(const std::string& date, const std::string& market) {
        return date + "|" + market;
    }
};

} // namespace nba
