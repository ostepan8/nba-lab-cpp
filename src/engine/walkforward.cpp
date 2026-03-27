#include "walkforward.h"
#include <unordered_map>
#include <chrono>

namespace nba {

WalkforwardRunner::WalkforwardRunner(const DataStore& store,
                                     const PlayerIndex& index,
                                     const KalshiCache& kalshi)
    : store_(store), index_(index), kalshi_(kalshi) {}

ExperimentResult WalkforwardRunner::run(const StrategyConfig& config,
                                        BetCallback callback) {
    auto t0 = std::chrono::high_resolution_clock::now();

    ExperimentResult result;
    result.bankroll = 1000.0;

    const auto& prop_dates = store_.get_prop_dates();

    for (const auto& date : prop_dates) {
        const auto& day_props = store_.get_props(date);
        if (day_props.empty()) continue;

        // Group props by player name for this market type.
        // We only need the first prop per player (they should be unique per
        // market type per player per date, but dedup just in case).
        std::unordered_map<std::string, const PropLine*> player_props;
        player_props.reserve(day_props.size());

        for (const auto& p : day_props) {
            if (p.market_type != config.target_market) continue;
            // Keep only the first occurrence per player
            if (player_props.find(p.player_name) == player_props.end()) {
                player_props[p.player_name] = &p;
            }
        }

        for (const auto& [pname, prop] : player_props) {
            // Look up player in index
            const PlayerStats* ps = nullptr;
            if (prop->player_id != 0) {
                ps = index_.get_by_id(prop->player_id);
            }
            if (!ps) {
                ps = index_.get_by_name(pname);
            }
            if (!ps) continue;

            // Find the game index for the day BEFORE this prop date.
            // We use the date before to avoid lookahead bias -- only use
            // data from games that occurred before this date.
            // find_date_index returns the last game on or before the given date.
            // Props are for games on `date`, so we need games strictly before.
            // Subtract one day approximation: use date itself and subtract 1 from index.
            int date_idx = ps->find_date_index(date);
            // date_idx points to the last game on or before `date`.
            // If that game is ON `date`, it's the game we're predicting, so
            // we should use date_idx - 1 for features. If it's before, use it.
            if (date_idx < 0) continue;
            if (date_idx < static_cast<int>(ps->dates.size()) &&
                ps->dates[date_idx] == date) {
                // Game is on this date -- use prior data only
                date_idx -= 1;
            }
            if (date_idx < 0) continue;

            // Use end_idx = date_idx + 1 (exclusive end for feature functions)
            auto bet_opt = callback(*ps, date_idx + 1, prop->line,
                                    prop->over_odds, prop->under_odds, date);

            if (bet_opt.has_value()) {
                auto& bet = bet_opt.value();

                // Universal kelly cap: 5% of bankroll max per bet
                double max_kelly_bet = 0.05 * 1000.0;  // 5% of $1000 bankroll
                if (bet.bet_size > max_kelly_bet) {
                    bet.bet_size = max_kelly_bet;
                }

                // Resolve actual outcome from gamelog
                // The game on `date` should exist at the original index
                int game_idx = ps->find_date_index(date);
                if (game_idx >= 0 && ps->dates[game_idx] == date) {
                    const auto& stat_vals = ps->get_stat(config.target_stat);
                    double actual = stat_vals[game_idx];
                    bet.actual = actual;

                    if (bet.side == "OVER") {
                        bet.won = actual > bet.line;
                    } else {
                        bet.won = actual < bet.line;
                    }

                    if (bet.won) {
                        bet.pnl = bet.bet_size * (bet.odds - 1.0);
                        result.wins++;
                    } else {
                        bet.pnl = -bet.bet_size;
                    }

                    result.bankroll += bet.pnl;
                    result.pnl += bet.pnl;
                    result.total_bets++;
                    result.bets.push_back(std::move(bet));
                }
            }
        }
    }

    // Compute summary stats
    if (result.total_bets > 0) {
        result.win_rate = static_cast<double>(result.wins) / result.total_bets;

        // ROI = total PnL / total wagered
        double total_wagered = 0.0;
        for (const auto& b : result.bets) {
            total_wagered += b.bet_size;
        }
        result.roi = (total_wagered > 0) ? result.pnl / total_wagered : 0.0;

        // P-value: two methods, take the minimum (matches Python)
        // Method 1: Binomial test — H0: win_rate = 0.5 (same as Python)
        double n = result.total_bets;
        double obs_wr = result.win_rate;
        double p0 = 0.5;  // Python uses fair_p = 0.5
        double se = std::sqrt(p0 * (1.0 - p0) / n);
        double pval_binom = 1.0;
        if (se > 1e-9) {
            double z = (obs_wr - p0) / se;
            // Upper tail: P(WR >= observed | H0: WR = 0.5)
            pval_binom = 0.5 * std::erfc(z / std::sqrt(2.0));
        }

        // Method 2: T-test on PnL series — H0: mean PnL = 0
        double pval_ttest = 1.0;
        if (n >= 5) {
            double mean_pnl = result.pnl / n;
            double var_pnl = 0.0;
            for (const auto& b : result.bets) {
                double diff = b.pnl - mean_pnl;
                var_pnl += diff * diff;
            }
            var_pnl /= (n - 1.0);
            double se_pnl = std::sqrt(var_pnl / n);
            if (se_pnl > 1e-9) {
                double t = mean_pnl / se_pnl;
                // Approximate one-sided p-value from t-statistic
                // For large n, t ≈ z, so use normal CDF
                pval_ttest = 0.5 * std::erfc(t / std::sqrt(2.0));
            }
        }

        result.pvalue = std::min(pval_binom, pval_ttest);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    return result;
}

} // namespace nba
