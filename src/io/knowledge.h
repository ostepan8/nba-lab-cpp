#pragma once

#include "../strategies/strategy.h"
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nba {

struct ProvenConfig {
    std::string market;
    std::string approach;
    std::string name;
    double roi = 0;
    double net_roi = 0;
    double pvalue = 1.0;
    int bets = 0;
    double wr = 0;
    nlohmann::json config;
    std::string timestamp;
};

class KnowledgeBase {
public:
    void load(const std::string& path);
    void save(const std::string& path) const;

    // Add a proven config (p < 0.05, net_roi > 0). Never deleted.
    // Returns true if it entered either top 5 (triggers notification).
    bool add_proven(const ProvenConfig& entry);

    // Get top 5 by ROI (highest net ROI, p < 0.05)
    std::vector<ProvenConfig> top_by_roi(int n = 5) const;

    // Get top 5 by significance (lowest p-value, net ROI > 0)
    std::vector<ProvenConfig> top_by_significance(int n = 5) const;

    // All proven configs (never deleted)
    const std::vector<ProvenConfig>& all_proven() const;

    // Best per market (for quick lookup)
    std::map<std::string, ProvenConfig> best_per_market() const;

    // Stats
    int experiments_run() const;
    void increment_experiments(int n = 1);
    double total_runtime_hours() const;
    void add_runtime(double hours);

private:
    mutable std::mutex mutex_;
    std::vector<ProvenConfig> all_proven_;
    std::set<std::string> prev_top_roi_names_;
    std::set<std::string> prev_top_sig_names_;
    int experiments_run_ = 0;
    double total_runtime_hours_ = 0.0;
};

} // namespace nba
