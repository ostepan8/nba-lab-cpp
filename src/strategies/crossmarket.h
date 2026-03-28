#pragma once

#include "strategy.h"

namespace nba {

class CrossMarketStrategy : public Strategy {
public:
    ExperimentResult run(const StrategyConfig& config,
                          const DataStore& store,
                          const PlayerIndex& index,
                          const KalshiCache& kalshi,
                                  const PropCache* prop_cache = nullptr) override;
};

} // namespace nba
