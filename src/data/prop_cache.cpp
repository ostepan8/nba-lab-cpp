#include "prop_cache.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <chrono>

namespace nba {

const std::vector<AggregatedProp> PropCache::empty_;

void PropCache::build(const DataStore& store) {
    auto t0 = std::chrono::high_resolution_clock::now();

    dates_ = store.get_prop_dates();
    std::unordered_set<std::string> market_set;

    auto median = [](std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        if (v.size() == 1) return v[0];
        if (v.size() == 2) return (v[0] + v[1]) / 2.0;
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        return (n % 2 == 0) ? (v[n/2 - 1] + v[n/2]) / 2.0 : v[n/2];
    };

    size_t total_entries = 0;

    for (const auto& date : dates_) {
        const auto& day_props = store.get_props(date);
        if (day_props.empty()) continue;

        // Group by market_type first
        std::unordered_map<std::string, std::vector<const PropLine*>> by_market;
        for (const auto& p : day_props) {
            by_market[p.market_type].push_back(&p);
        }

        for (auto& [market, props] : by_market) {
            market_set.insert(market);

            // Step 1: Group by player name, collect raw values
            struct RawAgg {
                std::string player_name;
                int player_id = 0;
                std::vector<double> lines, over_odds, under_odds;
            };
            std::unordered_map<std::string, RawAgg> player_agg;

            for (const auto* p : props) {
                auto& agg = player_agg[p->player_name];
                agg.player_name = p->player_name;
                if (p->player_id != 0) agg.player_id = p->player_id;
                agg.lines.push_back(p->line);
                agg.over_odds.push_back(p->over_odds);
                agg.under_odds.push_back(p->under_odds);
            }

            // Step 2: Merge by player_id (dedup name variants)
            std::unordered_map<int, size_t> id_to_idx;
            std::vector<RawAgg> merged;

            for (auto& [name, agg] : player_agg) {
                if (agg.player_id != 0 && id_to_idx.count(agg.player_id)) {
                    auto& existing = merged[id_to_idx[agg.player_id]];
                    existing.lines.insert(existing.lines.end(), agg.lines.begin(), agg.lines.end());
                    existing.over_odds.insert(existing.over_odds.end(), agg.over_odds.begin(), agg.over_odds.end());
                    existing.under_odds.insert(existing.under_odds.end(), agg.under_odds.begin(), agg.under_odds.end());
                } else {
                    if (agg.player_id != 0) id_to_idx[agg.player_id] = merged.size();
                    merged.push_back(std::move(agg));
                }
            }

            // Step 3: Compute medians and build final vector
            std::string key = make_key(date, market);
            auto& result = cache_[key];
            result.reserve(merged.size());

            for (auto& agg : merged) {
                AggregatedProp ap;
                ap.player_name = std::move(agg.player_name);
                ap.player_id = agg.player_id;
                ap.line = median(agg.lines);
                ap.over_odds = median(agg.over_odds);
                ap.under_odds = median(agg.under_odds);
                result.push_back(std::move(ap));
            }

            total_entries += result.size();
        }
    }

    markets_.assign(market_set.begin(), market_set.end());
    std::sort(markets_.begin(), markets_.end());

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    printf("  PropCache: %zu date-market combos, %zu player entries (%ldms)\n",
           cache_.size(), total_entries, ms);
}

const std::vector<AggregatedProp>& PropCache::get(const std::string& date,
                                                    const std::string& market) const {
    auto it = cache_.find(make_key(date, market));
    return (it != cache_.end()) ? it->second : empty_;
}

} // namespace nba
