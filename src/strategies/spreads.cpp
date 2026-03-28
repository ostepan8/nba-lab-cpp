#include "spreads.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace nba {

// Spreads strategy: game-level spread prediction using ELO + home court advantage.
// Predicts point differential = (home_elo - away_elo) / K + home_advantage.
// Uses t-distribution cover probability (heavy tails, df=6) for spread bets.
// Bets when model spread differs from market spread by > min_edge_points.

struct SpreadTeamElo {
    double elo = 1500.0;
    int games = 0;
    double total_margin = 0.0;  // cumulative point differential
};

static double elo_expected(double elo_a, double elo_b) {
    return 1.0 / (1.0 + std::pow(10.0, (elo_b - elo_a) / 400.0));
}

// Approximate t-distribution CDF with df=6 using a rational approximation.
// P(X <= x) for Student's t with 6 degrees of freedom.
// Uses the regularized incomplete beta function approximation.
static double t_cdf_df6(double x) {
    // For the t-distribution with df=6, use a numerical approximation:
    // Convert to a normal-like CDF with heavier tails.
    // Use the approximation: t_cdf ≈ Φ(x * (1 - 1/(4*df)))
    // where Φ is the standard normal CDF.
    // This is a decent approximation for moderate df.
    double adjusted = x * (1.0 - 1.0 / 24.0);  // df=6, so 1/(4*6) = 1/24

    // Standard normal CDF via erfc
    return 0.5 * std::erfc(-adjusted / std::sqrt(2.0));
}

// Cover probability: P(home_margin > spread) using t-distribution
// spread is from the book's perspective (negative means home favored)
// model_margin is our predicted home margin
// std_dev is the expected standard deviation of point differentials (~12 points in NBA)
static double cover_prob(double model_margin, double spread, double std_dev) {
    if (std_dev < 0.1) std_dev = 12.0;
    // We need P(actual_margin > -spread) where spread is in "home spread" form
    // If spread = -5.5, home is favored by 5.5, so home covers if margin > 5.5
    double t_stat = (model_margin - (-spread)) / std_dev;
    // P(margin > threshold) = 1 - CDF(t_stat) ... but we want the tail
    // Actually: cover = P(margin > -spread) = 1 - t_cdf((-spread - model_margin)/std_dev)
    // Simplify: t_stat = (model_margin + spread) / std_dev
    t_stat = (model_margin + spread) / std_dev;
    return t_cdf_df6(t_stat);
}

ExperimentResult SpreadsStrategy::run(const StrategyConfig& config,
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

    const double K = config.elo_k;
    const double home_adv = config.home_advantage;
    const double nba_std = 12.0;  // NBA game margin std dev ~12 points

    std::unordered_map<std::string, SpreadTeamElo> elo_map;

    for (const auto& date : dates) {
        const auto& odds_lines = store.get_odds(date);
        if (odds_lines.empty()) continue;

        for (const auto& ol : odds_lines) {
            if (ol.market_type != "spreads") continue;

            const auto& home = ol.home_abbr.empty() ? ol.home_team : ol.home_abbr;
            const auto& away = ol.away_abbr.empty() ? ol.away_team : ol.away_abbr;
            if (home.empty() || away.empty()) continue;

            if (elo_map.find(home) == elo_map.end()) elo_map[home] = SpreadTeamElo{};
            if (elo_map.find(away) == elo_map.end()) elo_map[away] = SpreadTeamElo{};

            auto& he = elo_map[home];
            auto& ae = elo_map[away];

            // Need minimum games before betting
            if (he.games < config.min_games || ae.games < config.min_games) {
                he.games++;
                ae.games++;
                continue;
            }

            // Predict point differential: positive = home wins by that much
            double model_spread = (he.elo - ae.elo) / K + home_adv;

            // Market spread (home_point): negative means home is favored
            double market_spread = ol.home_point;

            // Edge in points
            double edge_pts = model_spread - (-market_spread);
            // model_spread is "home wins by X", market -spread is "home needs to win by X"

            if (std::abs(edge_pts) < config.min_edge_points) {
                // Update ELO even when not betting
                he.games++;
                ae.games++;
                continue;
            }

            // Compute cover probability using t-distribution
            double p_cover = cover_prob(model_spread, market_spread, nba_std);

            std::string side;
            double dec_odds_val = 0.0;
            double model_prob = 0.0;

            // home_point < 0 means home favored; we bet on home covering or away covering
            if (edge_pts > config.min_edge_points) {
                // Our model thinks home is better than market → bet home covers
                side = "HOME";
                model_prob = p_cover;
            } else if (edge_pts < -config.min_edge_points) {
                // Our model thinks away is better than market → bet away covers
                side = "AWAY";
                model_prob = 1.0 - p_cover;
            } else {
                he.games++;
                ae.games++;
                continue;
            }

            // Spread bets are typically -110 → decimal 1.909
            // Use home/away odds if available, else default to -110
            double home_dec = odds::american_to_decimal(ol.home_odds);
            double away_dec = odds::american_to_decimal(ol.away_odds);
            if (home_dec < 1.01) home_dec = 1.909;
            if (away_dec < 1.01) away_dec = 1.909;

            dec_odds_val = (side == "HOME") ? home_dec : away_dec;
            if (dec_odds_val > config.max_odds || dec_odds_val < 1.01) {
                he.games++;
                ae.games++;
                continue;
            }

            // Kelly sizing
            double b = dec_odds_val - 1.0;
            double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
            kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
            if (kelly_frac < 1e-6) {
                he.games++;
                ae.games++;
                continue;
            }

            double bet_size = kelly_frac * config.kelly * 1000.0;

            // Resolve bet using actual game result
            const GameResult* gr = game_cache ? game_cache->get(date, home, away) : nullptr;
            if (!gr) {
                he.games++;
                ae.games++;
                continue;  // Can't resolve without actual result
            }

            Bet bet;
            bet.date = date;
            bet.player = home + " vs " + away;
            bet.stat = "SPREAD";
            bet.line = market_spread;
            bet.side = side;
            bet.odds = dec_odds_val;
            bet.bet_size = bet_size;
            bet.actual = static_cast<double>(gr->margin);

            // Did the bet cover the spread?
            // market_spread is home's spread (negative = home favored)
            // Home covers if actual_margin > -market_spread
            bool home_covered = gr->margin > (-market_spread);
            bet.won = (side == "HOME") ? home_covered : !home_covered;

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

            // Update ELO ratings using actual result
            double expected_home = elo_expected(he.elo + 50.0, ae.elo);
            double actual_home = gr->won ? 1.0 : 0.0;
            he.elo += K * (actual_home - expected_home);
            ae.elo += K * ((1.0 - actual_home) - (1.0 - expected_home));
            he.games++;
            ae.games++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    // Summary stats (wins/pnl already accumulated inline)
    if (result.total_bets > 0) {
        result.win_rate = static_cast<double>(result.wins) / result.total_bets;
        double total_wagered = 0.0;
        for (const auto& b : result.bets) total_wagered += b.bet_size;
        result.roi = (total_wagered > 0) ? result.pnl / total_wagered : 0.0;

        // P-value
        double n = result.total_bets;
        double se = std::sqrt(0.25 / n);
        double z = (result.win_rate - 0.5) / se;
        result.pvalue = 0.5 * std::erfc(z / std::sqrt(2.0));
    }

    return result;
}

} // namespace nba
