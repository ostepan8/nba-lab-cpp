#include "residual.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace nba {

// Residual strategy: instead of predicting raw stat values, predict
// (actual - line) residuals. If a player consistently beats the line,
// bet OVER. If they consistently miss, bet UNDER.
//
// This exploits systematic line mispricing for specific players.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Compute weighted average of residuals (actual - line) for recent games.
// We approximate by using (actual - season_avg) as a proxy for residuals
// since we don't have historical line data for every game in the walkforward.
// The key insight: if the player's recent actuals are consistently above/below
// the current line, that pattern likely persists.
struct ResidualStats {
    double mean_residual = 0.0;    // avg(actual - line) proxy
    double weighted_residual = 0.0; // recency-weighted
    double residual_std = 0.0;     // std of residuals
    double consistency = 0.0;      // fraction of residuals with same sign
    bool valid = false;
};

static ResidualStats compute_residuals(const std::vector<double>& vals,
                                        int end_idx, double line, int window) {
    ResidualStats rs;
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    // Require minimum 20 games of residual history
    if (count < 20) return rs;

    // Compute residuals: actual - line
    std::vector<double> residuals;
    residuals.reserve(count);
    for (int i = start; i < n; ++i) {
        residuals.push_back(vals[i] - line);
    }

    // Compute raw mean and std first for trimming
    double raw_sum = 0.0;
    for (double r : residuals) raw_sum += r;
    double raw_mean = raw_sum / count;
    double raw_sq_sum = 0.0;
    for (double r : residuals) {
        double diff = r - raw_mean;
        raw_sq_sum += diff * diff;
    }
    double raw_std = std::sqrt(raw_sq_sum / count);

    // Trimmed mean: remove outlier residuals beyond 2 std devs before averaging
    std::vector<double> trimmed;
    trimmed.reserve(count);
    for (double r : residuals) {
        if (raw_std < 1e-9 || std::abs(r - raw_mean) <= 2.0 * raw_std) {
            trimmed.push_back(r);
        }
    }
    if (trimmed.empty()) return rs;

    // Mean residual from trimmed data
    double sum = 0.0;
    for (double r : trimmed) sum += r;
    rs.mean_residual = sum / trimmed.size();

    // Weighted mean (recency-weighted) from full data, but cap at 2 std devs
    double wsum = 0.0, wtotal = 0.0;
    for (int i = 0; i < count; ++i) {
        double capped = std::max(-2.0 * raw_std, std::min(2.0 * raw_std, residuals[i]));
        double w = static_cast<double>(i + 1);
        wsum += capped * w;
        wtotal += w;
    }
    rs.weighted_residual = (wtotal > 1e-9) ? wsum / wtotal : 0.0;

    // Cap predicted residual at 2 standard deviations
    if (raw_std > 1e-9) {
        rs.weighted_residual = std::max(-2.0 * raw_std, std::min(2.0 * raw_std, rs.weighted_residual));
        rs.mean_residual = std::max(-2.0 * raw_std, std::min(2.0 * raw_std, rs.mean_residual));
    }

    // Residual std from trimmed data
    double sq_sum = 0.0;
    for (double r : trimmed) {
        double diff = r - rs.mean_residual;
        sq_sum += diff * diff;
    }
    rs.residual_std = std::sqrt(sq_sum / trimmed.size());

    // Consistency: fraction of residuals with the same sign as the mean
    int same_sign = 0;
    for (double r : residuals) {
        if ((r > 0 && rs.mean_residual > 0) || (r < 0 && rs.mean_residual < 0)) {
            same_sign++;
        }
    }
    rs.consistency = static_cast<double>(same_sign) / count;

    rs.valid = true;
    return rs;
}

ExperimentResult ResidualStrategy::run(const StrategyConfig& config,
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

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        const auto& vals = player.get_stat(stat_name);

        // Compute residual statistics
        auto rs = compute_residuals(vals, end_idx, line, config.lookback_season);
        if (!rs.valid) return std::nullopt;
        if (rs.residual_std < 1e-9) return std::nullopt;

        // Signal strength: how many standard deviations is the residual from zero?
        double residual_z = rs.weighted_residual / rs.residual_std;

        // Determine side
        std::string side;
        double dk_ml = 0.0;

        if (residual_z > config.min_dev && allow_over && rs.consistency > config.consistency_thresh) {
            // Player consistently beats the line → OVER
            side = "OVER";
            dk_ml = over_odds_ml;
        } else if (residual_z < -config.min_dev && allow_under && rs.consistency > config.consistency_thresh) {
            // Player consistently misses the line → UNDER
            side = "UNDER";
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

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
        kelly_frac = std::max(0.0, std::min(kelly_frac, config.kelly));
        if (kelly_frac < 1e-6) return std::nullopt;

        // No consistency boost — cap kelly at config.kelly to avoid overconfidence
        kelly_frac = std::min(kelly_frac, config.kelly);

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = kelly_frac * 1000.0;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
