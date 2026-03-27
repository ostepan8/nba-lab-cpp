#include "store.h"
#include "csv_parser.h"
#include <algorithm>
#include <cstdio>

namespace nba {

const std::vector<PlayerGame> DataStore::empty_games_;
const std::vector<PropLine> DataStore::empty_props_;
const std::vector<OddsLine> DataStore::empty_odds_;

void DataStore::load_all(const std::string& data_dir) {
    // --- Gamelogs ---
    auto all_games = parse_gamelogs(data_dir);
    total_games_ = all_games.size();

    for (auto& g : all_games) {
        games_by_pid_[g.player_id].push_back(g);
        games_by_name_[g.player_name].push_back(g);
    }

    // Sort each player's games by date
    for (auto& [pid, games] : games_by_pid_) {
        std::sort(games.begin(), games.end(),
            [](const PlayerGame& a, const PlayerGame& b) {
                return a.game_date < b.game_date;
            });
    }
    for (auto& [name, games] : games_by_name_) {
        std::sort(games.begin(), games.end(),
            [](const PlayerGame& a, const PlayerGame& b) {
                return a.game_date < b.game_date;
            });
    }

    printf("  Gamelogs: %zu rows, %zu unique players\n",
           total_games_, games_by_pid_.size());

    // --- Props ---
    std::string props_dir = data_dir + "/player_props";
    props_by_date_ = parse_props(props_dir);

    size_t total_props = 0;
    for (auto& [date, props] : props_by_date_)
        total_props += props.size();
    printf("  Props: %zu rows across %zu dates\n",
           total_props, props_by_date_.size());

    // --- Odds ---
    std::string odds_dir = data_dir + "/odds";
    odds_by_date_ = parse_odds(odds_dir);

    size_t total_odds = 0;
    for (auto& [date, odds] : odds_by_date_)
        total_odds += odds.size();
    printf("  Odds: %zu rows across %zu dates\n",
           total_odds, odds_by_date_.size());
}

const std::vector<PlayerGame>& DataStore::get_player_games(int player_id) const {
    auto it = games_by_pid_.find(player_id);
    if (it != games_by_pid_.end()) return it->second;
    return empty_games_;
}

const std::vector<PlayerGame>& DataStore::get_player_games_by_name(const std::string& name) const {
    auto it = games_by_name_.find(name);
    if (it != games_by_name_.end()) return it->second;
    return empty_games_;
}

const std::vector<PropLine>& DataStore::get_props(const std::string& date) const {
    auto it = props_by_date_.find(date);
    if (it != props_by_date_.end()) return it->second;
    return empty_props_;
}

std::vector<std::string> DataStore::get_prop_dates() const {
    std::vector<std::string> dates;
    dates.reserve(props_by_date_.size());
    for (auto& [date, _] : props_by_date_)
        dates.push_back(date);
    return dates; // already sorted (std::map)
}

const std::vector<OddsLine>& DataStore::get_odds(const std::string& date) const {
    auto it = odds_by_date_.find(date);
    if (it != odds_by_date_.end()) return it->second;
    return empty_odds_;
}

size_t DataStore::num_players() const {
    return games_by_pid_.size();
}

size_t DataStore::num_prop_dates() const {
    return props_by_date_.size();
}

size_t DataStore::num_games() const {
    return total_games_;
}

} // namespace nba
