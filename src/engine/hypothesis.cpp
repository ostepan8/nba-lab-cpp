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
    // Weights: meanrev 20%, situational 15%, twostage 10%, crossmarket 15%,
    //          meta 10%, bayesian 10%, ml_props 5%, moneyline 5%,
    //          compound 3%, residual 3%, ensemble 4%
    double w_meanrev     = config_.meanrev_weight;
    double w_situational = config_.situational_weight;
    double w_twostage    = config_.twostage_weight;
    double w_crossmarket = config_.crossmarket_weight;
    double w_meta        = config_.meta_weight;
    double w_bayesian    = config_.bayesian_weight;
    double w_ml_props    = config_.ml_props_weight;
    double w_moneyline   = config_.moneyline_weight;
    double w_compound    = config_.compound_weight;
    double w_residual    = config_.residual_weight;
    double w_ensemble    = config_.ensemble_weight;

    double total = w_meanrev + w_situational + w_twostage + w_crossmarket
                 + w_meta + w_bayesian + w_ml_props + w_moneyline
                 + w_compound + w_residual + w_ensemble;

    double r = rand_double(0.0, total);

    double cum = 0.0;
    cum += w_meanrev;     if (r < cum) return generate_meanrev();
    cum += w_situational; if (r < cum) return generate_situational();
    cum += w_twostage;    if (r < cum) return generate_twostage();
    cum += w_crossmarket; if (r < cum) return generate_crossmarket();
    cum += w_meta;        if (r < cum) return generate_meta_ensemble();
    cum += w_bayesian;    if (r < cum) return generate_bayesian();
    cum += w_ml_props;    if (r < cum) return generate_ml_props();
    cum += w_moneyline;   if (r < cum) return generate_moneyline();
    cum += w_compound;    if (r < cum) return generate_compound();
    cum += w_residual;    if (r < cum) return generate_residual();
    return generate_ensemble();
}

// Helper: pick a random market/stat pair
static std::pair<std::string, std::string> pick_pair(
    std::function<int(int,int)> rand_int_fn) {
    int idx = rand_int_fn(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    return MARKET_STAT_PAIRS[idx];
}

// Helper: pick random sides
static std::vector<std::string> pick_sides(std::function<int(int,int)> rand_int_fn) {
    int mode = rand_int_fn(0, 2);
    if (mode == 0) return {"OVER", "UNDER"};
    if (mode == 1) return {"OVER"};
    return {"UNDER"};
}

StrategyConfig HypothesisGenerator::generate_meanrev() {
    StrategyConfig c;
    c.type = "meanrev";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

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

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

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

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

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

StrategyConfig HypothesisGenerator::generate_crossmarket() {
    StrategyConfig c;
    c.type = "crossmarket";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.50, 0.65);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.min_edge = rand_double(0.02, 0.10);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "crossmkt_%s_ls%d_hr%.2f_edge%.2f_k%.3f",
                  c.target_stat.c_str(), c.lookback_season,
                  c.min_hit_rate, c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_meta_ensemble() {
    StrategyConfig c;
    c.type = "meta_ensemble";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(20, 60);
    c.min_hit_rate = rand_double(0.50, 0.65);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.min_edge = rand_double(0.02, 0.10);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "meta_%s_lr%d_ls%d_edge%.2f_k%.3f",
                  c.target_stat.c_str(), c.lookback_recent, c.lookback_season,
                  c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_bayesian() {
    StrategyConfig c;
    c.type = "bayesian";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.50, 0.65);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.min_edge = rand_double(0.02, 0.10);
    c.consistency_thresh = rand_double(0.15, 0.40);
    c.min_factors = rand_int(2, 8); // used as prior_strength multiplier

    char buf[256];
    std::snprintf(buf, sizeof(buf), "bayes_%s_ls%d_ct%.2f_pf%d_k%.3f",
                  c.target_stat.c_str(), c.lookback_season,
                  c.consistency_thresh, c.min_factors, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_ml_props() {
    StrategyConfig c;
    c.type = "ml_props";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.40, 0.60);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.mins_lookback = rand_int(5, 20);
    c.rate_lookback = rand_int(5, 25);
    c.min_edge = rand_double(0.03, 0.15);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "mlprops_%s_lr%d_ml%d_edge%.2f_k%.3f",
                  c.target_stat.c_str(), c.lookback_recent,
                  c.mins_lookback, c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_moneyline() {
    StrategyConfig c;
    c.type = "moneyline";

    c.target_market = "h2h";
    c.target_stat = "ML";
    c.sides = {"OVER", "UNDER"}; // HOME/AWAY mapped to OVER/UNDER

    c.min_games = rand_int(10, 30);
    c.kelly = rand_double(0.01, 0.06);
    c.max_odds = rand_double(1.5, 4.0);
    c.min_edge = rand_double(0.03, 0.12);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "moneyline_mg%d_edge%.2f_k%.3f",
                  c.min_games, c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_compound() {
    StrategyConfig c;
    c.type = "compound";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.40, 0.60);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.min_dev = rand_double(0.3, 1.5);
    c.min_factors = rand_int(2, 4);  // min stats that must agree

    char buf[256];
    std::snprintf(buf, sizeof(buf), "compound_%s_dev%.1f_mf%d_k%.3f",
                  c.target_stat.c_str(), c.min_dev, c.min_factors, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_residual() {
    StrategyConfig c;
    c.type = "residual";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.40, 0.60);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.min_dev = rand_double(0.3, 1.5);  // residual z-score threshold
    c.consistency_thresh = rand_double(0.4, 0.7);  // min fraction of residuals same sign

    char buf[256];
    std::snprintf(buf, sizeof(buf), "residual_%s_dev%.1f_ct%.2f_k%.3f",
                  c.target_stat.c_str(), c.min_dev, c.consistency_thresh, c.kelly);
    c.name = buf;
    return c;
}

StrategyConfig HypothesisGenerator::generate_ensemble() {
    StrategyConfig c;
    c.type = "ensemble";

    auto [mkt, stat] = pick_pair([&](int a, int b) { return rand_int(a, b); });
    c.target_market = mkt;
    c.target_stat = stat;
    c.sides = pick_sides([&](int a, int b) { return rand_int(a, b); });

    // Ensemble uses params from all three sub-strategies
    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.40, 0.60);
    c.min_games = rand_int(10, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 3.5);
    c.hit_rate_window = rand_int(10, 30);
    c.line_gap_threshold = rand_double(0.2, 1.0);
    c.min_dev = rand_double(0.3, 1.5);
    c.z_thresh = rand_double(0.5, 1.8);
    c.b2b_enabled = rand_int(0, 1) == 1;
    c.fatigue_mins = rand_double(28.0, 38.0);
    c.mins_lookback = rand_int(5, 20);
    c.rate_lookback = rand_int(5, 25);
    c.b2b_mins_adj = rand_double(0.85, 0.98);
    c.min_edge = rand_double(0.03, 0.15);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "ens_%s_dev%.1f_zt%.1f_edge%.2f_k%.3f",
                  c.target_stat.c_str(), c.min_dev, c.z_thresh,
                  c.min_edge, c.kelly);
    c.name = buf;
    return c;
}

} // namespace nba
