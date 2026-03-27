#include "walkforward.h"
#include <unordered_map>
#include <unordered_set>
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

        // Group props by player name, compute MEDIAN line and odds across bookmakers
        // (matches Python: groupby("player_name").agg({"line": "median", ...}))
        struct AggProp {
            std::string player_name;
            int player_id = 0;
            std::vector<double> lines, over_odds, under_odds;
            PropLine median_prop;  // Will hold the aggregated values
        };
        std::unordered_map<std::string, AggProp> player_agg;

        for (const auto& p : day_props) {
            if (p.market_type != config.target_market) continue;
            auto& agg = player_agg[p.player_name];
            agg.player_name = p.player_name;
            if (p.player_id != 0) agg.player_id = p.player_id;
            agg.lines.push_back(p.line);
            agg.over_odds.push_back(p.over_odds);
            agg.under_odds.push_back(p.under_odds);
        }

        // Compute medians
        auto median = [](std::vector<double>& v) -> double {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            size_t n = v.size();
            return (n % 2 == 0) ? (v[n/2 - 1] + v[n/2]) / 2.0 : v[n/2];
        };

        std::vector<std::pair<std::string, PropLine>> player_props_vec;
        for (auto& [name, agg] : player_agg) {
            PropLine pl;
            pl.player_name = name;
            pl.player_id = agg.player_id;
            pl.market_type = config.target_market;
            pl.date = date;
            pl.line = median(agg.lines);
            pl.over_odds = median(agg.over_odds);
            pl.under_odds = median(agg.under_odds);
            player_props_vec.push_back({name, pl});
        }

        // Aggregate by player_id — merge name variants into one entry per player
        // "Cam Thomas" and "Cameron Thomas" with the same ID get merged,
        // their lines/odds combined into one median. Matches Python behavior
        // where groupby("player_name") + player_id lookup effectively deduplicates.
        {
            std::unordered_map<int, size_t> id_to_idx;  // player_id → index in deduped
            std::vector<std::pair<std::string, AggProp>> merged;
            for (auto& [name, agg] : player_agg) {
                if (agg.player_id != 0 && id_to_idx.count(agg.player_id)) {
                    // Merge into existing entry
                    auto& existing = merged[id_to_idx[agg.player_id]].second;
                    existing.lines.insert(existing.lines.end(), agg.lines.begin(), agg.lines.end());
                    existing.over_odds.insert(existing.over_odds.end(), agg.over_odds.begin(), agg.over_odds.end());
                    existing.under_odds.insert(existing.under_odds.end(), agg.under_odds.begin(), agg.under_odds.end());
                } else {
                    if (agg.player_id != 0) id_to_idx[agg.player_id] = merged.size();
                    merged.push_back({name, agg});
                }
            }
            // Rebuild player_props_vec from merged
            player_props_vec.clear();
            for (auto& [name, agg] : merged) {
                PropLine pl;
                pl.player_name = name;
                pl.player_id = agg.player_id;
                pl.market_type = config.target_market;
                pl.date = date;
                pl.line = median(agg.lines);
                pl.over_odds = median(agg.over_odds);
                pl.under_odds = median(agg.under_odds);
                player_props_vec.push_back({name, pl});
            }
        }

        for (const auto& [pname, prop_val] : player_props_vec) {
            const PropLine* prop = &prop_val;
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

                // Bet size is kelly fraction — multiply by current bankroll (compounding)
                // Strategy returns kelly_frac in bet_size field, we scale by bankroll here
                bet.bet_size = bet.bet_size * result.bankroll / 1000.0;

                // Universal kelly cap: 5% of current bankroll per bet
                double max_kelly_bet = 0.05 * result.bankroll;
                if (bet.bet_size > max_kelly_bet) {
                    bet.bet_size = max_kelly_bet;
                }
                if (bet.bet_size < 0.50) continue;  // Skip tiny bets

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
