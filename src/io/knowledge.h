#pragma once

#include "../strategies/strategy.h"
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <string>

namespace nba {

class KnowledgeBase {
public:
    struct MarketEntry {
        double roi = -999.0;
        double net_roi = -999.0;
        double pvalue = 1.0;
        int bets = 0;
        double wr = 0.0;
        std::string approach;
        nlohmann::json config;
    };

    // Load/save from JSON file
    void load(const std::string& path);
    void save(const std::string& path) const;

    // Update leaderboard if new result is better for this market key.
    // Returns true if leaderboard was updated.
    // Thread-safe.
    bool update(const std::string& market_key, const ExperimentResult& result,
                const StrategyConfig& config);

    // Get a snapshot of the leaderboard (thread-safe)
    std::map<std::string, MarketEntry> get_leaderboard() const;

    // Stats
    int experiments_run() const;
    void increment_experiments(int n = 1);
    double total_runtime_hours() const;
    void add_runtime(double hours);

private:
    mutable std::mutex mutex_;
    std::map<std::string, MarketEntry> leaderboard_;
    int experiments_run_ = 0;
    double total_runtime_hours_ = 0.0;
};

} // namespace nba
