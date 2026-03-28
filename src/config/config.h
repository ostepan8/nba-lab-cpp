#pragma once

#include <string>

namespace nba {

struct LabConfig {
    // Paths
    std::string data_dir;
    std::string kalshi_dir;
    std::string output_dir;
    std::string knowledge_path;
    std::string notify_script;

    // Workers
    int fast_workers = 6;
    int slow_workers = 2;

    // Experiment generation weights
    double meanrev_weight = 0.20;
    double situational_weight = 0.15;
    double twostage_weight = 0.10;
    double crossmarket_weight = 0.15;
    double meta_weight = 0.10;
    double bayesian_weight = 0.10;
    double ml_props_weight = 0.05;
    double moneyline_weight = 0.05;
    double compound_weight = 0.03;
    double residual_weight = 0.03;
    double ensemble_weight = 0.04;
    double timeseries_weight = 0.05;
    double neural_weight = 0.05;
    double spreads_weight = 0.03;
    double totals_weight = 0.02;

    // Notification
    bool notify_enabled = true;
    double notify_min_roi = 0.0;

    // Kalshi fee
    double kalshi_fee_rate = 0.038;
    double max_runtime_seconds = 0;  // 0 = run forever

    // Load config from a JSON file. Returns defaults() on any error.
    static LabConfig load(const std::string& path);

    // Return a config with hardcoded defaults.
    static LabConfig defaults();

    // Write the current config to a JSON file.
    void save(const std::string& path) const;

    // Expand ~ to $HOME in all path fields (in-place).
    void expand_paths();
};

} // namespace nba
