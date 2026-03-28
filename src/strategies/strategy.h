#pragma once

#include "../data/types.h"
#include "../data/store.h"
#include "../features/player_index.h"
#include "../features/odds.h"
#include "../data/prop_cache.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace nba {

struct StrategyConfig {
    std::string name;
    std::string type;           // "meanrev", "situational", "twostage", "crossmarket",
                                 // "meta_ensemble", "bayesian", "ml_props", "moneyline",
                                 // "compound", "residual", "ensemble", "timeseries",
                                 // "neural_props", "spreads", "totals"
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

    // Situational strategy params
    double z_thresh = 1.0;
    bool b2b_enabled = true;
    double fatigue_mins = 32.0;
    double defense_thresh = 0.03;
    double blowout_thresh = 0.35;
    bool injury_boost = true;
    bool cold_bounce = false;
    bool trend_enabled = false;
    double consistency_thresh = 0.4;
    int min_factors = 3;

    // Twostage strategy params
    int mins_lookback = 10;       // window for minutes prediction
    int rate_lookback = 15;       // window for per-minute rate
    double b2b_mins_adj = 0.92;   // B2B minutes multiplier (default -8%)
    double min_edge = 0.08;       // minimum prediction-vs-line edge to bet

    // Timeseries (EWMA) strategy params
    double fast_alpha = 0.3;
    double medium_alpha = 0.1;
    double slow_alpha = 0.03;
    double fast_weight = 0.5;
    double medium_weight = 0.3;
    double slow_weight = 0.2;

    // Neural/KNN strategy params
    int k_neighbors = 5;
    int seq_len = 10;

    // Spreads/Totals strategy params
    double elo_k = 30.0;
    double home_advantage = 3.5;
    double min_edge_points = 2.0;
    int pace_window = 10;
    int ortg_window = 10;

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
        // Situational
        j["z_thresh"] = z_thresh;
        j["b2b_enabled"] = b2b_enabled;
        j["fatigue_mins"] = fatigue_mins;
        j["defense_thresh"] = defense_thresh;
        j["blowout_thresh"] = blowout_thresh;
        j["injury_boost"] = injury_boost;
        j["cold_bounce"] = cold_bounce;
        j["trend_enabled"] = trend_enabled;
        j["consistency_thresh"] = consistency_thresh;
        j["min_factors"] = min_factors;
        // Twostage
        j["mins_lookback"] = mins_lookback;
        j["rate_lookback"] = rate_lookback;
        j["b2b_mins_adj"] = b2b_mins_adj;
        j["min_edge"] = min_edge;
        // Timeseries
        j["fast_alpha"] = fast_alpha;
        j["medium_alpha"] = medium_alpha;
        j["slow_alpha"] = slow_alpha;
        j["fast_weight"] = fast_weight;
        j["medium_weight"] = medium_weight;
        j["slow_weight"] = slow_weight;
        // Neural/KNN
        j["k_neighbors"] = k_neighbors;
        j["seq_len"] = seq_len;
        // Spreads/Totals
        j["elo_k"] = elo_k;
        j["home_advantage"] = home_advantage;
        j["min_edge_points"] = min_edge_points;
        j["pace_window"] = pace_window;
        j["ortg_window"] = ortg_window;
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
        // Situational
        if (j.contains("z_thresh"))          c.z_thresh = j["z_thresh"].get<double>();
        if (j.contains("b2b_enabled"))       c.b2b_enabled = j["b2b_enabled"].get<bool>();
        if (j.contains("fatigue_mins"))      c.fatigue_mins = j["fatigue_mins"].get<double>();
        if (j.contains("defense_thresh"))    c.defense_thresh = j["defense_thresh"].get<double>();
        if (j.contains("blowout_thresh"))    c.blowout_thresh = j["blowout_thresh"].get<double>();
        if (j.contains("injury_boost"))      c.injury_boost = j["injury_boost"].get<bool>();
        if (j.contains("cold_bounce"))       c.cold_bounce = j["cold_bounce"].get<bool>();
        if (j.contains("trend_enabled"))     c.trend_enabled = j["trend_enabled"].get<bool>();
        if (j.contains("consistency_thresh"))c.consistency_thresh = j["consistency_thresh"].get<double>();
        if (j.contains("min_factors"))       c.min_factors = j["min_factors"].get<int>();
        // Twostage
        if (j.contains("mins_lookback"))     c.mins_lookback = j["mins_lookback"].get<int>();
        if (j.contains("rate_lookback"))     c.rate_lookback = j["rate_lookback"].get<int>();
        if (j.contains("b2b_mins_adj"))      c.b2b_mins_adj = j["b2b_mins_adj"].get<double>();
        if (j.contains("min_edge"))          c.min_edge = j["min_edge"].get<double>();
        // Timeseries
        if (j.contains("fast_alpha"))        c.fast_alpha = j["fast_alpha"].get<double>();
        if (j.contains("medium_alpha"))      c.medium_alpha = j["medium_alpha"].get<double>();
        if (j.contains("slow_alpha"))        c.slow_alpha = j["slow_alpha"].get<double>();
        if (j.contains("fast_weight"))       c.fast_weight = j["fast_weight"].get<double>();
        if (j.contains("medium_weight"))     c.medium_weight = j["medium_weight"].get<double>();
        if (j.contains("slow_weight"))       c.slow_weight = j["slow_weight"].get<double>();
        // Neural/KNN
        if (j.contains("k_neighbors"))       c.k_neighbors = j["k_neighbors"].get<int>();
        if (j.contains("seq_len"))           c.seq_len = j["seq_len"].get<int>();
        // Spreads/Totals
        if (j.contains("elo_k"))             c.elo_k = j["elo_k"].get<double>();
        if (j.contains("home_advantage"))    c.home_advantage = j["home_advantage"].get<double>();
        if (j.contains("min_edge_points"))   c.min_edge_points = j["min_edge_points"].get<double>();
        if (j.contains("pace_window"))       c.pace_window = j["pace_window"].get<int>();
        if (j.contains("ortg_window"))       c.ortg_window = j["ortg_window"].get<int>();
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
                                  const KalshiCache& kalshi,
                                  const PropCache* prop_cache = nullptr) = 0;
};

} // namespace nba
