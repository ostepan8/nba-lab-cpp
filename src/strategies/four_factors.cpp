#include "four_factors.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <numeric>

namespace nba {

// Rolling team stats computed from GameCache history
struct TeamFactors {
    // Raw box score accumulators (rolling window)
    struct Game {
        int fgm, fga, fg3m, ftm, fta, oreb, tov, pts;
        int opp_fgm, opp_fga, opp_fg3m, opp_ftm, opp_fta, opp_oreb, opp_tov, opp_pts;
    };
    std::vector<Game> games;
    int total_games = 0;

    void add_game(const GameResult& gr, const GameResult* opp_gr) {
        Game g;
        g.fgm = gr.fgm; g.fga = gr.fga; g.fg3m = gr.fg3m;
        g.ftm = gr.ftm; g.fta = gr.fta; g.oreb = gr.oreb;
        g.tov = gr.tov; g.pts = gr.pts;
        // Opponent stats: we need to reconstruct from the game
        // For the home team GameResult, opp stats come from the away team
        if (opp_gr) {
            g.opp_fgm = opp_gr->fgm; g.opp_fga = opp_gr->fga; g.opp_fg3m = opp_gr->fg3m;
            g.opp_ftm = opp_gr->ftm; g.opp_fta = opp_gr->fta; g.opp_oreb = opp_gr->oreb;
            g.opp_tov = opp_gr->tov; g.opp_pts = opp_gr->pts;
        } else {
            g.opp_fgm = 0; g.opp_fga = 0; g.opp_fg3m = 0;
            g.opp_ftm = 0; g.opp_fta = 0; g.opp_oreb = 0;
            g.opp_tov = 0; g.opp_pts = gr.opp_pts;
        }
        games.push_back(g);
        total_games++;
    }

    // Compute four factors over last N games
    struct Factors {
        double efg_pct;     // Effective FG% = (FGM + 0.5*FG3M) / FGA
        double tov_pct;     // Turnover rate = TOV / (FGA + 0.44*FTA + TOV)
        double oreb_pct;    // Offensive rebound rate = OREB / (OREB + opp_DREB)
        double ft_rate;     // Free throw rate = FTM / FGA
        double pts_per_game;
    };

    Factors offensive(int window) const {
        Factors f{0, 0, 0, 0, 0};
        int n = static_cast<int>(games.size());
        int start = std::max(0, n - window);
        if (start >= n) return f;

        int fgm = 0, fga = 0, fg3m = 0, ftm = 0, fta = 0, oreb = 0, tov = 0, pts = 0;
        int opp_dreb = 0;
        for (int i = start; i < n; i++) {
            fgm += games[i].fgm; fga += games[i].fga; fg3m += games[i].fg3m;
            ftm += games[i].ftm; fta += games[i].fta; oreb += games[i].oreb;
            tov += games[i].tov; pts += games[i].pts;
            // opp DREB = opp REB - opp OREB; approximate as opp_fga - opp_fgm (missed shots available)
            opp_dreb += (games[i].opp_fga - games[i].opp_fgm - games[i].opp_oreb);
        }
        int cnt = n - start;
        if (fga > 0) f.efg_pct = (fgm + 0.5 * fg3m) / (double)fga;
        double poss = fga + 0.44 * fta + tov;
        if (poss > 0) f.tov_pct = tov / poss;
        int total_orebs = oreb + std::max(0, opp_dreb);
        if (total_orebs > 0) f.oreb_pct = oreb / (double)total_orebs;
        if (fga > 0) f.ft_rate = ftm / (double)fga;
        if (cnt > 0) f.pts_per_game = pts / (double)cnt;
        return f;
    }

    Factors defensive(int window) const {
        // Opponent's four factors = our defensive performance
        Factors f{0, 0, 0, 0, 0};
        int n = static_cast<int>(games.size());
        int start = std::max(0, n - window);
        if (start >= n) return f;

        int fgm = 0, fga = 0, fg3m = 0, ftm = 0, fta = 0, oreb = 0, tov = 0, pts = 0;
        int our_dreb = 0;
        for (int i = start; i < n; i++) {
            fgm += games[i].opp_fgm; fga += games[i].opp_fga; fg3m += games[i].opp_fg3m;
            ftm += games[i].opp_ftm; fta += games[i].opp_fta; oreb += games[i].opp_oreb;
            tov += games[i].opp_tov; pts += games[i].opp_pts;
            our_dreb += (games[i].fga - games[i].fgm - games[i].oreb);
        }
        int cnt = n - start;
        if (fga > 0) f.efg_pct = (fgm + 0.5 * fg3m) / (double)fga;
        double poss = fga + 0.44 * fta + tov;
        if (poss > 0) f.tov_pct = tov / poss;
        int total_orebs = oreb + std::max(0, our_dreb);
        if (total_orebs > 0) f.oreb_pct = oreb / (double)total_orebs;
        if (fga > 0) f.ft_rate = ftm / (double)fga;
        if (cnt > 0) f.pts_per_game = pts / (double)cnt;
        return f;
    }
};

// Predict points scored by team A against team B
// Uses team A's offensive factors vs team B's defensive factors
static double predict_points(const TeamFactors::Factors& off,
                              const TeamFactors::Factors& def,
                              double league_avg_pts) {
    // Weighted four factors model:
    // eFG% accounts for ~40% of variance
    // TOV% accounts for ~25%
    // OREB% accounts for ~20%
    // FT rate accounts for ~15%
    double off_score = off.efg_pct * 0.40 + (1.0 - off.tov_pct) * 0.25
                     + off.oreb_pct * 0.20 + off.ft_rate * 0.15;
    double def_score = def.efg_pct * 0.40 + (1.0 - def.tov_pct) * 0.25
                     + def.oreb_pct * 0.20 + def.ft_rate * 0.15;

    // Blend: how good is their offense vs how bad is opponent's defense
    // Normalize against league average (~0.50 for eFG, ~0.12 for TOV, etc.)
    double league_factor = 0.50 * 0.40 + 0.88 * 0.25 + 0.25 * 0.20 + 0.20 * 0.15; // ~0.49
    double off_ratio = (league_factor > 0) ? off_score / league_factor : 1.0;
    double def_ratio = (league_factor > 0) ? def_score / league_factor : 1.0;

    // Predicted points = league_avg * off_strength * def_weakness
    return league_avg_pts * off_ratio * (def_ratio / 1.0);
}

ExperimentResult FourFactorsStrategy::run(const StrategyConfig& config,
                                            const DataStore& store,
                                            const PlayerIndex& index,
                                            const KalshiCache& kalshi,
                                            const PropCache* prop_cache,
                                            const GameCache* game_cache) {
    (void)index; (void)kalshi; (void)prop_cache;

    auto t0 = std::chrono::high_resolution_clock::now();
    ExperimentResult result;
    result.bankroll = 1000.0;
    if (!game_cache) return result;

    auto dates = store.get_prop_dates();
    if (dates.empty()) return result;

    const int window = config.ortg_window > 0 ? config.ortg_window : 15;
    const double home_adv_pts = config.home_advantage > 0 ? config.home_advantage : 3.0;
    const double nba_avg_pts = 112.0;
    const double spread_std = 12.0;
    const double total_std = 15.0;

    std::unordered_map<std::string, TeamFactors> team_stats;
    std::unordered_set<std::string> processed_games;

    for (const auto& date : dates) {
        const auto& odds_lines = store.get_odds(date);
        if (odds_lines.empty()) continue;

        std::unordered_set<std::string> bet_games;

        for (const auto& ol : odds_lines) {
            const auto& home = ol.home_abbr.empty() ? ol.home_team : ol.home_abbr;
            const auto& away = ol.away_abbr.empty() ? ol.away_team : ol.away_abbr;
            if (home.empty() || away.empty()) continue;

            std::string game_key = date + "|" + home + "|" + away;

            const GameResult* gr = game_cache->get(date, home, away);
            if (!gr) continue;

            auto& hs = team_stats[home];
            auto& as = team_stats[away];

            // Need minimum games
            if (hs.total_games < config.min_games || as.total_games < config.min_games) {
                // Update stats even when not betting
                if (!processed_games.count(game_key)) {
                    processed_games.insert(game_key);
                    // Build opponent result for stats
                    GameResult opp_gr = *gr;
                    opp_gr.pts = gr->opp_pts; opp_gr.opp_pts = gr->pts;
                    // Swap box score stats (approximate: we only have home perspective)
                    hs.add_game(*gr, nullptr);
                    as.add_game(opp_gr, nullptr);
                }
                continue;
            }

            if (bet_games.count(game_key)) continue;

            // Compute four factors
            auto home_off = hs.offensive(window);
            auto home_def = hs.defensive(window);
            auto away_off = as.offensive(window);
            auto away_def = as.defensive(window);

            // Predict points
            double pred_home_pts = predict_points(home_off, away_def, nba_avg_pts) + home_adv_pts / 2.0;
            double pred_away_pts = predict_points(away_off, home_def, nba_avg_pts) - home_adv_pts / 2.0;
            double pred_margin = pred_home_pts - pred_away_pts;
            double pred_total = pred_home_pts + pred_away_pts;

            // Try all 3 market types for this game
            bool bet_placed = false;
            for (const auto& ol2 : odds_lines) {
                const auto& h2 = ol2.home_abbr.empty() ? ol2.home_team : ol2.home_abbr;
                const auto& a2 = ol2.away_abbr.empty() ? ol2.away_team : ol2.away_abbr;
                if (h2 != home || a2 != away) continue;

                std::string market_key = game_key + "|" + ol2.market_type;
                if (bet_games.count(market_key)) continue;

                std::string side;
                double dec_odds_val = 0.0;
                double model_prob = 0.0;

                if (ol2.market_type == "h2h") {
                    // Moneyline: bet on predicted winner if edge exists
                    double p_home = 1.0 / (1.0 + std::pow(10.0, -pred_margin / 5.0));
                    double home_dec = odds::american_to_decimal(ol2.home_odds);
                    double away_dec = odds::american_to_decimal(ol2.away_odds);
                    if (home_dec < 1.01 || away_dec < 1.01) continue;

                    double impl_home = 1.0 / home_dec;
                    double impl_away = 1.0 / away_dec;
                    double total_impl = impl_home + impl_away;
                    if (total_impl < 0.01) continue;
                    impl_home /= total_impl;

                    if (p_home - impl_home > config.min_edge) {
                        side = "HOME"; dec_odds_val = home_dec; model_prob = p_home;
                    } else if ((1.0 - p_home) - (1.0 - impl_home) > config.min_edge) {
                        side = "AWAY"; dec_odds_val = away_dec; model_prob = 1.0 - p_home;
                    }
                } else if (ol2.market_type == "spreads") {
                    double market_spread = ol2.home_point;
                    double edge_pts = pred_margin - (-market_spread);
                    if (std::abs(edge_pts) < config.min_edge_points) continue;

                    double t = (pred_margin + market_spread) / spread_std;
                    double p_cover = 0.5 * std::erfc(-t / std::sqrt(2.0));

                    double home_dec = odds::american_to_decimal(ol2.home_odds);
                    double away_dec = odds::american_to_decimal(ol2.away_odds);
                    if (home_dec < 1.01) home_dec = 1.909;
                    if (away_dec < 1.01) away_dec = 1.909;

                    if (edge_pts > config.min_edge_points) {
                        side = "HOME"; dec_odds_val = home_dec; model_prob = p_cover;
                    } else if (edge_pts < -config.min_edge_points) {
                        side = "AWAY"; dec_odds_val = away_dec; model_prob = 1.0 - p_cover;
                    }
                } else if (ol2.market_type == "totals") {
                    double market_total = ol2.over_under_point;
                    if (market_total < 1.0) continue;
                    double edge_pts = pred_total - market_total;
                    if (std::abs(edge_pts) < config.min_edge_points) continue;

                    double t = (pred_total - market_total) / total_std;
                    double p_over = 0.5 * std::erfc(-t / std::sqrt(2.0));

                    double dec = odds::american_to_decimal(ol2.home_odds);
                    if (dec < 1.01) dec = 1.909;

                    if (edge_pts > config.min_edge_points) {
                        side = "OVER"; dec_odds_val = dec; model_prob = p_over;
                    } else if (edge_pts < -config.min_edge_points) {
                        side = "UNDER"; dec_odds_val = dec; model_prob = 1.0 - p_over;
                    }
                }

                if (side.empty() || dec_odds_val > config.max_odds || dec_odds_val < 1.01) continue;

                // Kelly sizing
                double b = dec_odds_val - 1.0;
                double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
                kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
                if (kelly_frac < 1e-6) continue;

                double bet_size = kelly_frac * config.kelly * 1000.0;

                // Resolve
                Bet bet;
                bet.date = date;
                bet.player = home + " vs " + away;
                bet.stat = ol2.market_type == "h2h" ? "H2H" :
                           ol2.market_type == "spreads" ? "SPREADS" : "TOTALS";
                bet.odds = dec_odds_val;
                bet.bet_size = bet_size;
                bet.side = side;

                if (ol2.market_type == "h2h") {
                    bet.line = 0; bet.actual = gr->won ? 1.0 : 0.0;
                    bet.won = (side == "HOME") ? gr->won : !gr->won;
                } else if (ol2.market_type == "spreads") {
                    bet.line = ol2.home_point; bet.actual = gr->margin;
                    bool home_covered = gr->margin > (-ol2.home_point);
                    bet.won = (side == "HOME") ? home_covered : !home_covered;
                } else {
                    double actual_total = gr->pts + gr->opp_pts;
                    bet.line = ol2.over_under_point; bet.actual = actual_total;
                    bet.won = (side == "OVER") ? (actual_total > ol2.over_under_point)
                                                : (actual_total < ol2.over_under_point);
                }

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
                bet_games.insert(market_key);
                bet_placed = true;
            }

            // Update team stats (once per game)
            if (!processed_games.count(game_key)) {
                processed_games.insert(game_key);
                GameResult opp_gr = *gr;
                opp_gr.pts = gr->opp_pts; opp_gr.opp_pts = gr->pts;
                hs.add_game(*gr, nullptr);
                as.add_game(opp_gr, nullptr);
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
