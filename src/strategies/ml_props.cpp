#include "ml_props.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nba {

// ML props strategy: predicts the actual stat value directly using
// a weighted ensemble of estimators, then compares prediction to line.
// Without LightGBM: uses a multi-estimator weighted average.
// With LightGBM: trains a LightGBM regressor walk-forward.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Multiple estimators for stat prediction:
struct StatPrediction {
    double season_avg = 0.0;        // simple season average
    double recent_avg = 0.0;        // recent window average
    double weighted_avg = 0.0;      // recency-weighted average
    double rate_based = 0.0;        // per-minute rate * predicted minutes
    double median_recent = 0.0;     // median of recent games
    bool valid = false;
};

static double compute_weighted_avg(const std::vector<double>& vals, int end_idx, int window) {
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    if (count <= 0) return 0.0;

    double wsum = 0.0, wtotal = 0.0;
    for (int i = start; i < n; ++i) {
        double w = static_cast<double>(i - start + 1);
        wsum += vals[i] * w;
        wtotal += w;
    }
    return (wtotal > 1e-9) ? wsum / wtotal : 0.0;
}

static double compute_median(const std::vector<double>& vals, int end_idx, int window) {
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    if (count <= 0) return 0.0;

    std::vector<double> sorted_vals(vals.begin() + start, vals.begin() + n);
    std::sort(sorted_vals.begin(), sorted_vals.end());
    int mid = count / 2;
    if (count % 2 == 0) {
        return (sorted_vals[mid - 1] + sorted_vals[mid]) / 2.0;
    }
    return sorted_vals[mid];
}

static StatPrediction predict_stat(const PlayerStats& player, int end_idx,
                                     const std::string& stat_name,
                                     const StrategyConfig& config) {
    StatPrediction pred;
    const auto& vals = player.get_stat(stat_name);
    if (end_idx < 10) return pred;

    pred.season_avg = features::rolling_avg(vals, end_idx, config.lookback_season);
    pred.recent_avg = features::rolling_avg(vals, end_idx, config.lookback_recent);
    pred.weighted_avg = compute_weighted_avg(vals, end_idx, config.lookback_recent * 2);
    pred.median_recent = compute_median(vals, end_idx, config.lookback_recent);

    // Rate-based: per-minute rate * average minutes
    double rate = features::per_minute_rate(vals, player.minutes, end_idx, config.rate_lookback);
    double avg_mins = features::rolling_avg(player.minutes, end_idx, config.mins_lookback);
    pred.rate_based = rate * avg_mins;

    pred.valid = (pred.season_avg > 0.01);
    return pred;
}

ExperimentResult MlPropsStrategy::run(const StrategyConfig& config,
                                       const DataStore& store,
                                       const PlayerIndex& index,
                                       const KalshiCache& kalshi,
                                  const PropCache* prop_cache) {
    WalkforwardRunner runner(store, index, kalshi, prop_cache);

    const std::string stat_name = config.target_stat.empty()
        ? market_to_stat(config.target_market)
        : config.target_stat;

    bool allow_over = false, allow_under = false;
    for (const auto& s : config.sides) {
        if (s == "OVER") allow_over = true;
        if (s == "UNDER") allow_under = true;
    }
    if (config.sides.empty()) {
        allow_over = true;
        allow_under = true;
    }

    // Ensemble weights for the sub-predictors
    const double w_season = 0.15;
    const double w_recent = 0.25;
    const double w_weighted = 0.25;
    const double w_rate = 0.20;
    const double w_median = 0.15;

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        auto pred = predict_stat(player, end_idx, stat_name, config);
        if (!pred.valid) return pred.valid ? std::optional<Bet>{} : std::nullopt;

        // Ensemble prediction
        double prediction = w_season * pred.season_avg
                           + w_recent * pred.recent_avg
                           + w_weighted * pred.weighted_avg
                           + w_rate * pred.rate_based
                           + w_median * pred.median_recent;

        if (prediction < 0.1) return std::nullopt;

        // Edge: how far is our prediction from the line?
        double edge = (prediction - line) / std::max(line, 0.5);

        std::string side;
        double dk_ml = 0.0;

        if (edge > config.min_edge && allow_over) {
            side = "OVER";
            dk_ml = over_odds_ml;
        } else if (edge < -config.min_edge && allow_under) {
            side = "UNDER";
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Hit rate confirmation
        const auto& vals = player.get_stat(stat_name);
        double hr = (side == "OVER")
            ? features::hit_rate_over(vals, end_idx, line, config.hit_rate_window)
            : features::hit_rate_under(vals, end_idx, line, config.hit_rate_window);
        if (hr < config.min_hit_rate) return std::nullopt;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly sizing with edge scaling
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Scale by edge magnitude
        double edge_mult = std::min(2.0, std::abs(edge) / config.min_edge);
        kelly_frac *= edge_mult;
        kelly_frac = std::min(kelly_frac, config.kelly * 2.0);

        // Agreement bonus: if multiple estimators agree on direction, boost
        int agree_count = 0;
        if (side == "OVER") {
            if (pred.season_avg > line) agree_count++;
            if (pred.recent_avg > line) agree_count++;
            if (pred.weighted_avg > line) agree_count++;
            if (pred.rate_based > line) agree_count++;
            if (pred.median_recent > line) agree_count++;
        } else {
            if (pred.season_avg < line) agree_count++;
            if (pred.recent_avg < line) agree_count++;
            if (pred.weighted_avg < line) agree_count++;
            if (pred.rate_based < line) agree_count++;
            if (pred.median_recent < line) agree_count++;
        }
        // Require at least 3 of 5 estimators to agree
        if (agree_count < 3) return std::nullopt;

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = kelly_frac * config.kelly * 1000.0;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
