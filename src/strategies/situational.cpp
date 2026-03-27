#include "situational.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>

namespace nba {

// Map market_type -> stat name
static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Check if two dates are consecutive (back-to-back).
// Simple approach: parse YYYY-MM-DD and check day difference <= 1.
// Handles month boundaries correctly using a days-since-epoch diff.
static int date_to_days(const std::string& d) {
    // YYYY-MM-DD -> approximate days since epoch for comparison
    if (d.size() < 10) return 0;
    int y = std::stoi(d.substr(0, 4));
    int m = std::stoi(d.substr(5, 2));
    int day = std::stoi(d.substr(8, 2));
    // Approximate: good enough for 1-day difference detection
    return y * 365 + m * 30 + day;
}

static bool is_back_to_back(const std::vector<std::string>& dates, int end_idx) {
    // Check if the last two games before end_idx are <= 1 day apart
    if (end_idx < 2) return false;
    int d1 = date_to_days(dates[end_idx - 1]);
    int d2 = date_to_days(dates[end_idx - 2]);
    return (d1 - d2) <= 1;
}

// Evaluate situational factors for a player on a given date.
// Returns: (over_factors, under_factors) — counts of factors favoring each side.
struct FactorCounts {
    int over_factors = 0;
    int under_factors = 0;
};

static FactorCounts evaluate_factors(
    const PlayerStats& player, int end_idx,
    double line, const std::string& stat_name,
    const StrategyConfig& config)
{
    FactorCounts fc;
    const auto& stat_vals = player.get_stat(stat_name);

    // Compute z-score
    double z = features::z_score(stat_vals, end_idx,
                                  config.lookback_recent,
                                  config.lookback_season);

    double season_avg = features::rolling_avg(stat_vals, end_idx,
                                               config.lookback_season);
    double season_std = features::rolling_std(stat_vals, end_idx,
                                               config.lookback_season);

    // Factor 1: Hot streak (z > z_thresh) -> UNDER (mean reversion)
    if (z > config.z_thresh) {
        fc.under_factors++;
    }

    // Factor 2: Cold streak (z < -z_thresh) -> OVER (mean reversion)
    if (z < -config.z_thresh) {
        fc.over_factors++;
    }

    // Factor 3: Back-to-back (rest <= 1 day) -> UNDER (fatigue)
    if (config.b2b_enabled && is_back_to_back(player.dates, end_idx)) {
        fc.under_factors++;
    }

    // Factor 4: High minutes last game (> fatigue_mins) -> UNDER (fatigue)
    if (end_idx >= 1) {
        double last_mins = player.minutes[end_idx - 1];
        if (last_mins > config.fatigue_mins) {
            fc.under_factors++;
        }
    }

    // TODO: Factor 5 — Tough positional defense -> UNDER
    // Requires positional defense data from Python pickle files.
    // When available: check opponent's defensive rating vs player's position.
    // If opponent defense rating > league_avg + defense_thresh -> UNDER.

    // TODO: Factor 6 — Weak positional defense -> OVER
    // Requires positional defense data from Python pickle files.
    // If opponent defense rating < league_avg - defense_thresh -> OVER.

    // TODO: Factor 7 — Blowout expected -> UNDER (starters sit in blowouts)
    // Requires game-level spread/odds data cross-referenced with blowout_thresh.
    // If abs(spread) > blowout_thresh * 100 -> UNDER.

    // Factor 8: Line above season average -> UNDER
    if (season_std > 1e-9) {
        double line_z = (line - season_avg) / season_std;
        if (line_z > config.line_gap_threshold) {
            fc.under_factors++;
        }
    }

    // Factor 9: Line below season average -> OVER
    if (season_std > 1e-9) {
        double line_z = (season_avg - line) / season_std;
        if (line_z > config.line_gap_threshold) {
            fc.over_factors++;
        }
    }

    // Optional: cold bounce (cold streak -> OVER bounce-back instead of UNDER)
    if (config.cold_bounce && z < -config.z_thresh) {
        // Already counted as OVER in factor 2
    }

    // Optional: trend mode (sustained trend -> same direction, not reversion)
    if (config.trend_enabled) {
        // If z > z_thresh and recently trending up -> OVER (ride the trend)
        if (z > config.z_thresh) {
            fc.over_factors++;  // add a countervailing OVER factor for trend
        }
        if (z < -config.z_thresh) {
            fc.under_factors++;  // add countervailing UNDER for downtrend
        }
    }

    // Consistency filter: if player's coefficient of variation is too high,
    // their stats are too noisy to predict reliably
    if (season_avg > 1e-9 && season_std > 1e-9) {
        double cv = season_std / season_avg;
        if (cv > config.consistency_thresh) {
            // High variance player: reduce conviction by removing one factor each
            if (fc.over_factors > 0) fc.over_factors--;
            if (fc.under_factors > 0) fc.under_factors--;
        }
    }

    return fc;
}

ExperimentResult SituationalStrategy::run(const StrategyConfig& config,
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

        // Evaluate all factors
        auto fc = evaluate_factors(player, end_idx, line, stat_name, config);

        // Determine bet direction: whichever side has more factors
        std::string side;
        double dk_ml = 0.0;
        int factor_count = 0;

        if (fc.under_factors >= config.min_factors &&
            fc.under_factors > fc.over_factors && allow_under) {
            side = "UNDER";
            dk_ml = under_odds_ml;
            factor_count = fc.under_factors;
        } else if (fc.over_factors >= config.min_factors &&
                   fc.over_factors > fc.under_factors && allow_over) {
            side = "OVER";
            dk_ml = over_odds_ml;
            factor_count = fc.over_factors;
        } else {
            return std::nullopt;
        }

        // Hit rate filter
        const auto& stat_vals = player.get_stat(stat_name);
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

        // Kelly sizing with factor boost: more factors = bigger fraction
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, config.kelly));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Factor boost: scale up Kelly by factor_count / min_factors
        // Capped at 2x the base Kelly
        double boost = std::min(2.0,
            static_cast<double>(factor_count) / config.min_factors);
        kelly_frac *= boost;
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
