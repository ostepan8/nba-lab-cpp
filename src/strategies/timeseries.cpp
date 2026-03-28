#include "timeseries.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nba {

// Timeseries strategy: EWMA-based time series prediction.
// Uses exponentially-weighted moving averages at 3 decay rates (fast/medium/slow)
// as a lightweight alternative to LSTM/Transformer models.
// The weighted combination captures the same recency patterns LSTMs learn.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Compute EWMA up to (but not including) end_idx with given alpha.
// EWMA_t = alpha * x_t + (1 - alpha) * EWMA_{t-1}
// Returns 0.0 if no data.
static double compute_ewma(const std::vector<double>& vals, int end_idx, double alpha) {
    int n = std::min(end_idx, static_cast<int>(vals.size()));
    if (n <= 0) return 0.0;

    double ewma = vals[0];
    for (int i = 1; i < n; ++i) {
        ewma = alpha * vals[i] + (1.0 - alpha) * ewma;
    }
    return ewma;
}

// Compute the slope/trend of EWMA over the last `window` points.
// Positive = trending up, negative = trending down.
static double compute_ewma_trend(const std::vector<double>& vals, int end_idx,
                                   double alpha, int window) {
    int n = std::min(end_idx, static_cast<int>(vals.size()));
    if (n < window + 1 || window < 2) return 0.0;

    // Compute EWMA at two points: (end_idx - window) and (end_idx)
    double ewma_early = vals[0];
    double ewma_late = vals[0];
    int early_stop = n - window;

    for (int i = 1; i < n; ++i) {
        if (i < early_stop) {
            ewma_early = alpha * vals[i] + (1.0 - alpha) * ewma_early;
        }
        ewma_late = alpha * vals[i] + (1.0 - alpha) * ewma_late;
    }
    // Also finalize early to the stopping point
    ewma_early = vals[0];
    for (int i = 1; i < early_stop; ++i) {
        ewma_early = alpha * vals[i] + (1.0 - alpha) * ewma_early;
    }

    return ewma_late - ewma_early;
}

ExperimentResult TimeseriesStrategy::run(const StrategyConfig& config,
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

    // EWMA parameters from config
    const double fast_alpha   = config.fast_alpha;
    const double medium_alpha = config.medium_alpha;
    const double slow_alpha   = config.slow_alpha;
    const double fast_weight  = config.fast_weight;
    const double medium_weight = config.medium_weight;
    const double slow_weight  = config.slow_weight;

    // Normalize weights
    double w_total = fast_weight + medium_weight + slow_weight;
    double w_fast = (w_total > 1e-9) ? fast_weight / w_total : 0.33;
    double w_med  = (w_total > 1e-9) ? medium_weight / w_total : 0.33;
    double w_slow = (w_total > 1e-9) ? slow_weight / w_total : 0.34;

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        const auto& vals = player.get_stat(stat_name);

        // Compute 3 EWMA series
        double ewma_fast   = compute_ewma(vals, end_idx, fast_alpha);
        double ewma_medium = compute_ewma(vals, end_idx, medium_alpha);
        double ewma_slow   = compute_ewma(vals, end_idx, slow_alpha);

        // Weighted combination = prediction
        double prediction = w_fast * ewma_fast + w_med * ewma_medium + w_slow * ewma_slow;

        if (prediction < 0.1) return std::nullopt;

        // Edge: relative difference between prediction and line
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

        // Trend confirmation: fast EWMA should agree with bet direction
        double trend = ewma_fast - ewma_slow;
        if (side == "OVER" && trend < 0) return std::nullopt;
        if (side == "UNDER" && trend > 0) return std::nullopt;

        // Hit rate filter
        double hr = (side == "OVER")
            ? features::hit_rate_over(vals, end_idx, line, config.hit_rate_window)
            : features::hit_rate_under(vals, end_idx, line, config.hit_rate_window);
        if (hr < config.min_hit_rate) return std::nullopt;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly sizing
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Edge scaling: bigger edge = bigger bet
        double edge_mult = std::min(1.5, std::abs(edge) / config.min_edge);
        kelly_frac *= edge_mult;
        kelly_frac = std::min(kelly_frac, config.kelly * 1.5);

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
