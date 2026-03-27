#include "meanrev.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>

namespace nba {

// Map market_type → stat name for get_stat()
static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

ExperimentResult MeanRevStrategy::run(const StrategyConfig& config,
                                       const DataStore& store,
                                       const PlayerIndex& index,
                                       const KalshiCache& kalshi) {
    WalkforwardRunner runner(store, index, kalshi);

    // Determine which stat array to use
    const std::string stat_name = config.target_stat.empty()
        ? market_to_stat(config.target_market)
        : config.target_stat;

    // Build a set of allowed sides for O(1) lookup
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
        // Minimum games check
        if (end_idx < config.min_games) return std::nullopt;

        const auto& stat_vals = player.get_stat(stat_name);

        // Compute z-score: (recent_avg - season_avg) / season_std
        double z = features::z_score(stat_vals, end_idx,
                                     config.lookback_recent,
                                     config.lookback_season);

        double season_avg = features::rolling_avg(stat_vals, end_idx,
                                                  config.lookback_season);
        double season_std = features::rolling_std(stat_vals, end_idx,
                                                  config.lookback_season);

        if (season_std < 1e-9) return std::nullopt;

        // Determine bet side based on deviation direction
        std::string side;
        double dk_ml = 0.0;

        // Player is running HOT (z > min_dev) → bet UNDER (mean reversion)
        // Player is running COLD (z < -min_dev) → bet OVER (mean reversion)
        if (z > config.min_dev && allow_under) {
            // Line gap: check that line is meaningfully above average
            double gap = (line - season_avg) / season_std;
            if (gap < config.line_gap_threshold) return std::nullopt;

            side = "UNDER";
            dk_ml = under_odds_ml;
        } else if (z < -config.min_dev && allow_over) {
            // Line gap: check that average is meaningfully above line
            double gap = (season_avg - line) / season_std;
            if (gap < config.line_gap_threshold) return std::nullopt;

            side = "OVER";
            dk_ml = over_odds_ml;
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

        // Resolve odds (Kalshi first, DK fallback)
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;

        // Max odds filter (skip extreme longshots)
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly criterion: k = (hr * b - (1 - hr)) / b
        // where b = decimal_odds - 1 (net payout ratio)
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));

        if (kelly_frac < 1e-6) return std::nullopt;

        // Size the bet
        double bet_size = kelly_frac * config.kelly * 1000.0;  // fraction of starting bankroll

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = bet_size;
        // won, pnl, actual will be filled by the walkforward runner

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
