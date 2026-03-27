#include "hypothesis.h"
#include <cstdio>

namespace nba {

static const std::vector<std::pair<std::string, std::string>> MARKET_STAT_PAIRS = {
    {"player_points",   "PTS"},
    {"player_rebounds",  "REB"},
    {"player_assists",   "AST"},
    {"player_threes",    "FG3M"},
    {"player_steals",    "STL"},
    {"player_blocks",    "BLK"},
};

HypothesisGenerator::HypothesisGenerator(const LabConfig& config)
    : config_(config), rng_(std::random_device{}()) {}

double HypothesisGenerator::rand_double(double lo, double hi) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng_);
}

int HypothesisGenerator::rand_int(int lo, int hi) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng_);
}

StrategyConfig HypothesisGenerator::generate(const std::string& /*queue_type*/) {
    double total = config_.meanrev_weight + config_.situational_weight
                 + config_.twostage_weight;
    double r = rand_double(0.0, total);

    if (r < config_.meanrev_weight) {
        return generate_meanrev();
    } else if (r < config_.meanrev_weight + config_.situational_weight) {
        return generate_situational();
    } else {
        return generate_twostage();
    }
}

StrategyConfig HypothesisGenerator::generate_meanrev() {
    StrategyConfig c;
    c.type = "meanrev";

    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    int side_mode = rand_int(0, 2);
    if (side_mode == 0)      c.sides = {"OVER", "UNDER"};
    else if (side_mode == 1) c.sides = {"OVER"};
    else                     c.sides = {"UNDER"};

    c.min_dev = rand_double(0.3, 1.5);
    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.4, 0.65);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.10);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);
    c.line_gap_threshold = rand_double(0.2, 1.0);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "meanrev_%s_dev%.1f_lr%d_ls%d_hr%.2f_k%.3f",
                  c.target_stat.c_str(), c.min_dev, c.lookback_recent,
                  c.lookback_season, c.min_hit_rate, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_situational() {
    StrategyConfig c;
    c.type = "situational";

    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    int side_mode = rand_int(0, 2);
    if (side_mode == 0)      c.sides = {"OVER", "UNDER"};
    else if (side_mode == 1) c.sides = {"OVER"};
    else                     c.sides = {"UNDER"};

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.35, 0.60);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);
    c.line_gap_threshold = rand_double(0.2, 1.0);

    c.z_thresh = rand_double(0.5, 1.8);
    c.b2b_enabled = rand_int(0, 1) == 1;
    c.fatigue_mins = rand_double(28.0, 38.0);
    c.defense_thresh = rand_double(0.01, 0.06);
    c.blowout_thresh = rand_double(0.25, 0.50);
    c.injury_boost = rand_int(0, 1) == 1;
    c.cold_bounce = rand_int(0, 1) == 1;
    c.trend_enabled = rand_int(0, 1) == 1;
    c.consistency_thresh = rand_double(0.3, 0.6);
    c.min_factors = rand_int(2, 4);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "sit_%s_zt%.1f_mf%d_k%.3f_b2b%d",
                  c.target_stat.c_str(), c.z_thresh, c.min_factors,
                  c.kelly, c.b2b_enabled ? 1 : 0);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_twostage() {
    StrategyConfig c;
    c.type = "twostage";

    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    int side_mode = rand_int(0, 2);
    if (side_mode == 0)      c.sides = {"OVER", "UNDER"};
    else if (side_mode == 1) c.sides = {"OVER"};
    else                     c.sides = {"UNDER"};

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.35, 0.60);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);

    c.mins_lookback = rand_int(5, 20);
    c.rate_lookback = rand_int(5, 25);
    c.b2b_mins_adj = rand_double(0.85, 0.98);
    c.min_edge = rand_double(0.03, 0.15);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "twostage_%s_ml%d_rl%d_edge%.2f_k%.3f",
                  c.target_stat.c_str(), c.mins_lookback, c.rate_lookback,
                  c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

} // namespace nba
