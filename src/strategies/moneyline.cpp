#include "moneyline.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace nba {

// Moneyline strategy: game-level prediction using simple ELO + recent form.
// Uses h2h odds from the odds data, not player props.
// Bets when our win probability estimate differs from the implied odds
// probability by more than min_edge.

// Simple ELO system for NBA teams.
struct TeamElo {
    double elo = 1500.0;
    int wins = 0;
    int losses = 0;
};

// Expected score for team A vs team B given ELO ratings.
static double elo_expected(double elo_a, double elo_b) {
    return 1.0 / (1.0 + std::pow(10.0, (elo_b - elo_a) / 400.0));
}

ExperimentResult MoneylineStrategy::run(const StrategyConfig& config,
                                         const DataStore& store,
                                         const PlayerIndex& index,
                                         const KalshiCache& kalshi,
                                  const PropCache* prop_cache) {
    (void)index;  // not used for game-level bets
    (void)kalshi; // ML odds come from odds files

    auto t0 = std::chrono::high_resolution_clock::now();
    ExperimentResult result;
    result.bankroll = 1000.0;

    // Get all prop dates (used to iterate over game dates)
    auto dates = store.get_prop_dates();
    if (dates.empty()) return result;

    // K-factor for ELO updates
    const double K = 20.0;
    const double home_advantage = 50.0;

    // Build ELO ratings and make bets as we go (walk-forward)
    std::unordered_map<std::string, TeamElo> elo_map;

    for (const auto& date : dates) {
        const auto& odds_lines = store.get_odds(date);
        if (odds_lines.empty()) continue;

        for (const auto& ol : odds_lines) {
            if (ol.market_type != "h2h") continue;

            const auto& home = ol.home_team;
            const auto& away = ol.away_team;
            if (home.empty() || away.empty()) continue;

            // Initialize ELO if new team
            if (elo_map.find(home) == elo_map.end()) elo_map[home] = TeamElo{};
            if (elo_map.find(away) == elo_map.end()) elo_map[away] = TeamElo{};

            auto& he = elo_map[home];
            auto& ae = elo_map[away];

            // Need minimum games before betting
            int min_games_needed = config.min_games;
            if (he.wins + he.losses < min_games_needed ||
                ae.wins + ae.losses < min_games_needed) {
                // Still update ELO below if we have game results
                continue;
            }

            // Our win probability estimate (home team perspective)
            double home_elo = he.elo + home_advantage;
            double p_home_win = elo_expected(home_elo, ae.elo);

            // Convert American odds to decimal, then to implied prob
            double home_dec = odds::american_to_decimal(ol.home_odds);
            double away_dec = odds::american_to_decimal(ol.away_odds);
            if (home_dec < 1.01 || away_dec < 1.01) continue;

            double impl_home = 1.0 / home_dec;
            double impl_away = 1.0 / away_dec;

            // Normalize implied probs (remove vig)
            double total_impl = impl_home + impl_away;
            if (total_impl < 0.01) continue;
            impl_home /= total_impl;
            impl_away /= total_impl;

            // Check for edge
            double home_edge = p_home_win - impl_home;
            double away_edge = (1.0 - p_home_win) - impl_away;

            std::string side;
            double dec_odds_val = 0.0;
            double model_prob = 0.0;
            double edge_val = 0.0;

            if (home_edge > config.min_edge) {
                side = "HOME";
                dec_odds_val = home_dec;
                model_prob = p_home_win;
                edge_val = home_edge;
            } else if (away_edge > config.min_edge) {
                side = "AWAY";
                dec_odds_val = away_dec;
                model_prob = 1.0 - p_home_win;
                edge_val = away_edge;
            } else {
                continue;
            }

            if (dec_odds_val > config.max_odds) continue;

            // Kelly sizing
            double b = dec_odds_val - 1.0;
            double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
            kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
            if (kelly_frac < 1e-6) continue;

            double bet_size = kelly_frac * config.kelly * 1000.0;

            // We don't have game results in the odds data to resolve bets
            // in the walkforward sense, so this strategy produces bets
            // but they need to be resolved against actual game outcomes.
            // For now, use the prop dates as a proxy timeline.
            Bet bet;
            bet.date = date;
            bet.player = home + " vs " + away;
            bet.stat = "ML";
            bet.line = 0.0;
            bet.side = side;
            bet.odds = dec_odds_val;
            bet.bet_size = bet_size;
            // Note: won/pnl/actual would need game result resolution
            result.bets.push_back(bet);
            result.total_bets++;
        }

        // Update ELOs based on any game results we can infer
        // (In practice, game results would come from a separate data source.
        // For now, this is a structural placeholder.)
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    // Compute win rate and ROI from resolved bets
    if (result.total_bets > 0) {
        for (const auto& b : result.bets) {
            if (b.won) result.wins++;
            result.pnl += b.pnl;
        }
        result.win_rate = (result.total_bets > 0)
            ? static_cast<double>(result.wins) / result.total_bets : 0.0;
        double total_wagered = 0.0;
        for (const auto& b : result.bets) total_wagered += b.bet_size;
        result.roi = (total_wagered > 0) ? result.pnl / total_wagered : 0.0;
        result.bankroll += result.pnl;
    }

    return result;
}

} // namespace nba
