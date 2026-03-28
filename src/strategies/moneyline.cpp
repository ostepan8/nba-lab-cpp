#include "moneyline.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nba {

struct TeamElo {
    double elo = 1500.0;
    int wins = 0;
    int losses = 0;
};

static double elo_expected(double elo_a, double elo_b) {
    return 1.0 / (1.0 + std::pow(10.0, (elo_b - elo_a) / 400.0));
}

ExperimentResult MoneylineStrategy::run(const StrategyConfig& config,
                                         const DataStore& store,
                                         const PlayerIndex& index,
                                         const KalshiCache& kalshi,
                                  const PropCache* prop_cache,
                                  const GameCache* game_cache) {
    (void)index;
    (void)kalshi;
    (void)prop_cache;

    auto t0 = std::chrono::high_resolution_clock::now();
    ExperimentResult result;
    result.bankroll = 1000.0;

    auto dates = store.get_prop_dates();
    if (dates.empty()) return result;

    const double K = config.elo_k > 0 ? config.elo_k : 20.0;
    const double home_advantage = 50.0;

    std::unordered_map<std::string, TeamElo> elo_map;
    std::unordered_set<std::string> bet_games;
    std::unordered_set<std::string> elo_updated;

    for (const auto& date : dates) {
        bet_games.clear();
        elo_updated.clear();
        const auto& odds_lines = store.get_odds(date);
        if (odds_lines.empty()) continue;

        for (const auto& ol : odds_lines) {
            if (ol.market_type != "h2h") continue;

            const auto& home = ol.home_abbr.empty() ? ol.home_team : ol.home_abbr;
            const auto& away = ol.away_abbr.empty() ? ol.away_team : ol.away_abbr;
            if (home.empty() || away.empty()) continue;

            if (elo_map.find(home) == elo_map.end()) elo_map[home] = TeamElo{};
            if (elo_map.find(away) == elo_map.end()) elo_map[away] = TeamElo{};

            auto& he = elo_map[home];
            auto& ae = elo_map[away];

            // Look up actual game result
            const GameResult* gr = game_cache ? game_cache->get(date, home, away) : nullptr;

            // Need minimum games AND game result to bet
            int min_games_needed = config.min_games;
            bool can_bet = (he.wins + he.losses >= min_games_needed &&
                           ae.wins + ae.losses >= min_games_needed && gr);

            if (can_bet) {
                // Our win probability estimate
                double home_elo = he.elo + home_advantage;
                double p_home_win = elo_expected(home_elo, ae.elo);

                // Implied probabilities from odds
                double home_dec = odds::american_to_decimal(ol.home_odds);
                double away_dec = odds::american_to_decimal(ol.away_odds);
                if (home_dec >= 1.01 && away_dec >= 1.01) {
                    double impl_home = 1.0 / home_dec;
                    double impl_away = 1.0 / away_dec;
                    double total_impl = impl_home + impl_away;
                    if (total_impl > 0.01) {
                        impl_home /= total_impl;
                        impl_away /= total_impl;

                        double home_edge = p_home_win - impl_home;
                        double away_edge = (1.0 - p_home_win) - impl_away;

                        std::string side;
                        double dec_odds_val = 0.0;
                        double model_prob = 0.0;

                        if (home_edge > config.min_edge) {
                            side = "HOME";
                            dec_odds_val = home_dec;
                            model_prob = p_home_win;
                        } else if (away_edge > config.min_edge) {
                            side = "AWAY";
                            dec_odds_val = away_dec;
                            model_prob = 1.0 - p_home_win;
                        }

                        if (!side.empty() && dec_odds_val <= config.max_odds) {
                            std::string game_key = date + "|" + home + "|" + away;
                            if (bet_games.count(game_key)) continue;
                            double b = dec_odds_val - 1.0;
                            double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
                            kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));

                            if (kelly_frac > 1e-6) {
                                double bet_size = kelly_frac * config.kelly * 1000.0;

                                Bet bet;
                                bet.date = date;
                                bet.player = home + " vs " + away;
                                bet.stat = "ML";
                                bet.line = 0.0;
                                bet.side = side;
                                bet.odds = dec_odds_val;
                                bet.bet_size = bet_size;
                                bet.actual = gr->won ? 1.0 : 0.0;
                                bet.won = (side == "HOME") ? gr->won : !gr->won;

                                if (bet.won) {
                                    bet.pnl = bet_size * (dec_odds_val - 1.0);
                                    result.wins++;
                                } else {
                                    bet.pnl = -bet_size;
                                }

                                result.bankroll += bet.pnl;
                                result.pnl += bet.pnl;
                                result.bets.push_back(bet);
                                result.total_bets++;
                                bet_games.insert(game_key);
                            }
                        }
                    }
                }
            }

            // Always update ELO from actual results (once per game, not per bookmaker)
            std::string elo_key = date + "|" + home + "|" + away;
            if (gr && !elo_updated.count(elo_key)) {
                elo_updated.insert(elo_key);
                double expected_home = elo_expected(he.elo + home_advantage, ae.elo);
                double actual_home = gr->won ? 1.0 : 0.0;
                he.elo += K * (actual_home - expected_home);
                ae.elo += K * ((1.0 - actual_home) - (1.0 - expected_home));
                if (gr->won) { he.wins++; ae.losses++; }
                else { he.losses++; ae.wins++; }
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    if (result.total_bets > 0) {
        result.win_rate = static_cast<double>(result.wins) / result.total_bets;
        double total_wagered = 0.0;
        for (const auto& b : result.bets) total_wagered += b.bet_size;
        result.roi = (total_wagered > 0) ? result.pnl / total_wagered : 0.0;

        double n = result.total_bets;
        double se = std::sqrt(0.25 / n);
        double z = (result.win_rate - 0.5) / se;
        result.pvalue = 0.5 * std::erfc(z / std::sqrt(2.0));
    }

    return result;
}

} // namespace nba
