#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <map>

namespace nba {

// Split a CSV line handling quoted fields
std::vector<std::string> split_csv_line(const std::string& line);

// Safe double parse: empty/invalid → 0.0
double safe_double(const std::string& s);

// Safe int parse: empty/invalid → 0
int safe_int(const std::string& s);

// Parse all player_gamelog_*.csv files in data_dir
std::vector<PlayerGame> parse_gamelogs(const std::string& data_dir);

// Parse all props_*.csv files in data_dir/player_props/
std::map<std::string, std::vector<PropLine>> parse_props(const std::string& dir_path);

// Parse all odds_*.csv files in data_dir/odds/
std::map<std::string, std::vector<OddsLine>> parse_odds(const std::string& dir_path);

} // namespace nba
