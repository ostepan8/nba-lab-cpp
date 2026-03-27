#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

namespace nba {

class DataStore {
public:
    // Load gamelogs, props, and odds from data_dir
    // Expects: data_dir/player_gamelog_*.csv
    //          data_dir/player_props/props_*.csv
    //          data_dir/odds/odds_*.csv
    void load_all(const std::string& data_dir);

    // Gamelogs indexed by player_id, sorted by date
    const std::vector<PlayerGame>& get_player_games(int player_id) const;
    const std::vector<PlayerGame>& get_player_games_by_name(const std::string& name) const;

    // Props by date
    const std::vector<PropLine>& get_props(const std::string& date) const;
    std::vector<std::string> get_prop_dates() const; // sorted

    // Odds by date
    const std::vector<OddsLine>& get_odds(const std::string& date) const;

    // Stats
    size_t num_players() const;
    size_t num_prop_dates() const;
    size_t num_games() const;

private:
    std::unordered_map<int, std::vector<PlayerGame>> games_by_pid_;
    std::unordered_map<std::string, std::vector<PlayerGame>> games_by_name_;
    std::map<std::string, std::vector<PropLine>> props_by_date_;
    std::map<std::string, std::vector<OddsLine>> odds_by_date_;
    size_t total_games_ = 0;

    static const std::vector<PlayerGame> empty_games_;
    static const std::vector<PropLine> empty_props_;
    static const std::vector<OddsLine> empty_odds_;
};

} // namespace nba
