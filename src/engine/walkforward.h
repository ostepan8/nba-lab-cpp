#pragma once

#include "../strategies/strategy.h"
#include "../data/store.h"
#include "../data/prop_cache.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include <functional>
#include <optional>
#include <string>

namespace nba {

class WalkforwardRunner {
public:
    WalkforwardRunner(const DataStore& store, const PlayerIndex& index,
                      const KalshiCache& kalshi,
                      const PropCache* prop_cache = nullptr);

    // Callback invoked for each (player, date, line, odds) combo.
    // Return a Bet to record it, or nullopt to skip.
    using BetCallback = std::function<std::optional<Bet>(
        const PlayerStats& player, int date_idx,
        double line, double over_odds_ml, double under_odds_ml,
        const std::string& date)>;

    // Walk forward over all prop dates for the given market, call the callback
    // for each player-prop encountered, accumulate results.
    ExperimentResult run(const StrategyConfig& config, BetCallback callback);

private:
    const DataStore& store_;
    const PlayerIndex& index_;
    const KalshiCache& kalshi_;
    const PropCache* prop_cache_ = nullptr;  // optional, for pre-computed props
};

} // namespace nba
