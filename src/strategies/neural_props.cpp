#include "neural_props.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace nba {

// Neural props strategy: Weighted K-Nearest-Neighbors player stat prediction.
// For each player+game, finds the K most similar historical game contexts
// (same stat, similar recent form, similar opponent strength) and averages
// their outcomes weighted by similarity. Approximates what an MLP would learn
// without requiring backprop infrastructure.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

// A "context vector" for a game: recent form summary used for similarity matching.
struct GameContext {
    double recent_avg;       // avg over last seq_len games
    double recent_std;       // std dev over last seq_len games
    double recent_trend;     // slope: last half avg - first half avg
    double minutes_avg;      // average minutes recently
    double actual_value;     // the outcome (what the player actually scored)
    int game_idx;            // index in player's game array
};

// Build context for a game at index `idx` using `seq_len` prior games.
// Returns nullopt if not enough history.
static std::optional<GameContext> build_context(const std::vector<double>& vals,
                                                 const std::vector<double>& minutes,
                                                 int idx, int seq_len) {
    if (idx < seq_len || idx >= static_cast<int>(vals.size())) return std::nullopt;

    int start = idx - seq_len;
    double sum = 0.0, sum_sq = 0.0;
    double first_half = 0.0, second_half = 0.0;
    double min_sum = 0.0;
    int half = seq_len / 2;

    for (int i = start; i < idx; ++i) {
        sum += vals[i];
        sum_sq += vals[i] * vals[i];
        if (i - start < half) first_half += vals[i];
        else second_half += vals[i];
        min_sum += minutes[i];
    }

    double avg = sum / seq_len;
    double var = (sum_sq / seq_len) - (avg * avg);
    double std_dev = (var > 0) ? std::sqrt(var) : 0.0;
    double trend = (half > 0) ? (second_half / (seq_len - half)) - (first_half / half) : 0.0;

    GameContext ctx;
    ctx.recent_avg = avg;
    ctx.recent_std = std_dev;
    ctx.recent_trend = trend;
    ctx.minutes_avg = min_sum / seq_len;
    ctx.actual_value = vals[idx];
    ctx.game_idx = idx;
    return ctx;
}

// Euclidean distance between two contexts (normalized by scale).
static double context_distance(const GameContext& a, const GameContext& b) {
    double d_avg = a.recent_avg - b.recent_avg;
    double d_std = a.recent_std - b.recent_std;
    double d_trend = a.recent_trend - b.recent_trend;
    double d_mins = (a.minutes_avg - b.minutes_avg) / 10.0;  // scale minutes

    // Weight the features
    return std::sqrt(d_avg * d_avg * 2.0 + d_std * d_std + d_trend * d_trend + d_mins * d_mins);
}

// KNN prediction: find K nearest contexts from historical data, return weighted avg.
static double knn_predict(const std::vector<GameContext>& history,
                            const GameContext& query, int k) {
    if (history.empty()) return query.recent_avg;

    // Compute distances
    struct DistVal {
        double dist;
        double value;
    };
    std::vector<DistVal> distances;
    distances.reserve(history.size());

    for (const auto& h : history) {
        double d = context_distance(query, h);
        distances.push_back({d, h.actual_value});
    }

    // Partial sort to get top-K
    int actual_k = std::min(k, static_cast<int>(distances.size()));
    std::partial_sort(distances.begin(), distances.begin() + actual_k,
                       distances.end(),
                       [](const DistVal& a, const DistVal& b) {
                           return a.dist < b.dist;
                       });

    // Inverse-distance weighted average
    double wsum = 0.0, wtotal = 0.0;
    for (int i = 0; i < actual_k; ++i) {
        double w = 1.0 / (distances[i].dist + 0.01);  // avoid division by zero
        wsum += distances[i].value * w;
        wtotal += w;
    }

    return (wtotal > 1e-9) ? wsum / wtotal : query.recent_avg;
}

ExperimentResult NeuralPropsStrategy::run(const StrategyConfig& config,
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

    // Minimum k_neighbors of 5 to prevent extreme predictions from few neighbors
    const int K = std::max(5, config.k_neighbors);
    const int seq_len = config.seq_len;

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        const auto& vals = player.get_stat(stat_name);
        const auto& mins = player.minutes;

        // Build context for the current game (using history up to end_idx)
        auto query_ctx = build_context(vals, mins, end_idx - 1, seq_len);
        if (!query_ctx) return std::nullopt;
        // We want to predict what happens at end_idx, so the query context
        // represents the state BEFORE the game. Override actual_value.
        query_ctx->actual_value = 0.0;

        // Build historical contexts from all prior games (walk-forward safe)
        // Each historical context at index i uses games [i-seq_len, i) to predict game i.
        std::vector<GameContext> history;
        int max_history = std::min(end_idx - 1, static_cast<int>(vals.size()) - 1);
        for (int i = seq_len; i < max_history; ++i) {
            auto ctx = build_context(vals, mins, i, seq_len);
            if (ctx) history.push_back(*ctx);
        }

        if (static_cast<int>(history.size()) < K) return std::nullopt;

        // KNN prediction
        double knn_pred = knn_predict(history, *query_ctx, K);
        if (knn_pred < 0.1) return std::nullopt;

        // Blend prediction with season average (70% KNN + 30% season avg)
        double season_avg = features::rolling_avg(vals, end_idx, config.lookback_season);
        double prediction = 0.7 * knn_pred + 0.3 * season_avg;

        // Cap predicted value: can't differ from line by more than 50%
        double max_pred = line * 1.5;
        double min_pred = line * 0.5;
        prediction = std::max(min_pred, std::min(max_pred, prediction));

        // Edge calculation
        double edge = (prediction - line) / std::max(line, 0.5);

        std::string side;
        double dk_ml = 0.0;

        if (edge > config.min_edge && allow_over) {
            side = "OVER";
            dk_ml = over_odds_ml;
        } else if (edge < -config.min_edge && allow_under) {
            side = "UNDER";
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Hit rate filter
        double hr = (side == "OVER")
            ? features::hit_rate_over(vals, end_idx, line, config.hit_rate_window)
            : features::hit_rate_under(vals, end_idx, line, config.hit_rate_window);
        if (hr < config.min_hit_rate) return std::nullopt;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly sizing
        double b = dec_odds - 1.0;
        double kelly_frac = (hr * b - (1.0 - hr)) / b;
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
