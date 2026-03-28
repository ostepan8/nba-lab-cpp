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
    c.models_db_path   = "~/claude-code-linux-harness/data/models.db";
    c.fast_workers     = 6;
    c.slow_workers     = 2;
    c.meanrev_weight     = 0.20;
    c.situational_weight = 0.15;
    c.twostage_weight    = 0.10;
    c.crossmarket_weight = 0.15;
    c.meta_weight        = 0.10;
    c.bayesian_weight    = 0.10;
    c.ml_props_weight    = 0.05;
    c.moneyline_weight   = 0.05;
    c.compound_weight    = 0.03;
    c.residual_weight    = 0.03;
    c.ensemble_weight    = 0.04;
    c.timeseries_weight  = 0.05;
    c.neural_weight      = 0.05;
    c.spreads_weight     = 0.03;
    c.totals_weight      = 0.02;
    c.four_factors_weight = 0.05;
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
    if (j.contains("models_db_path"))     c.models_db_path     = j["models_db_path"].get<std::string>();
    if (j.contains("fast_workers"))       c.fast_workers       = j["fast_workers"].get<int>();
    if (j.contains("slow_workers"))       c.slow_workers       = j["slow_workers"].get<int>();
    if (j.contains("meanrev_weight"))     c.meanrev_weight     = j["meanrev_weight"].get<double>();
    if (j.contains("situational_weight")) c.situational_weight = j["situational_weight"].get<double>();
    if (j.contains("twostage_weight"))    c.twostage_weight    = j["twostage_weight"].get<double>();
    if (j.contains("crossmarket_weight")) c.crossmarket_weight = j["crossmarket_weight"].get<double>();
    if (j.contains("meta_weight"))        c.meta_weight        = j["meta_weight"].get<double>();
    if (j.contains("bayesian_weight"))    c.bayesian_weight    = j["bayesian_weight"].get<double>();
    if (j.contains("ml_props_weight"))    c.ml_props_weight    = j["ml_props_weight"].get<double>();
    if (j.contains("moneyline_weight"))   c.moneyline_weight   = j["moneyline_weight"].get<double>();
    if (j.contains("compound_weight"))    c.compound_weight    = j["compound_weight"].get<double>();
    if (j.contains("residual_weight"))    c.residual_weight    = j["residual_weight"].get<double>();
    if (j.contains("ensemble_weight"))    c.ensemble_weight    = j["ensemble_weight"].get<double>();
    if (j.contains("timeseries_weight"))  c.timeseries_weight  = j["timeseries_weight"].get<double>();
    if (j.contains("neural_weight"))      c.neural_weight      = j["neural_weight"].get<double>();
    if (j.contains("spreads_weight"))     c.spreads_weight     = j["spreads_weight"].get<double>();
    if (j.contains("totals_weight"))      c.totals_weight      = j["totals_weight"].get<double>();
    if (j.contains("four_factors_weight")) c.four_factors_weight = j["four_factors_weight"].get<double>();
    if (j.contains("notify_enabled"))     c.notify_enabled     = j["notify_enabled"].get<bool>();
    if (j.contains("notify_min_roi"))     c.notify_min_roi     = j["notify_min_roi"].get<double>();
    if (j.contains("kalshi_fee_rate"))    c.kalshi_fee_rate    = j["kalshi_fee_rate"].get<double>();

    // Validate workers
    if (c.fast_workers < 1) c.fast_workers = 1;
    if (c.slow_workers < 0) c.slow_workers = 0;

    // Normalize weights: ensure they sum to something positive
    double total = c.meanrev_weight + c.situational_weight + c.twostage_weight
                 + c.crossmarket_weight + c.meta_weight + c.bayesian_weight
                 + c.ml_props_weight + c.moneyline_weight + c.compound_weight
                 + c.residual_weight + c.ensemble_weight
                 + c.timeseries_weight + c.neural_weight + c.spreads_weight
                 + c.totals_weight + c.four_factors_weight;
    if (total < 1e-9) {
        // Reset to defaults
        auto def = defaults();
        c.meanrev_weight     = def.meanrev_weight;
        c.situational_weight = def.situational_weight;
        c.twostage_weight    = def.twostage_weight;
        c.crossmarket_weight = def.crossmarket_weight;
        c.meta_weight        = def.meta_weight;
        c.bayesian_weight    = def.bayesian_weight;
        c.ml_props_weight    = def.ml_props_weight;
        c.moneyline_weight   = def.moneyline_weight;
        c.compound_weight    = def.compound_weight;
        c.residual_weight    = def.residual_weight;
        c.ensemble_weight    = def.ensemble_weight;
        c.timeseries_weight  = def.timeseries_weight;
        c.neural_weight      = def.neural_weight;
        c.spreads_weight     = def.spreads_weight;
        c.totals_weight      = def.totals_weight;
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
    j["models_db_path"]     = models_db_path;
    j["fast_workers"]       = fast_workers;
    j["slow_workers"]       = slow_workers;
    j["meanrev_weight"]     = meanrev_weight;
    j["situational_weight"] = situational_weight;
    j["twostage_weight"]    = twostage_weight;
    j["crossmarket_weight"] = crossmarket_weight;
    j["meta_weight"]        = meta_weight;
    j["bayesian_weight"]    = bayesian_weight;
    j["ml_props_weight"]    = ml_props_weight;
    j["moneyline_weight"]   = moneyline_weight;
    j["compound_weight"]    = compound_weight;
    j["residual_weight"]    = residual_weight;
    j["ensemble_weight"]    = ensemble_weight;
    j["timeseries_weight"]  = timeseries_weight;
    j["neural_weight"]      = neural_weight;
    j["spreads_weight"]     = spreads_weight;
    j["totals_weight"]      = totals_weight;
    j["four_factors_weight"]  = four_factors_weight;
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
    models_db_path = expand_home(models_db_path);
}

} // namespace nba
