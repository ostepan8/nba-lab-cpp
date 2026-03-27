#pragma once

#include "../strategies/strategy.h"
#include "../data/store.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include "../io/knowledge.h"
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

    // Run forever -- generates and runs experiments in parallel
    void run();

    // Run a single experiment
    void run_single(const StrategyConfig& config);

    // Benchmark: run N experiments across all strategy types, report throughput
    void bench(int n = 100);

    // Generate a random hypothesis for the given queue type
    StrategyConfig generate_hypothesis(const std::string& queue_type);

    // Print the current leaderboard from the knowledge base
    void print_leaderboard() const;

    // Set the knowledge base path (for persistence)
    void set_knowledge_path(const std::string& path);

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

    // Persistent knowledge base
    KnowledgeBase knowledge_;
    std::string knowledge_path_;

    // RNG per thread would be better, but for hypothesis generation
    // we protect with mutex
    std::mt19937 rng_;
    std::mutex rng_mutex_;

    // Helpers
    double rand_double(double lo, double hi);
    int rand_int(int lo, int hi);
    std::string rand_choice(const std::vector<std::string>& v);

    // Generate configs for specific strategy types
    StrategyConfig generate_meanrev_config();
    StrategyConfig generate_situational_config();
    StrategyConfig generate_twostage_config();
};

} // namespace nba
