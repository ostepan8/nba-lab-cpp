#pragma once

#include "../data/types.h"
#include <string>
#include <vector>

namespace nba {
namespace bet_history {

// Save bet history as JSONL file.
// Each line: {"date","player","stat","line","actual","odds","won","pnl","bet_size","side"}
void save(const std::string& name, const std::vector<Bet>& bets,
          const std::string& output_dir);

} // namespace bet_history
} // namespace nba
