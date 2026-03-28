#include "walkforward.h"
#include <unordered_map>
#include <unordered_set>
#include <chrono>

namespace nba {

WalkforwardRunner::WalkforwardRunner(const DataStore& store,
                                     const PlayerIndex& index,
                                     const KalshiCache& kalshi,
                                     const PropCache* prop_cache)
    : store_(store), index_(index), kalshi_(kalshi), prop_cache_(prop_cache) {}

ExperimentResult WalkforwardRunner::run(const StrategyConfig& config,
                                        BetCallback callback) {
    auto t0 = std::chrono::high_resolution_clock::now();

    ExperimentResult result;
    result.bankroll = 1000.0;

    const auto& prop_dates = prop_cache_ ? prop_cache_->dates() : store_.get_prop_dates();

    for (const auto& date : prop_dates) {
        // --- Get aggregated props (pre-computed or compute on the fly) ---
        const std::vector<AggregatedProp>* agg_props_ptr = nullptr;
        std::vector<AggregatedProp> fallback_props;

        if (prop_cache_) {
            // Fast path: use pre-computed cache
            agg_props_ptr = &prop_cache_->get(date, config.target_market);
        } else {
            // Slow path: compute on the fly (legacy)
            const auto& day_props = store_.get_props(date);
            if (day_props.empty()) continue;

            struct RawAgg {
                std::string player_name;
                int player_id = 0;
                std::vector<double> lines, over_odds, under_odds;
            };
            std::unordered_map<std::string, RawAgg> player_agg;

            for (const auto& p : day_props) {
                if (p.market_type != config.target_market) continue;
                auto& agg = player_agg[p.player_name];
                agg.player_name = p.player_name;
                if (p.player_id != 0) agg.player_id = p.player_id;
                agg.lines.push_back(p.line);
                agg.over_odds.push_back(p.over_odds);
                agg.under_odds.push_back(p.under_odds);
            }

            auto median = [](std::vector<double>& v) -> double {
                if (v.empty()) return 0.0;
                if (v.size() == 1) return v[0];
                if (v.size() == 2) return (v[0] + v[1]) / 2.0;
                std::sort(v.begin(), v.end());
                size_t n = v.size();
                return (n % 2 == 0) ? (v[n/2 - 1] + v[n/2]) / 2.0 : v[n/2];
            };

            // Merge by player_id
            std::unordered_map<int, size_t> id_to_idx;
            std::vector<RawAgg> merged;
            for (auto& [name, agg] : player_agg) {
                if (agg.player_id != 0 && id_to_idx.count(agg.player_id)) {
                    auto& existing = merged[id_to_idx[agg.player_id]];
                    existing.lines.insert(existing.lines.end(), agg.lines.begin(), agg.lines.end());
                    existing.over_odds.insert(existing.over_odds.end(), agg.over_odds.begin(), agg.over_odds.end());
                    existing.under_odds.insert(existing.under_odds.end(), agg.under_odds.begin(), agg.under_odds.end());
                } else {
                    if (agg.player_id != 0) id_to_idx[agg.player_id] = merged.size();
                    merged.push_back(std::move(agg));
                }
            }

            for (auto& agg : merged) {
                AggregatedProp ap;
                ap.player_name = std::move(agg.player_name);
                ap.player_id = agg.player_id;
                ap.line = median(agg.lines);
                ap.over_odds = median(agg.over_odds);
                ap.under_odds = median(agg.under_odds);
                fallback_props.push_back(std::move(ap));
            }
            agg_props_ptr = &fallback_props;
        }

        if (agg_props_ptr->empty()) continue;

        // --- Process each player's aggregated prop ---
        for (const auto& prop : *agg_props_ptr) {
            // Look up player in index
            const PlayerStats* ps = nullptr;
            if (prop.player_id != 0) {
                ps = index_.get_by_id(prop.player_id);
            }
            if (!ps) {
                ps = index_.get_by_name(prop.player_name);
            }
            if (!ps) continue;

            // Find the game index for the day BEFORE this prop date.
            int date_idx = ps->find_date_index(date);
            if (date_idx < 0) continue;
            if (date_idx < static_cast<int>(ps->dates.size()) &&
                ps->dates[date_idx] == date) {
                date_idx -= 1;
            }
            if (date_idx < 0) continue;

            auto bet_opt = callback(*ps, date_idx + 1, prop.line,
                                    prop.over_odds, prop.under_odds, date);

            if (bet_opt.has_value()) {
                auto& bet = bet_opt.value();

                bet.bet_size = bet.bet_size * result.bankroll / 1000.0;

                double max_kelly_bet = 0.05 * result.bankroll;
                if (bet.bet_size > max_kelly_bet) {
                    bet.bet_size = max_kelly_bet;
                }
                if (bet.bet_size < 0.50) continue;

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

        double total_wagered = 0.0;
        for (const auto& b : result.bets) {
            total_wagered += b.bet_size;
        }
        result.roi = (total_wagered > 0) ? result.pnl / total_wagered : 0.0;

        double n = result.total_bets;
        double obs_wr = result.win_rate;
        double p0 = 0.5;
        double se = std::sqrt(p0 * (1.0 - p0) / n);
        double pval_binom = 1.0;
        if (se > 1e-9) {
            double z = (obs_wr - p0) / se;
            pval_binom = 0.5 * std::erfc(z / std::sqrt(2.0));
        }

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
