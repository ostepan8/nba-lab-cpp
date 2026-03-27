#pragma once

#include "../data/store.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace nba {

struct PlayerStats {
    std::string name;
    int player_id = 0;
    std::vector<std::string> dates;      // sorted game dates
    std::vector<double> pts, reb, ast, fg3m, stl, blk;
    std::vector<double> minutes;
    std::vector<std::string> teams;
    std::vector<std::string> opponents;
    std::vector<bool> is_home;

    // Get stat array by name: "pts", "reb", "ast", "fg3m", "stl", "blk", "minutes"
    const std::vector<double>& get_stat(const std::string& stat) const;

    // Find index for a date using binary search.
    // Returns the index of the last game ON or BEFORE `date`.
    // Returns -1 if no games exist on or before that date.
    int find_date_index(const std::string& date) const;

    // Number of games
    size_t num_games() const { return dates.size(); }
};

class PlayerIndex {
public:
    // Build from loaded DataStore — iterates all players once
    void build(const DataStore& store);

    const PlayerStats* get_by_id(int pid) const;
    const PlayerStats* get_by_name(const std::string& name) const;

    size_t size() const { return by_id_.size(); }
    static std::string normalize_name(const std::string& name);

    // Iterate all players
    const std::unordered_map<int, PlayerStats>& all() const { return by_id_; }

private:
    std::unordered_map<int, PlayerStats> by_id_;
    std::unordered_map<std::string, int> name_to_id_;
    std::unordered_map<std::string, int> normalized_to_id_;  // normalized name → pid
};

} // namespace nba
