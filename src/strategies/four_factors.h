#pragma once

#include "strategy.h"

namespace nba {

/// Four Factors strategy for game-level betting (spreads, totals, moneyline).
/// Uses Dean Oliver's four factors of basketball success:
///   1. Effective FG% (shooting efficiency)
///   2. Turnover rate (ball security)
///   3. Offensive rebound rate (second chances)
///   4. Free throw rate (getting to the line)
/// Computes rolling offensive + defensive ratings from these factors,
/// predicts point differential and total, bets when model disagrees with market.
class FourFactorsStrategy : public Strategy {
public:
    ExperimentResult run(const StrategyConfig& config,
                          const DataStore& store,
                          const PlayerIndex& index,
                          const KalshiCache& kalshi,
                          const PropCache* prop_cache = nullptr,
                          const GameCache* game_cache = nullptr) override;
};

} // namespace nba
