#include "totals.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nba {

// Totals strategy: game-level over/under totals prediction.
// Predicts total points = team1_pace_adjusted_ortg + team2_pace_adjusted_ortg.
// Uses rolling averages of each team's offensive rating and pace.
// Bets when model total differs from market by > min_edge_points.

struct TeamTotalsStats {
    std::vector<double> points_scored;    // per-game points scored
    std::vector<double> points_allowed;   // per-game points allowed
    std::vector<double> possessions;      // estimated possessions per game
    int games = 0;

    // Pace: possessions per game (rolling average)
    double rolling_pace(int window) const {
        if (possessions.empty()) return 100.0;  // NBA average ~100
        int n = static_cast<int>(possessions.size());
        int start = std::max(0, n - window);
        double sum = 0.0;
        int count = 0;
        for (int i = start; i < n; ++i) {
            sum += possessions[i];
            count++;
        }
        return (count > 0) ? sum / count : 100.0;
    }

    // Offensive rating: points per 100 possessions (rolling average)
    double rolling_ortg(int window) const {
        if (points_scored.empty() || possessions.empty()) return 110.0;  // NBA avg
        int n = std::min(static_cast<int>(points_scored.size()),
                          static_cast<int>(possessions.size()));
        int start = std::max(0, n - window);
        double total_pts = 0.0, total_poss = 0.0;
        for (int i = start; i < n; ++i) {
            total_pts += points_scored[i];
            total_poss += possessions[i];
        }
        return (total_poss > 0) ? (total_pts / total_poss) * 100.0 : 110.0;
    }

    // Defensive rating: points allowed per 100 possessions (rolling average)
    double rolling_drtg(int window) const {
        if (points_allowed.empty() || possessions.empty()) return 110.0;
        int n = std::min(static_cast<int>(points_allowed.size()),
                          static_cast<int>(possessions.size()));
        int start = std::max(0, n - window);
        double total_pts = 0.0, total_poss = 0.0;
        for (int i = start; i < n; ++i) {
            total_pts += points_allowed[i];
            total_poss += possessions[i];
        }
        return (total_poss > 0) ? (total_pts / total_poss) * 100.0 : 110.0;
    }
};

// Estimate possessions from box score approximation:
// Poss ≈ FGA - OREB + TOV + 0.44*FTA (Dean Oliver formula, simplified)
// Since we don't have OREB directly, use a simpler proxy: ~95-105 based on game pace
// For now, estimate from points: poss ≈ total_points / 2.1 (average ~1.05 pts/poss per team)
static double estimate_possessions(double team_pts) {
    return team_pts / 1.05;
}

// Approximate t-CDF with df=6 for over/under probability
static double t_cdf_df6(double x) {
    double adjusted = x * (1.0 - 1.0 / 24.0);
    return 0.5 * std::erfc(-adjusted / std::sqrt(2.0));
}

ExperimentResult TotalsStrategy::run(const StrategyConfig& config,
                                       const DataStore& store,
                                       const PlayerIndex& index,
                                       const KalshiCache& kalshi,
                                  const PropCache* prop_cache,
                                  const GameCache* game_cache) {
    (void)index;
    (void)kalshi;

    auto t0 = std::chrono::high_resolution_clock::now();
    ExperimentResult result;
    result.bankroll = 1000.0;

    auto dates = store.get_prop_dates();
    if (dates.empty()) return result;

    const int pace_window = config.pace_window;
    const int ortg_window = config.ortg_window;
    const double nba_total_std = 15.0;  // NBA total points std dev ~15

    std::unordered_map<std::string, TeamTotalsStats> team_stats;
    std::unordered_set<std::string> bet_games;

    for (const auto& date : dates) {
        bet_games.clear();
        const auto& odds_lines = store.get_odds(date);
        if (odds_lines.empty()) continue;

        for (const auto& ol : odds_lines) {
            if (ol.market_type != "totals") continue;

            const auto& home = ol.home_abbr.empty() ? ol.home_team : ol.home_abbr;
            const auto& away = ol.away_abbr.empty() ? ol.away_team : ol.away_abbr;
            if (home.empty() || away.empty()) continue;

            auto& hs = team_stats[home];
            auto& as = team_stats[away];

            // Need minimum games before betting
            if (hs.games < config.min_games || as.games < config.min_games) {
                hs.games++;
                as.games++;
                continue;
            }

            // Predict total using pace-adjusted ratings
            double home_pace = hs.rolling_pace(pace_window);
            double away_pace = as.rolling_pace(pace_window);
            double avg_pace = (home_pace + away_pace) / 2.0;

            double home_ortg = hs.rolling_ortg(ortg_window);
            double away_ortg = as.rolling_ortg(ortg_window);
            double home_drtg = hs.rolling_drtg(ortg_window);
            double away_drtg = as.rolling_drtg(ortg_window);

            // Expected points for each team:
            // Home team scores based on their ORTG vs opponent's DRTG
            double home_pts_expected = ((home_ortg + away_drtg) / 2.0) * (avg_pace / 100.0);
            double away_pts_expected = ((away_ortg + home_drtg) / 2.0) * (avg_pace / 100.0);

            double model_total = home_pts_expected + away_pts_expected;
            double market_total = ol.over_under_point;

            if (market_total < 1.0) {
                hs.games++;
                as.games++;
                continue;  // invalid line
            }

            // Edge in points
            double edge_pts = model_total - market_total;

            if (std::abs(edge_pts) < config.min_edge_points) {
                hs.games++;
                as.games++;
                continue;
            }

            // Probability of going over using t-distribution
            double t_stat = (model_total - market_total) / nba_total_std;
            double p_over = t_cdf_df6(t_stat);

            std::string side;
            double model_prob = 0.0;

            if (edge_pts > config.min_edge_points) {
                side = "OVER";
                model_prob = p_over;
            } else if (edge_pts < -config.min_edge_points) {
                side = "UNDER";
                model_prob = 1.0 - p_over;
            } else {
                hs.games++;
                as.games++;
                continue;
            }

            // Totals bets typically at -110 → 1.909 decimal
            double dec_odds_val = 1.909;
            double home_dec = odds::american_to_decimal(ol.home_odds);
            if (home_dec > 1.01) dec_odds_val = home_dec;

            if (dec_odds_val > config.max_odds || dec_odds_val < 1.01) {
                hs.games++;
                as.games++;
                continue;
            }

            // Kelly sizing
            double b = dec_odds_val - 1.0;
            double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
            kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
            if (kelly_frac < 1e-6) {
                hs.games++;
                as.games++;
                continue;
            }

            double bet_size = kelly_frac * config.kelly * 1000.0;

            // Dedup: one bet per game
            std::string game_key = date + "|" + home + "|" + away;
            if (bet_games.count(game_key)) {
                hs.games++;
                as.games++;
                continue;
            }

            // Resolve bet using actual game result
            const GameResult* gr = game_cache ? game_cache->get(date, home, away) : nullptr;
            if (!gr) {
                hs.games++;
                as.games++;
                continue;
            }

            double actual_total = static_cast<double>(gr->pts + gr->opp_pts);

            Bet bet;
            bet.date = date;
            bet.player = home + " vs " + away;
            bet.stat = "TOTAL";
            bet.line = market_total;
            bet.side = side;
            bet.odds = dec_odds_val;
            bet.bet_size = bet_size;
            bet.actual = actual_total;
            bet.won = (side == "OVER") ? (actual_total > market_total) : (actual_total < market_total);

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

            // Update team stats with ACTUAL results
            hs.points_scored.push_back(gr->pts);
            hs.points_allowed.push_back(gr->opp_pts);
            hs.possessions.push_back(estimate_possessions(gr->pts));

            as.points_scored.push_back(gr->opp_pts);
            as.points_allowed.push_back(gr->pts);
            as.possessions.push_back(estimate_possessions(gr->opp_pts));

            hs.games++;
            as.games++;
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
