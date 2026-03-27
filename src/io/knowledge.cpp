#include "knowledge.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

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
    try { f >> j; } catch (...) { return; }

    if (j.contains("experiments_run"))
        experiments_run_ = j["experiments_run"].get<int>();
    if (j.contains("total_runtime_hours"))
        total_runtime_hours_ = j["total_runtime_hours"].get<double>();

    if (j.contains("all_proven") && j["all_proven"].is_array()) {
        for (auto& val : j["all_proven"]) {
            ProvenConfig pc;
            if (val.contains("market"))    pc.market = val["market"].get<std::string>();
            if (val.contains("approach"))  pc.approach = val["approach"].get<std::string>();
            if (val.contains("name"))      pc.name = val["name"].get<std::string>();
            if (val.contains("roi"))       pc.roi = val["roi"].get<double>();
            if (val.contains("net_roi"))   pc.net_roi = val["net_roi"].get<double>();
            if (val.contains("pvalue"))    pc.pvalue = val["pvalue"].get<double>();
            if (val.contains("bets"))      pc.bets = val["bets"].get<int>();
            if (val.contains("wr"))        pc.wr = val["wr"].get<double>();
            if (val.contains("config"))    pc.config = val["config"];
            if (val.contains("timestamp")) pc.timestamp = val["timestamp"].get<std::string>();
            all_proven_.push_back(pc);
        }
    }

    // Initialize prev tops so first run doesn't spam
    auto roi_top = top_by_roi(5);
    auto sig_top = top_by_significance(5);
    for (auto& e : roi_top) prev_top_roi_names_.insert(e.name);
    for (auto& e : sig_top) prev_top_sig_names_.insert(e.name);

    printf("  KnowledgeBase: %zu proven configs, %d experiments\n",
           all_proven_.size(), experiments_run_);
}

void KnowledgeBase::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());

    nlohmann::json j;
    j["experiments_run"] = experiments_run_;
    j["total_runtime_hours"] = total_runtime_hours_;

    nlohmann::json arr = nlohmann::json::array();
    for (auto& pc : all_proven_) {
        nlohmann::json ej;
        ej["market"] = pc.market;
        ej["approach"] = pc.approach;
        ej["name"] = pc.name;
        ej["roi"] = pc.roi;
        ej["net_roi"] = pc.net_roi;
        ej["pvalue"] = pc.pvalue;
        ej["bets"] = pc.bets;
        ej["wr"] = pc.wr;
        ej["config"] = pc.config;
        ej["timestamp"] = pc.timestamp;
        arr.push_back(ej);
    }
    j["all_proven"] = arr;

    std::ofstream f(path);
    if (f.is_open()) f << j.dump(2) << "\n";
}

bool KnowledgeBase::add_proven(const ProvenConfig& entry) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Always save
    all_proven_.push_back(entry);

    // Check if this enters either top 5
    // Temporarily unlock-relock pattern not needed since we hold the lock
    // and top_by_* are const but need the lock — call internal versions

    // Top by ROI
    auto sorted_roi = all_proven_;
    std::sort(sorted_roi.begin(), sorted_roi.end(),
        [](const ProvenConfig& a, const ProvenConfig& b) { return a.net_roi > b.net_roi; });

    // Top by significance
    auto sorted_sig = all_proven_;
    std::sort(sorted_sig.begin(), sorted_sig.end(),
        [](const ProvenConfig& a, const ProvenConfig& b) { return a.pvalue < b.pvalue; });

    std::set<std::string> new_roi_names, new_sig_names;
    for (int i = 0; i < 5 && i < (int)sorted_roi.size(); i++)
        new_roi_names.insert(sorted_roi[i].name);
    for (int i = 0; i < 5 && i < (int)sorted_sig.size(); i++)
        new_sig_names.insert(sorted_sig[i].name);

    bool changed = (new_roi_names != prev_top_roi_names_) ||
                   (new_sig_names != prev_top_sig_names_);

    if (changed) {
        prev_top_roi_names_ = new_roi_names;
        prev_top_sig_names_ = new_sig_names;
    }
    return changed;
}

std::vector<ProvenConfig> KnowledgeBase::top_by_roi(int n) const {
    auto sorted = all_proven_;
    std::sort(sorted.begin(), sorted.end(),
        [](const ProvenConfig& a, const ProvenConfig& b) { return a.net_roi > b.net_roi; });
    if ((int)sorted.size() > n) sorted.resize(n);
    return sorted;
}

std::vector<ProvenConfig> KnowledgeBase::top_by_significance(int n) const {
    auto sorted = all_proven_;
    std::sort(sorted.begin(), sorted.end(),
        [](const ProvenConfig& a, const ProvenConfig& b) { return a.pvalue < b.pvalue; });
    if ((int)sorted.size() > n) sorted.resize(n);
    return sorted;
}

const std::vector<ProvenConfig>& KnowledgeBase::all_proven() const {
    return all_proven_;
}

std::map<std::string, ProvenConfig> KnowledgeBase::best_per_market() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, ProvenConfig> best;
    for (auto& pc : all_proven_) {
        auto it = best.find(pc.market);
        if (it == best.end() || pc.net_roi > it->second.net_roi)
            best[pc.market] = pc;
    }
    return best;
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
