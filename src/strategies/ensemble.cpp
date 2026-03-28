#include "ensemble.h"
#include "meanrev.h"
#include "situational.h"
#include "twostage.h"
#include "../engine/walkforward.h"
#include "../features/z_score.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace nba {

// Ensemble strategy: runs meanrev + situational + twostage logic inline
// and takes a majority vote. If 2+ sub-strategies agree on a side, bet
// with averaged Kelly fraction.
//
// Does NOT instantiate sub-strategy objects (that would require separate
// walkforward passes). Instead, inlines the core logic of each in the
// callback for efficiency.

static std::string market_to_stat(const std::string& market) {
    if (market == "player_points")     return "PTS";
    if (market == "player_rebounds")   return "REB";
    if (market == "player_assists")    return "AST";
    if (market == "player_threes")     return "FG3M";
    if (market == "player_steals")     return "STL";
    if (market == "player_blocks")     return "BLK";
    return "PTS";
}

static bool is_b2b(const std::vector<std::string>& dates, int end_idx) {
    if (end_idx < 2) return false;
    const auto& d1 = dates[end_idx - 1];
    const auto& d2 = dates[end_idx - 2];
    if (d1.size() < 10 || d2.size() < 10) return false;
    int y1 = std::stoi(d1.substr(0, 4)), m1 = std::stoi(d1.substr(5, 2)),
        day1 = std::stoi(d1.substr(8, 2));
    int y2 = std::stoi(d2.substr(0, 4)), m2 = std::stoi(d2.substr(5, 2)),
        day2 = std::stoi(d2.substr(8, 2));
    int approx1 = y1 * 365 + m1 * 30 + day1;
    int approx2 = y2 * 365 + m2 * 30 + day2;
    return (approx1 - approx2) <= 1;
}

static double weighted_avg(const std::vector<double>& vals, int end_idx, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = std::min(end_idx, (int)vals.size());
    int start = std::max(0, n - window);
    int count = n - start;
    if (count <= 0) return 0.0;
    double wsum = 0.0, wtotal = 0.0;
    for (int i = start; i < n; ++i) {
        double w = static_cast<double>(i - start + 1);
        wsum += vals[i] * w;
        wtotal += w;
    }
    return (wtotal > 1e-9) ? wsum / wtotal : 0.0;
}

// Sub-strategy vote: +1 = OVER, -1 = UNDER, 0 = SKIP
struct SubVote {
    int direction = 0;  // +1 OVER, -1 UNDER, 0 SKIP
    double kelly = 0.0;
};

ExperimentResult EnsembleStrategy::run(const StrategyConfig& config,
                                        const DataStore& store,
                                        const PlayerIndex& index,
                                        const KalshiCache& kalshi,
                                  const PropCache* prop_cache) {
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

    auto callback = [&](const PlayerStats& player, int end_idx,
                         double line, double over_odds_ml, double under_odds_ml,
                         const std::string& date) -> std::optional<Bet> {
        if (end_idx < config.min_games) return std::nullopt;

        const auto& stat_vals = player.get_stat(stat_name);
        double z = features::z_score(stat_vals, end_idx,
                                      config.lookback_recent, config.lookback_season);
        double season_avg = features::rolling_avg(stat_vals, end_idx, config.lookback_season);
        double season_std = features::rolling_std(stat_vals, end_idx, config.lookback_season);
        if (season_std < 1e-9) return std::nullopt;

        // --- Sub-strategy 1: MeanRev ---
        SubVote meanrev_vote;
        {
            if (z > config.min_dev) {
                double gap = (line - season_avg) / season_std;
                if (gap >= config.line_gap_threshold) {
                    double hr = features::hit_rate_under(stat_vals, end_idx, line,
                                                          config.hit_rate_window);
                    if (hr >= config.min_hit_rate) {
                        meanrev_vote.direction = -1;
                        meanrev_vote.kelly = hr;
                    }
                }
            } else if (z < -config.min_dev) {
                double gap = (season_avg - line) / season_std;
                if (gap >= config.line_gap_threshold) {
                    double hr = features::hit_rate_over(stat_vals, end_idx, line,
                                                         config.hit_rate_window);
                    if (hr >= config.min_hit_rate) {
                        meanrev_vote.direction = 1;
                        meanrev_vote.kelly = hr;
                    }
                }
            }
        }

        // --- Sub-strategy 2: Situational ---
        SubVote sit_vote;
        {
            int over_f = 0, under_f = 0;
            if (z > config.z_thresh) under_f++;
            if (z < -config.z_thresh) over_f++;
            if (config.b2b_enabled && is_b2b(player.dates, end_idx)) under_f++;
            if (end_idx >= 1 && player.minutes[end_idx - 1] > config.fatigue_mins)
                under_f++;
            double line_z = (line - season_avg) / season_std;
            if (line_z > config.line_gap_threshold) under_f++;
            double line_z2 = (season_avg - line) / season_std;
            if (line_z2 > config.line_gap_threshold) over_f++;

            if (under_f >= 2 && under_f > over_f) {
                double hr = features::hit_rate_under(stat_vals, end_idx, line,
                                                      config.hit_rate_window);
                if (hr >= config.min_hit_rate) {
                    sit_vote.direction = -1;
                    sit_vote.kelly = hr;
                }
            } else if (over_f >= 2 && over_f > under_f) {
                double hr = features::hit_rate_over(stat_vals, end_idx, line,
                                                     config.hit_rate_window);
                if (hr >= config.min_hit_rate) {
                    sit_vote.direction = 1;
                    sit_vote.kelly = hr;
                }
            }
        }

        // --- Sub-strategy 3: Twostage ---
        SubVote twostage_vote;
        {
            double pred_mins = weighted_avg(player.minutes, end_idx, config.mins_lookback);
            if (pred_mins >= 1.0) {
                if (is_b2b(player.dates, end_idx))
                    pred_mins *= config.b2b_mins_adj;

                int n = std::min(end_idx, (int)stat_vals.size());
                n = std::min(n, (int)player.minutes.size());
                int start = std::max(0, n - config.rate_lookback);
                double rate_wsum = 0.0, rate_wtotal = 0.0;
                int idx = 0;
                for (int i = start; i < n; ++i) {
                    if (player.minutes[i] > 1.0) {
                        double w = static_cast<double>(idx + 1);
                        rate_wsum += (stat_vals[i] / player.minutes[i]) * w;
                        rate_wtotal += w;
                        idx++;
                    }
                }
                if (rate_wtotal > 1e-9) {
                    double pred_rate = rate_wsum / rate_wtotal;
                    double prediction = pred_mins * pred_rate;
                    double edge = (prediction - line) / std::max(line, 0.5);

                    if (edge > config.min_edge) {
                        double hr = features::hit_rate_over(stat_vals, end_idx, line,
                                                             config.hit_rate_window);
                        if (hr >= config.min_hit_rate) {
                            twostage_vote.direction = 1;
                            twostage_vote.kelly = hr;
                        }
                    } else if (edge < -config.min_edge) {
                        double hr = features::hit_rate_under(stat_vals, end_idx, line,
                                                              config.hit_rate_window);
                        if (hr >= config.min_hit_rate) {
                            twostage_vote.direction = -1;
                            twostage_vote.kelly = hr;
                        }
                    }
                }
            }
        }

        // --- Majority vote ---
        int over_votes = 0, under_votes = 0;
        double kelly_sum = 0.0;
        int vote_count = 0;

        auto tally = [&](const SubVote& v) {
            if (v.direction > 0) { over_votes++; kelly_sum += v.kelly; vote_count++; }
            if (v.direction < 0) { under_votes++; kelly_sum += v.kelly; vote_count++; }
        };
        tally(meanrev_vote);
        tally(sit_vote);
        tally(twostage_vote);

        std::string side;
        double dk_ml = 0.0;

        if (over_votes >= 2 && over_votes > under_votes && allow_over) {
            side = "OVER";
            dk_ml = over_odds_ml;
        } else if (under_votes >= 2 && under_votes > over_votes && allow_under) {
            side = "UNDER";
            dk_ml = under_odds_ml;
        } else {
            return std::nullopt;
        }

        // Averaged hit rate from voting strategies
        double avg_hr = (vote_count > 0) ? kelly_sum / vote_count : 0.0;

        // Resolve odds
        auto resolved = odds::resolve(kalshi, date, player.name,
                                       stat_name, line, side, dk_ml);
        double dec_odds = resolved.decimal;
        if (dec_odds > config.max_odds || dec_odds < 1.01) return std::nullopt;

        // Kelly sizing using averaged hit rate
        double b = dec_odds - 1.0;
        double kelly_frac = (avg_hr * b - (1.0 - avg_hr)) / b;
        kelly_frac = std::max(0.0, std::min(kelly_frac, 0.05));
        if (kelly_frac < 1e-6) return std::nullopt;

        // Unanimity boost: if all 3 agree, increase sizing
        int total_votes = over_votes + under_votes;
        if (total_votes == 3) {
            kelly_frac *= 1.5;
            kelly_frac = std::min(kelly_frac, config.kelly * 2.0);
        }

        Bet bet;
        bet.date = date;
        bet.player = player.name;
        bet.stat = stat_name;
        bet.line = line;
        bet.side = side;
        bet.odds = dec_odds;
        bet.bet_size = kelly_frac * config.kelly * 1000.0;

        return bet;
    };

    return runner.run(config, callback);
}

} // namespace nba
