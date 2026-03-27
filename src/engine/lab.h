#pragma once

#include "../strategies/strategy.h"
#include "../data/store.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <random>

namespace nba {

class Lab {
public:
    Lab(const DataStore& store, const PlayerIndex& index,
        const KalshiCache& kalshi,
        int fast_workers = 6, int slow_workers = 2);

    // Run forever — generates and runs experiments in parallel
    void run();

    // Run a single experiment
    void run_single(const StrategyConfig& config);

    // Benchmark: run N meanrev experiments, report throughput
    void bench(int n = 100);

    // Generate a random hypothesis for the given queue type
    StrategyConfig generate_hypothesis(const std::string& queue_type);

private:
    const DataStore& store_;
    const PlayerIndex& index_;
    const KalshiCache& kalshi_;
    int fast_workers_;
    int slow_workers_;

    std::unique_ptr<Strategy> create_strategy(const std::string& type);
    void evaluate_result(const StrategyConfig& config, const ExperimentResult& result);
    void log_experiment(const StrategyConfig& config, const ExperimentResult& result);

    std::atomic<int> experiments_run_{0};
    std::mutex log_mutex_;

    // Knowledge base: best result per market
    struct MarketBest {
        double roi = -999.0;
        double pvalue = 1.0;
        int bets = 0;
        StrategyConfig config;
    };
    std::map<std::string, MarketBest> leaderboard_;
    std::mutex leaderboard_mutex_;

    // RNG per thread would be better, but for hypothesis generation
    // we protect with mutex
    std::mt19937 rng_;
    std::mutex rng_mutex_;

    // Helpers
    double rand_double(double lo, double hi);
    int rand_int(int lo, int hi);
    std::string rand_choice(const std::vector<std::string>& v);
};

} // namespace nba
