#pragma once

#include "../strategies/strategy.h"
#include "../config/config.h"
#include <random>
#include <mutex>
#include <string>
#include <vector>

namespace nba {

// Thread-safe hypothesis generator for experiment configs.
class HypothesisGenerator {
public:
    explicit HypothesisGenerator(const LabConfig& config);

    // Generate a random experiment config using the configured weights.
    StrategyConfig generate(const std::string& queue_type = "fast");

private:
    LabConfig config_;
    std::mt19937 rng_;
    std::mutex rng_mutex_;

    double rand_double(double lo, double hi);
    int rand_int(int lo, int hi);

    StrategyConfig generate_meanrev();
    StrategyConfig generate_situational();
    StrategyConfig generate_twostage();
    StrategyConfig generate_crossmarket();
    StrategyConfig generate_meta_ensemble();
    StrategyConfig generate_bayesian();
    StrategyConfig generate_ml_props();
    StrategyConfig generate_moneyline();
    StrategyConfig generate_compound();
    StrategyConfig generate_residual();
    StrategyConfig generate_ensemble();
};

} // namespace nba
