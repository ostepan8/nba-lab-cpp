#pragma once

#include "../strategies/strategy.h"
#include "../config/config.h"
#include "../data/store.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include "../io/knowledge.h"
#include "hypothesis.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <string>

namespace nba {

class Lab {
public:
    Lab(const DataStore& store, const PlayerIndex& index,
        const KalshiCache& kalshi, const LabConfig& config);

    // Run forever -- generates and runs experiments in parallel.
    // Respects the running_ flag for graceful shutdown.
    void run();

    // Run a single experiment
    void run_single(const StrategyConfig& config);

    // Benchmark: run N experiments, report throughput
    void bench(int n = 100);

    // Print the current leaderboard from the knowledge base
    void print_leaderboard() const;

    // Request graceful shutdown (called by signal handler)
    void request_stop();

    // Check if the lab is still running
    bool is_running() const { return running_.load(); }

private:
    const DataStore& store_;
    const PlayerIndex& index_;
    const KalshiCache& kalshi_;
    LabConfig config_;

    std::unique_ptr<Strategy> create_strategy(const std::string& type);
    void evaluate_result(const StrategyConfig& cfg, const ExperimentResult& result);
    void log_experiment(const StrategyConfig& cfg, const ExperimentResult& result);

    std::atomic<int> experiments_run_{0};
    std::atomic<bool> running_{true};
    std::mutex log_mutex_;

    KnowledgeBase knowledge_;
    HypothesisGenerator hypothesis_gen_;
};

} // namespace nba
