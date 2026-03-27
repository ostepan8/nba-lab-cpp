#include "meta_ensemble.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nba {

// Meta-ensemble: combines signals from multiple feature families
// (rolling avgs, z-scores, trend slope, consistency, autocorrelation, hit rates).
// Without LightGBM: uses a weighted average of feature-based sub-signals.
// With LightGBM: trains walk-forward LightGBM on these features.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Compute trend slope via simple linear regression on the last `window` values.
// Returns the slope (positive = trending up).
static double trend_slope(const std::vector<double>& vals, int end_idx, int window) {
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    if (count < 3) return 0.0;

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (int i = 0; i < count; ++i) {
        double x = static_cast<double>(i);
        double y = vals[start + i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    double denom = count * sum_x2 - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) return 0.0;
    return (count * sum_xy - sum_x * sum_y) / denom;
}

// Autocorrelation at lag 1 for the last `window` values.
static double autocorr_lag1(const std::vector<double>& vals, int end_idx, int window) {
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    if (count < 4) return 0.0;

    double mean = 0.0;
    for (int i = start; i < n; ++i) mean += vals[i];
    mean /= count;

    double num = 0.0, den = 0.0;
    for (int i = start; i < n; ++i) {
        den += (vals[i] - mean) * (vals[i] - mean);
        if (i > start) {
            num += (vals[i] - mean) * (vals[i - 1] - mean);
        }
    }
    return (den > 1e-12) ? num / den : 0.0;
}

struct MetaSignal {
    double z_signal = 0.0;       // z-score based (mean-reversion)
    double trend_signal = 0.0;   // trend-following
    double consistency_signal = 0.0;
    double autocorr_signal = 0.0;
    double hit_rate_signal = 0.0;
    double rate_signal = 0.0;    // per-minute rate prediction
    bool valid = false;
};

static MetaSignal compute_signals(const PlayerStats& player, int end_idx,
                                    double line, const std::string& stat_name,
                                    const StrategyConfig& config) {
    MetaSignal s;
    const auto& vals = player.get_stat(stat_name);
    if (end_idx < 10) return s;

    double season_avg = features::rolling_avg(vals, end_idx, config.lookback_season);
    double season_std = features::rolling_std(vals, end_idx, config.lookback_season);
    if (season_std < 1e-9) return s;

    // Z-score signal: positive z → player is hot → OVER
    double z = features::z_score(vals, end_idx, config.lookback_recent, config.lookback_season);
    s.z_signal = z;  // raw z-score, positive = trending above average

    // Trend signal: positive slope → trending up → OVER
    double slope = trend_slope(vals, end_idx, config.lookback_recent);
    s.trend_signal = slope / std::max(season_std, 1.0);

    // Consistency: low CV → more predictable → stronger signal
    double cv = season_std / std::max(season_avg, 1.0);
    s.consistency_signal = 1.0 - std::min(cv, 1.0);

    // Autocorrelation: high autocorr → momentum is real
    s.autocorr_signal = autocorr_lag1(vals, end_idx, 20);

    // Hit rate signals
    double hr_over = features::hit_rate_over(vals, end_idx, line, config.hit_rate_window);
    double hr_under = features::hit_rate_under(vals, end_idx, line, config.hit_rate_window);
    // Positive → OVER is historically more likely
    s.hit_rate_signal = hr_over - hr_under;

    // Per-minute rate prediction vs line
    double rate = features::per_minute_rate(vals, player.minutes, end_idx, 10);
    double avg_mins = features::rolling_avg(player.minutes, end_idx, 10);
    double pred = rate * avg_mins;
    s.rate_signal = (pred - line) / std::max(season_std, 1.0);

    s.valid = true;
    return s;
}

ExperimentResult MetaEnsembleStrategy::run(const StrategyConfig& config,
                                            const DataStore& store,
                                            const PlayerIndex& index,
                                            const KalshiCache& kalshi) {
    WalkforwardRunner runner(store, index, kalshi);

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

    // Weights for combining sub-signals (without ML, hand-tuned)
    const double w_z = 0.20;
    const double w_trend = 0.15;
    const double w_hit = 0.25;
    const double w_rate = 0.20;
    const double w_autocorr = 0.10;
    const double w_consistency = 0.10;

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        auto sig = compute_signals(player, end_idx, line, stat_name, config);
        if (!sig.valid) return std::nullopt;

        // Combine signals into a single score.
        // Positive score → OVER, negative → UNDER.
        double combined = w_z * sig.z_signal
                        + w_trend * sig.trend_signal
                        + w_hit * sig.hit_rate_signal
                        + w_rate * sig.rate_signal
                        + w_autocorr * sig.autocorr_signal * sig.z_signal
                        + w_consistency * sig.consistency_signal * sig.z_signal;

        // Normalize combined signal to [-1, 1] range before sigmoid
        combined = std::max(-1.0, std::min(1.0, combined));

        // Convert to probability via sigmoid, then clamp to [0.35, 0.65]
        double prob_over_raw = 1.0 / (1.0 + std::exp(-combined));
        double prob_over = std::max(0.35, std::min(0.65, prob_over_raw));
        double prob_under = 1.0 - prob_over;

        std::string side;
        double model_prob = 0.0;
        double dk_ml = 0.0;

        if (prob_over > prob_under && prob_over > config.min_hit_rate && allow_over) {
            side = "OVER";
            model_prob = prob_over;
            dk_ml = over_odds_ml;
        } else if (prob_under > prob_over && prob_under > config.min_hit_rate && allow_under) {
            side = "UNDER";
            model_prob = prob_under;
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Edge check
        double implied_prob = 1.0 / dec_odds;
        double edge = model_prob - implied_prob;
        if (edge < config.min_edge) return std::nullopt;

        // Kelly sizing
        double b = dec_odds - 1.0;
        double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
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
