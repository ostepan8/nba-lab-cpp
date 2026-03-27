#include "config.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

namespace nba {

static std::string expand_home(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

LabConfig LabConfig::defaults() {
    LabConfig c;
    c.data_dir         = "~/Desktop/nba-modeling/data/raw";
    c.kalshi_dir       = "~/Desktop/nba-modeling/data/raw/kalshi";
    c.output_dir       = "~/Desktop/nba-modeling/results";
    c.knowledge_path   = "~/Desktop/nba-modeling/results/lab/knowledge_cpp.json";
    c.notify_script    = "~/claude-code-linux-harness/notify.sh";
    c.fast_workers     = 6;
    c.slow_workers     = 2;
    c.meanrev_weight   = 0.40;
    c.situational_weight = 0.30;
    c.twostage_weight  = 0.30;
    c.notify_enabled   = true;
    c.notify_min_roi   = 0.0;
    c.kalshi_fee_rate  = 0.038;
    return c;
}

LabConfig LabConfig::load(const std::string& path) {
    LabConfig c = defaults();

    if (!fs::exists(path)) {
        fprintf(stderr, "Config file not found: %s (using defaults)\n", path.c_str());
        return c;
    }

    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open config file: %s (using defaults)\n", path.c_str());
        return c;
    }

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        fprintf(stderr, "Config parse error in %s: %s (using defaults)\n",
                path.c_str(), e.what());
        return c;
    }

    // Read each field, falling back to default if missing
    if (j.contains("data_dir"))           c.data_dir           = j["data_dir"].get<std::string>();
    if (j.contains("kalshi_dir"))         c.kalshi_dir         = j["kalshi_dir"].get<std::string>();
    if (j.contains("output_dir"))         c.output_dir         = j["output_dir"].get<std::string>();
    if (j.contains("knowledge_path"))     c.knowledge_path     = j["knowledge_path"].get<std::string>();
    if (j.contains("notify_script"))      c.notify_script      = j["notify_script"].get<std::string>();
    if (j.contains("fast_workers"))       c.fast_workers       = j["fast_workers"].get<int>();
    if (j.contains("slow_workers"))       c.slow_workers       = j["slow_workers"].get<int>();
    if (j.contains("meanrev_weight"))     c.meanrev_weight     = j["meanrev_weight"].get<double>();
    if (j.contains("situational_weight")) c.situational_weight = j["situational_weight"].get<double>();
    if (j.contains("twostage_weight"))    c.twostage_weight    = j["twostage_weight"].get<double>();
    if (j.contains("notify_enabled"))     c.notify_enabled     = j["notify_enabled"].get<bool>();
    if (j.contains("notify_min_roi"))     c.notify_min_roi     = j["notify_min_roi"].get<double>();
    if (j.contains("kalshi_fee_rate"))    c.kalshi_fee_rate    = j["kalshi_fee_rate"].get<double>();

    // Validate workers
    if (c.fast_workers < 1) c.fast_workers = 1;
    if (c.slow_workers < 0) c.slow_workers = 0;

    // Normalize weights
    double total = c.meanrev_weight + c.situational_weight + c.twostage_weight;
    if (total < 1e-9) {
        c.meanrev_weight = 0.40;
        c.situational_weight = 0.30;
        c.twostage_weight = 0.30;
    }

    printf("Config loaded from: %s\n", path.c_str());
    return c;
}

void LabConfig::save(const std::string& path) const {
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }

    nlohmann::json j;
    j["data_dir"]           = data_dir;
    j["kalshi_dir"]         = kalshi_dir;
    j["output_dir"]         = output_dir;
    j["knowledge_path"]     = knowledge_path;
    j["notify_script"]      = notify_script;
    j["fast_workers"]       = fast_workers;
    j["slow_workers"]       = slow_workers;
    j["meanrev_weight"]     = meanrev_weight;
    j["situational_weight"] = situational_weight;
    j["twostage_weight"]    = twostage_weight;
    j["notify_enabled"]     = notify_enabled;
    j["notify_min_roi"]     = notify_min_roi;
    j["kalshi_fee_rate"]    = kalshi_fee_rate;

    std::ofstream f(path);
    if (f.is_open()) {
        f << j.dump(2) << "\n";
    }
}

void LabConfig::expand_paths() {
    data_dir       = expand_home(data_dir);
    kalshi_dir     = expand_home(kalshi_dir);
    output_dir     = expand_home(output_dir);
    knowledge_path = expand_home(knowledge_path);
    notify_script  = expand_home(notify_script);
}

} // namespace nba
