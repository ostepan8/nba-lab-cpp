#include "crossmarket.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nba {

// Cross-market strategy: uses game-level context (spread, total, pace, defense)
// to build a weighted logistic model for player prop prediction.
// If HAS_LIGHTGBM is defined, uses LightGBM classifier; otherwise uses a
// simple weighted feature sum with sigmoid activation.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Simple sigmoid function
static double sigmoid(double x) {
    return 1.0 / (1.0 + std::exp(-x));
}

// Build a feature vector for cross-market prediction.
// Returns: {season_avg, l5_avg, l10_avg, z_score, per_min_rate,
//           avg_mins, is_home, hit_rate, consistency}
struct CrossFeatures {
    double season_avg = 0.0;
    double l5_avg = 0.0;
    double l10_avg = 0.0;
    double z_score_val = 0.0;
    double per_min_rate = 0.0;
    double avg_mins = 0.0;
    double is_home = 0.0;
    double hit_rate = 0.0;
    double consistency = 0.0; // 1 - CV (coefficient of variation)
    double line_diff = 0.0;   // (season_avg - line) / season_std
    bool valid = false;
};

static CrossFeatures build_features(const PlayerStats& player, int end_idx,
                                     double line, const std::string& stat_name,
                                     const std::string& side, int season_lb) {
    CrossFeatures f;
    const auto& vals = player.get_stat(stat_name);

    if (end_idx < 10) return f;

    f.season_avg = features::rolling_avg(vals, end_idx, season_lb);
    f.l5_avg = features::rolling_avg(vals, end_idx, 5);
    f.l10_avg = features::rolling_avg(vals, end_idx, 10);
    f.z_score_val = features::z_score(vals, end_idx, 5, season_lb);

    double season_std = features::rolling_std(vals, end_idx, season_lb);
    if (season_std < 1e-9) return f;

    f.per_min_rate = features::per_minute_rate(vals, player.minutes, end_idx, 10);
    f.avg_mins = features::rolling_avg(player.minutes, end_idx, 10);
    f.is_home = (end_idx > 0 && end_idx <= (int)player.is_home.size())
                ? (player.is_home[end_idx - 1] ? 1.0 : 0.0) : 0.5;

    if (side == "OVER") {
        f.hit_rate = features::hit_rate_over(vals, end_idx, line, 20);
    } else {
        f.hit_rate = features::hit_rate_under(vals, end_idx, line, 20);
    }

    f.consistency = (f.season_avg > 1e-9) ? (1.0 - season_std / f.season_avg) : 0.0;
    f.line_diff = (f.season_avg - line) / season_std;

    f.valid = true;
    return f;
}

// Weighted logistic model: hand-tuned weights for each feature.
// Positive output -> OVER likely, negative -> UNDER likely.
// The model predicts P(OVER).
static double predict_prob_over(const CrossFeatures& f) {
    // Weights learned from intuition about what drives prop outcomes:
    // - Positive z-score (hot streak) → likely OVER
    // - High per-min rate → productive player → OVER
    // - High recent avg vs season avg → trending up → OVER
    // - Home advantage → slight OVER boost
    // - Line below season avg → OVER
    double score = 0.0;
    score += 0.30 * f.z_score_val;           // hot/cold streak signal
    score += 0.15 * (f.l5_avg - f.season_avg) / std::max(f.season_avg, 1.0); // recent trend
    score += 0.10 * (f.l10_avg - f.season_avg) / std::max(f.season_avg, 1.0);
    score += 0.20 * f.line_diff;              // season avg vs line
    score += 0.05 * (f.is_home - 0.5);        // home advantage
    score += 0.10 * (f.consistency - 0.5);     // consistent players are more predictable
    score += 0.10 * (f.per_min_rate * f.avg_mins - f.season_avg) /
             std::max(f.season_avg, 1.0);      // production rate signal

    // Clamp to [0.3, 0.7] — no model should be more than 70% confident
    double raw = sigmoid(score);
    return std::max(0.3, std::min(0.7, raw));
}

ExperimentResult CrossMarketStrategy::run(const StrategyConfig& config,
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

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        // Build features for OVER side first, then decide
        auto f_over = build_features(player, end_idx, line, stat_name, "OVER",
                                      config.lookback_season);
        if (!f_over.valid) return std::nullopt;

        double prob_over = predict_prob_over(f_over);
        double prob_under = 1.0 - prob_over;

        // Determine side: pick whichever side has higher model confidence
        std::string side;
        double model_prob = 0.0;
        double dk_ml = 0.0;

        double min_model_prob = config.min_hit_rate; // reuse as min confidence

        if (prob_over > prob_under && prob_over > min_model_prob && allow_over) {
            side = "OVER";
            model_prob = prob_over;
            dk_ml = over_odds_ml;
        } else if (prob_under > prob_over && prob_under > min_model_prob && allow_under) {
            side = "UNDER";
            model_prob = prob_under;
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Hit rate filter (empirical validation)
        const auto& vals = player.get_stat(stat_name);
        double hr = (side == "OVER")
            ? features::hit_rate_over(vals, end_idx, line, config.hit_rate_window)
            : features::hit_rate_under(vals, end_idx, line, config.hit_rate_window);

        // Blend model probability with empirical hit rate
        double blended_prob = 0.6 * model_prob + 0.4 * hr;
        if (blended_prob < config.min_hit_rate) return std::nullopt;

        // Check edge vs implied odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        double implied_prob = 1.0 / dec_odds;
        double edge = blended_prob - implied_prob;
        if (edge < config.min_edge) return std::nullopt;

        // Kelly sizing based on blended probability
        double b = dec_odds - 1.0;
        double kelly_frac = (blended_prob * b - (1.0 - blended_prob)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
        if (kelly_frac < 1e-6) return std::nullopt;

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
