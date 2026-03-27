#include "bet_history.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace nba {
namespace bet_history {

// Module-level mutex for file writes
static std::mutex write_mutex_;

void save(const std::string& name, const std::vector<Bet>& bets,
          const std::string& output_dir) {
    if (bets.empty() || name.empty()) return;

    fs::create_directories(output_dir);

    std::string path = output_dir + "/" + name + ".jsonl";

    std::lock_guard<std::mutex> lock(write_mutex_);
    std::ofstream f(path);
    if (!f.is_open()) return;

    for (const auto& b : bets) {
        nlohmann::json j;
        j["date"] = b.date;
        j["player"] = b.player;
        j["stat"] = b.stat;
        j["line"] = b.line;
        j["side"] = b.side;
        j["actual"] = b.actual;
        j["odds"] = b.odds;
        j["won"] = b.won;
        j["pnl"] = b.pnl;
        j["bet_size"] = b.bet_size;
        f << j.dump() << "\n";
    }
}

} // namespace bet_history
} // namespace nba
