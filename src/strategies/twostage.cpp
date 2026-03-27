#include "twostage.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>

namespace nba {

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Check if the player's last two games are on consecutive days (back-to-back)
static bool is_b2b(const std::vector<std::string>& dates, int end_idx) {
    if (end_idx < 2) return false;
    const auto& d1 = dates[end_idx - 1];
    const auto& d2 = dates[end_idx - 2];
    if (d1.size() < 10 || d2.size() < 10) return false;
    int y1 = std::stoi(d1.substr(0, 4)), m1 = std::stoi(d1.substr(5, 2)),
        day1 = std::stoi(d1.substr(8, 2));
    int y2 = std::stoi(d2.substr(0, 4)), m2 = std::stoi(d2.substr(5, 2)),
        day2 = std::stoi(d2.substr(8, 2));
    int approx1 = y1 * 365 + m1 * 30 + day1;
    int approx2 = y2 * 365 + m2 * 30 + day2;
    return (approx1 - approx2) <= 1;
}

// Weighted average: more recent games get higher weight.
// Linear decay: weight_i = (i - start + 1) for i in [start, end_idx).
static double weighted_avg(const std::vector<double>& vals, int end_idx, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = static_cast<int>(vals.size());
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count <= 0) return 0.0;

    double wsum = 0.0;
    double wtotal = 0.0;
    for (int i = start; i < end_idx; ++i) {
        double w = static_cast<double>(i - start + 1);
        wsum += vals[i] * w;
        wtotal += w;
    }
    return (wtotal > 1e-9) ? wsum / wtotal : 0.0;
}

ExperimentResult TwostageStrategy::run(const StrategyConfig& config,
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

        const auto& stat_vals = player.get_stat(stat_name);
        const auto& mins = player.minutes;

        // Stage 1: Predict minutes
        // Weighted average of recent minutes with B2B adjustment
        double pred_mins = weighted_avg(mins, end_idx, config.mins_lookback);
        if (pred_mins < 1.0) return std::nullopt;  // DNP / garbage time player

        // B2B adjustment: reduce predicted minutes
        if (is_b2b(player.dates, end_idx)) {
            pred_mins *= config.b2b_mins_adj;
        }

        // Stage 2: Predict per-minute production rate
        // Weighted average of stat/minute over recent games
        // Build per-minute values for the window
        int n = std::min(end_idx, static_cast<int>(stat_vals.size()));
        n = std::min(n, static_cast<int>(mins.size()));
        int start = std::max(0, n - config.rate_lookback);

        // Compute per-game rates, then weighted average
        std::vector<double> rates;
        rates.reserve(n - start);
        for (int i = start; i < n; ++i) {
            if (mins[i] > 1.0) {
                rates.push_back(stat_vals[i] / mins[i]);
            }
        }
        if (rates.empty()) return std::nullopt;

        // Weighted average of rates (most recent = highest weight)
        double rate_wsum = 0.0, rate_wtotal = 0.0;
        for (size_t i = 0; i < rates.size(); ++i) {
            double w = static_cast<double>(i + 1);
            rate_wsum += rates[i] * w;
            rate_wtotal += w;
        }
        double pred_rate = (rate_wtotal > 1e-9) ? rate_wsum / rate_wtotal : 0.0;

        // Combined prediction
        double prediction = pred_mins * pred_rate;
        if (prediction < 0.1) return std::nullopt;

        // Edge: how far is our prediction from the line?
        double edge = (prediction - line) / line;

        // Determine side based on edge direction
        std::string side;
        double dk_ml = 0.0;

        if (edge > config.min_edge && allow_over) {
            // We predict higher than the line -> bet OVER
            side = "OVER";
            dk_ml = over_odds_ml;
        } else if (edge < -config.min_edge && allow_under) {
            // We predict lower than the line -> bet UNDER
            side = "UNDER";
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Hit rate filter
        double hr = 0.0;
        if (side == "OVER") {
            hr = features::hit_rate_over(stat_vals, end_idx, line,
                                          config.hit_rate_window);
        } else {
            hr = features::hit_rate_under(stat_vals, end_idx, line,
                                           config.hit_rate_window);
        }
        if (hr < config.min_hit_rate) return std::nullopt;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly criterion with edge scaling
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, config.kelly));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Scale Kelly by edge magnitude (bigger edge = more conviction)
        double edge_mult = std::min(2.0, std::abs(edge) / config.min_edge);
        kelly_frac *= edge_mult;
        kelly_frac = std::min(kelly_frac, config.kelly * 2.0);

        double bet_size = kelly_frac * 1000.0;

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = bet_size;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
