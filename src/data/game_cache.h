#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace nba {

/// Result of a single NBA game from one team's perspective.
struct GameResult {
    std::string date;
    std::string game_id;
    std::string team_abbr;      // e.g. "BOS"
    std::string opponent_abbr;  // e.g. "DAL"
    bool is_home = false;
    bool won = false;
    int pts = 0;                // this team's score
    int opp_pts = 0;            // opponent's score
    int margin = 0;             // pts - opp_pts (positive = won by X)
    double plus_minus = 0.0;
    // Box score stats (for advanced features)
    int fgm = 0, fga = 0, fg3m = 0, fg3a = 0;
    int ftm = 0, fta = 0;
    int oreb = 0, dreb = 0, reb = 0;
    int ast = 0, stl = 0, blk = 0, tov = 0;
};

/// Pre-computed game results cache. Built once at startup.
/// Provides O(1) lookup: given (date, home_team, away_team), get game outcome.
class GameCache {
public:
    /// Build from games CSV files in data_dir (games_2022-23.csv, etc.)
    void build(const std::string& data_dir);

    /// Look up a game result. Returns nullptr if not found.
    /// Key: "date|home_abbr|away_abbr"
    const GameResult* get(const std::string& date,
                          const std::string& home_abbr,
                          const std::string& away_abbr) const;

    /// Get by matchup string (e.g. "WAS vs. PHI" on a given date)
    const GameResult* get_by_matchup(const std::string& date,
                                      const std::string& home_team,
                                      const std::string& away_team) const;

    /// Get all game results for a team (sorted by date).
    const std::vector<const GameResult*>& team_history(const std::string& team_abbr) const;

    size_t size() const { return results_.size(); }

private:
    // Key: "date|home|away" → GameResult (from home team perspective)
    std::unordered_map<std::string, GameResult> results_;
    // team_abbr → sorted vector of pointers to game results
    std::unordered_map<std::string, std::vector<const GameResult*>> team_games_;
    static const std::vector<const GameResult*> empty_history_;

    static std::string make_key(const std::string& date,
                                 const std::string& home,
                                 const std::string& away) {
        return date + "|" + home + "|" + away;
    }

    void load_file(const std::string& path);
};

} // namespace nba
