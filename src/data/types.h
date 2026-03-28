#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>

namespace nba {

struct PlayerGame {
    std::string player_name;
    int player_id = 0;
    std::string game_date;   // YYYY-MM-DD
    std::string team;        // abbreviation e.g. "DEN"
    std::string matchup;     // "DEN vs. NYK" or "DEN @ PHX"
    bool is_home = false;
    double minutes = 0.0;
    double pts = 0.0;
    double reb = 0.0;
    double ast = 0.0;
    double fg3m = 0.0;
    double stl = 0.0;
    double blk = 0.0;
    double fgm = 0.0;
    double fga = 0.0;
    double ftm = 0.0;
    double fta = 0.0;
    double tov = 0.0;
    double plus_minus = 0.0;
};

struct PropLine {
    std::string date;
    std::string player_name;
    int player_id = 0;
    std::string market_type;  // "player_points", "player_rebounds", etc
    double line = 0.0;
    double over_odds = 0.0;  // American odds
    double under_odds = 0.0;
    std::string bookmaker;
};

struct OddsLine {
    std::string date;
    std::string home_team;
    std::string away_team;
    std::string home_abbr;    // e.g. "WAS"
    std::string away_abbr;    // e.g. "PHI"
    double home_odds = 0.0;   // American
    double away_odds = 0.0;
    std::string market_type;  // "h2h", "spreads", "totals"
    double home_point = 0.0;
    double over_under_point = 0.0;
};

struct Bet {
    std::string date;
    std::string player;
    std::string stat;
    double line = 0.0;
    std::string side;    // "OVER" or "UNDER"
    double odds = 0.0;   // decimal
    double bet_size = 0.0;
    bool won = false;
    double pnl = 0.0;
    double actual = 0.0;
};

} // namespace nba
