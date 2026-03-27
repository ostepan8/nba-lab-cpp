#include "knowledge.h"
#include <fstream>
#include <filesystem>
#include <cstdio>

namespace fs = std::filesystem;

namespace nba {

void KnowledgeBase::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fs::exists(path)) {
        printf("  KnowledgeBase: no file at %s, starting fresh\n", path.c_str());
        return;
    }

    std::ifstream f(path);
    if (!f.is_open()) return;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        printf("  KnowledgeBase: parse error in %s: %s\n", path.c_str(), e.what());
        return;
    }

    if (j.contains("experiments_run"))
        experiments_run_ = j["experiments_run"].get<int>();
    if (j.contains("total_runtime_hours"))
        total_runtime_hours_ = j["total_runtime_hours"].get<double>();

    if (j.contains("leaderboard") && j["leaderboard"].is_object()) {
        for (auto& [key, val] : j["leaderboard"].items()) {
            MarketEntry entry;
            if (val.contains("roi"))       entry.roi = val["roi"].get<double>();
            if (val.contains("net_roi"))   entry.net_roi = val["net_roi"].get<double>();
            if (val.contains("pvalue"))    entry.pvalue = val["pvalue"].get<double>();
            if (val.contains("bets"))      entry.bets = val["bets"].get<int>();
            if (val.contains("wr"))        entry.wr = val["wr"].get<double>();
            if (val.contains("approach"))  entry.approach = val["approach"].get<std::string>();
            if (val.contains("config"))    entry.config = val["config"];
            leaderboard_[key] = entry;
        }
    }

    printf("  KnowledgeBase: loaded %zu market entries, %d experiments total\n",
           leaderboard_.size(), experiments_run_);
}

void KnowledgeBase::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Ensure parent directory exists
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }

    nlohmann::json j;
    j["experiments_run"] = experiments_run_;
    j["total_runtime_hours"] = total_runtime_hours_;

    nlohmann::json lb = nlohmann::json::object();
    for (const auto& [key, entry] : leaderboard_) {
        nlohmann::json ej;
        ej["roi"] = entry.roi;
        ej["net_roi"] = entry.net_roi;
        ej["pvalue"] = entry.pvalue;
        ej["bets"] = entry.bets;
        ej["wr"] = entry.wr;
        ej["approach"] = entry.approach;
        ej["config"] = entry.config;
        lb[key] = ej;
    }
    j["leaderboard"] = lb;

    std::ofstream f(path);
    if (f.is_open()) {
        f << j.dump(2) << "\n";
    }
}

bool KnowledgeBase::update(const std::string& market_key,
                            const ExperimentResult& result,
                            const StrategyConfig& config) {
    if (result.total_bets < 20) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = leaderboard_.find(market_key);
    bool is_better = false;

    if (it == leaderboard_.end()) {
        // No entry yet — any result with reasonable p-value qualifies
        is_better = (result.pvalue < 0.15);
    } else {
        // Beat existing ROI with reasonable p-value
        is_better = (result.roi > it->second.roi && result.pvalue < 0.15);
    }

    if (is_better) {
        MarketEntry entry;
        entry.roi = result.roi;
        entry.net_roi = result.roi;  // same for now; could differ with commission
        entry.pvalue = result.pvalue;
        entry.bets = result.total_bets;
        entry.wr = result.win_rate;
        entry.approach = config.type;
        entry.config = config.to_json();
        leaderboard_[market_key] = entry;
        return true;
    }

    return false;
}

std::map<std::string, KnowledgeBase::MarketEntry> KnowledgeBase::get_leaderboard() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return leaderboard_;
}

int KnowledgeBase::experiments_run() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return experiments_run_;
}

void KnowledgeBase::increment_experiments(int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    experiments_run_ += n;
}

double KnowledgeBase::total_runtime_hours() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_runtime_hours_;
}

void KnowledgeBase::add_runtime(double hours) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_runtime_hours_ += hours;
}

} // namespace nba
