#include "bayesian.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>

namespace nba {

// Bayesian strategy: uses beta-binomial model with league-wide prior.
// Prior: league average over/under rate (e.g., ~50% for a well-set line).
// Likelihood: player-specific historical data.
// Posterior: Bayesian shrinkage toward league average for small samples,
// converges to player-specific rate as data accumulates.
//
// Without LightGBM: purely analytical beta-binomial posterior.
// With LightGBM: LightGBM with Bayesian-inspired features.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// Beta distribution parameters for the prior.
// alpha_prior = prior_strength * league_avg_hit_rate
// beta_prior  = prior_strength * (1 - league_avg_hit_rate)
// Higher prior_strength → more shrinkage toward prior.
struct BetaPosterior {
    double alpha = 1.0;  // pseudo-counts for "over"
    double beta = 1.0;   // pseudo-counts for "under"

    double mean() const { return alpha / (alpha + beta); }

    // Posterior probability of the next event being "over"
    double prob_over() const { return mean(); }
    double prob_under() const { return 1.0 - mean(); }

    // Credible interval width (measure of uncertainty)
    double uncertainty() const {
        double n = alpha + beta;
        if (n < 2.0) return 1.0;
        return std::sqrt(alpha * beta / (n * n * (n + 1.0)));
    }
};

// Update beta posterior with observed hit counts.
static BetaPosterior compute_posterior(const std::vector<double>& vals,
                                        int end_idx, double line, int window,
                                        double prior_strength) {
    BetaPosterior post;
    // Prior: weakly informative centered at 50% (no initial bias)
    post.alpha = prior_strength * 0.5;
    post.beta = prior_strength * 0.5;

    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);

    for (int i = start; i < n; ++i) {
        if (vals[i] > line) {
            post.alpha += 1.0;
        } else {
            post.beta += 1.0;
        }
    }

    return post;
}

// Weighted posterior: recent games count more.
// Uses exponential decay weighting.
static BetaPosterior compute_weighted_posterior(const std::vector<double>& vals,
                                                 int end_idx, double line, int window,
                                                 double prior_strength, double decay) {
    BetaPosterior post;
    post.alpha = prior_strength * 0.5;
    post.beta = prior_strength * 0.5;

    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;

    for (int i = start; i < n; ++i) {
        // Weight: more recent games get exponentially higher weight
        double w = std::exp(decay * (i - start - count + 1));
        if (vals[i] > line) {
            post.alpha += w;
        } else {
            post.beta += w;
        }
    }

    return post;
}

ExperimentResult BayesianStrategy::run(const StrategyConfig& config,
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

    // Bayesian params: prior_strength controls shrinkage
    // Use min_factors as a proxy for prior_strength, but enforce minimum of 10
    // to prevent the posterior from being too easily swayed by small samples
    double prior_strength = std::max(10.0, static_cast<double>(config.min_factors) * 2.0);
    // Decay rate for recency weighting
    double decay = 0.05;

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;
        // Require minimum 30 games in the posterior window to avoid small-sample overconfidence
        if (end_idx < 30) return std::nullopt;

        const auto& vals = player.get_stat(stat_name);

        // Compute weighted posterior from player's game history vs this line
        auto post = compute_weighted_posterior(vals, end_idx, line,
                                                config.lookback_season,
                                                prior_strength, decay);

        // Shrinkage: blend posterior with 50% prior to prevent extreme probabilities
        double raw_p_over = post.prob_over();
        double p_over = raw_p_over * 0.7 + 0.5 * 0.3;
        double p_under = 1.0 - p_over;

        // Uncertainty filter: skip if posterior is too uncertain
        double unc = post.uncertainty();
        if (unc > config.consistency_thresh) return std::nullopt;

        // Determine side
        std::string side;
        double model_prob = 0.0;
        double dk_ml = 0.0;

        if (p_over > p_under && p_over > config.min_hit_rate && allow_over) {
            side = "OVER";
            model_prob = p_over;
            dk_ml = over_odds_ml;
        } else if (p_under > p_over && p_under > config.min_hit_rate && allow_under) {
            side = "UNDER";
            model_prob = p_under;
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Also check the unweighted posterior for confirmation
        auto post_uw = compute_posterior(vals, end_idx, line,
                                          config.hit_rate_window, prior_strength);
        double uw_prob = (side == "OVER") ? post_uw.prob_over() : post_uw.prob_under();
        // Both weighted and unweighted should agree
        if (uw_prob < 0.5) return std::nullopt;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Edge: posterior prob vs implied
        double implied = 1.0 / dec_odds;
        double edge = model_prob - implied;
        if (edge < config.min_edge) return std::nullopt;

        // Kelly sizing using posterior probability — no confidence boost
        double b = dec_odds - 1.0;
        double kelly_frac = (model_prob * b - (1.0 - model_prob)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, config.kelly));
        if (kelly_frac < 1e-6) return std::nullopt;

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = kelly_frac * 1000.0;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
