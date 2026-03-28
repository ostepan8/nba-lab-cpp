#include "compound.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace nba {

// Compound strategy: bets bigger when multiple stat props for the same player
// all align in the same direction. Uses meanrev z-score per stat market.
// If UNDER signals on PTS + REB + AST all align → boost Kelly.
// Walks through each player-date and checks all stat markets.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Check z-score signal for a specific stat
struct StatSignal {
    std::string stat_name;
    std::string market;
    double z = 0.0;
    double hr = 0.0;
    std::string suggested_side;
    bool valid = false;
};

static StatSignal check_stat_signal(const PlayerStats& player, int end_idx,
                                      const std::string& stat_name,
                                      double line, double min_dev,
                                      int lr, int ls, int hr_window) {
    StatSignal sig;
    sig.stat_name = stat_name;
    const auto& vals = player.get_stat(stat_name);
    if (end_idx < 10) return sig;

    double z = features::z_score(vals, end_idx, lr, ls);
    sig.z = z;

    double season_std = features::rolling_std(vals, end_idx, ls);
    if (season_std < 1e-9) return sig;

    if (z > min_dev) {
        sig.suggested_side = "UNDER";
        sig.hr = features::hit_rate_under(vals, end_idx, line, hr_window);
        sig.valid = true;
    } else if (z < -min_dev) {
        sig.suggested_side = "OVER";
        sig.hr = features::hit_rate_over(vals, end_idx, line, hr_window);
        sig.valid = true;
    }

    return sig;
}

ExperimentResult CompoundStrategy::run(const StrategyConfig& config,
                                        const DataStore& store,
                                        const PlayerIndex& index,
                                        const KalshiCache& kalshi,
                                  const PropCache* prop_cache,
                                  const GameCache* game_cache) {
    (void)game_cache;
    WalkforwardRunner runner(store, index, kalshi, prop_cache);

    const std::string primary_stat = config.target_stat.empty()
        ? market_to_stat(config.target_market)
        : config.target_stat;

    // All stat types we check for compound signals
    static const std::vector<std::pair<std::string, std::string>> ALL_STATS = {
        {"PTS", "player_points"},
        {"REB", "player_rebounds"},
        {"AST", "player_assists"},
        {"FG3M", "player_threes"},
        {"STL", "player_steals"},
        {"BLK", "player_blocks"},
    };

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

        // Check signal for the primary stat
        auto primary_sig = check_stat_signal(player, end_idx, primary_stat,
                                              line, config.min_dev,
                                              config.lookback_recent,
                                              config.lookback_season,
                                              config.hit_rate_window);
        if (!primary_sig.valid) return std::nullopt;
        if (primary_sig.hr < config.min_hit_rate) return std::nullopt;

        // Check if the suggested side is allowed
        if (primary_sig.suggested_side == "OVER" && !allow_over) return std::nullopt;
        if (primary_sig.suggested_side == "UNDER" && !allow_under) return std::nullopt;

        // Count how many OTHER stats also signal the same direction
        int compound_count = 1; // primary stat counts
        for (const auto& [stat, mkt] : ALL_STATS) {
            if (stat == primary_stat) continue;

            // Use a dummy line of 0 (just checking z-score direction)
            auto sig = check_stat_signal(player, end_idx, stat, 0.0,
                                          config.min_dev,
                                          config.lookback_recent,
                                          config.lookback_season,
                                          config.hit_rate_window);
            if (sig.valid && sig.suggested_side == primary_sig.suggested_side) {
                compound_count++;
            }
        }

        // Require at least 2 stats to align (not just 1 strong one)
        // Also respect min_factors from config
        if (compound_count < 2) return std::nullopt;
        if (compound_count < config.min_factors) return std::nullopt;

        std::string side = primary_sig.suggested_side;
        double dk_ml = (side == "OVER") ? over_odds_ml : under_odds_ml;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       primary_stat, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly sizing with compound boost
        double b = dec_odds - 1.0;
        double kelly_frac = (primary_sig.hr * b - (1.0 - primary_sig.hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Compound boost: more aligned stats → bigger bet, capped at 1.5x
        double boost = 1.0 + 0.15 * (compound_count - 1);
        boost = std::min(boost, 1.5);
        kelly_frac *= boost;
        kelly_frac = std::min(kelly_frac, 0.05);

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = primary_stat;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = kelly_frac * config.kelly * 1000.0;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
