#pragma once

#include "../data/types.h"
#include "../data/store.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace nba {

struct StrategyConfig {
    std::string name;
    std::string type;           // "meanrev", "situational", etc.
    std::string target_stat;    // "PTS", "REB", "AST", "FG3M", "STL", "BLK"
    std::string target_market;  // "player_points", "player_rebounds", etc.
    std::vector<std::string> sides;  // {"OVER","UNDER"} or {"UNDER"}
    double min_dev = 0.8;
    int lookback_recent = 5;
    int lookback_season = 40;
    double min_hit_rate = 0.5;
    int min_games = 15;
    double kelly = 0.05;
    double max_odds = 2.5;
    int hit_rate_window = 20;
    double line_gap_threshold = 0.5;  // (avg - line)/std threshold

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["name"] = name;
        j["type"] = type;
        j["target_stat"] = target_stat;
        j["target_market"] = target_market;
        j["sides"] = sides;
        j["min_dev"] = min_dev;
        j["lookback_recent"] = lookback_recent;
        j["lookback_season"] = lookback_season;
        j["min_hit_rate"] = min_hit_rate;
        j["min_games"] = min_games;
        j["kelly"] = kelly;
        j["max_odds"] = max_odds;
        j["hit_rate_window"] = hit_rate_window;
        j["line_gap_threshold"] = line_gap_threshold;
        return j;
    }

    static StrategyConfig from_json(const nlohmann::json& j) {
        StrategyConfig c;
        if (j.contains("name"))              c.name = j["name"].get<std::string>();
        if (j.contains("type"))              c.type = j["type"].get<std::string>();
        if (j.contains("target_stat"))       c.target_stat = j["target_stat"].get<std::string>();
        if (j.contains("target_market"))     c.target_market = j["target_market"].get<std::string>();
        if (j.contains("sides"))             c.sides = j["sides"].get<std::vector<std::string>>();
        if (j.contains("min_dev"))           c.min_dev = j["min_dev"].get<double>();
        if (j.contains("lookback_recent"))   c.lookback_recent = j["lookback_recent"].get<int>();
        if (j.contains("lookback_season"))   c.lookback_season = j["lookback_season"].get<int>();
        if (j.contains("min_hit_rate"))      c.min_hit_rate = j["min_hit_rate"].get<double>();
        if (j.contains("min_games"))         c.min_games = j["min_games"].get<int>();
        if (j.contains("kelly"))             c.kelly = j["kelly"].get<double>();
        if (j.contains("max_odds"))          c.max_odds = j["max_odds"].get<double>();
        if (j.contains("hit_rate_window"))   c.hit_rate_window = j["hit_rate_window"].get<int>();
        if (j.contains("line_gap_threshold"))c.line_gap_threshold = j["line_gap_threshold"].get<double>();
        return c;
    }
};

struct ExperimentResult {
    int total_bets = 0;
    int wins = 0;
    double win_rate = 0.0;
    double roi = 0.0;
    double pnl = 0.0;
    double pvalue = 1.0;
    double bankroll = 1000.0;
    std::vector<Bet> bets;
    double elapsed_seconds = 0.0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual ExperimentResult run(const StrategyConfig& config,
                                  const DataStore& store,
                                  const PlayerIndex& index,
                                  const KalshiCache& kalshi) = 0;
};

} // namespace nba
