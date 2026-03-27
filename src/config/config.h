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

    // Experiment generation weights (must sum to 1.0)
    double meanrev_weight = 0.40;
    double situational_weight = 0.30;
    double twostage_weight = 0.30;

    // Notification
    bool notify_enabled = true;
    double notify_min_roi = 0.0;

    // Kalshi fee
    double kalshi_fee_rate = 0.038;

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
