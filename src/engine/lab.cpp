#include "lab.h"
#include "../strategies/meanrev.h"
#include "../strategies/situational.h"
#include "../strategies/twostage.h"
#include "../io/bet_history.h"
#include "../io/notify.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <queue>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cmath>

namespace fs = std::filesystem;

namespace nba {

// ---- Market/stat mapping tables ----

static const std::vector<std::pair<std::string, std::string>> MARKET_STAT_PAIRS = {
    {"player_points",   "PTS"},
    {"player_rebounds",  "REB"},
    {"player_assists",   "AST"},
    {"player_threes",    "FG3M"},
    {"player_steals",    "STL"},
    {"player_blocks",    "BLK"},
};

static const std::vector<std::string> STRATEGY_TYPES = {
    "meanrev", "situational", "twostage"
};

// ---- Constructor ----

Lab::Lab(const DataStore& store, const PlayerIndex& index,
         const KalshiCache& kalshi, int fast_workers, int slow_workers)
    : store_(store), index_(index), kalshi_(kalshi),
      fast_workers_(fast_workers), slow_workers_(slow_workers),
      rng_(std::random_device{}()) {}

// ---- RNG helpers ----

double Lab::rand_double(double lo, double hi) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng_);
}

int Lab::rand_int(int lo, int hi) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng_);
}

std::string Lab::rand_choice(const std::vector<std::string>& v) {
    return v[rand_int(0, static_cast<int>(v.size()) - 1)];
}

// ---- Strategy factory ----

std::unique_ptr<Strategy> Lab::create_strategy(const std::string& type) {
    if (type == "meanrev") {
        return std::make_unique<MeanRevStrategy>();
    }
    if (type == "situational") {
        return std::make_unique<SituationalStrategy>();
    }
    if (type == "twostage") {
        return std::make_unique<TwostageStrategy>();
    }
    // Default fallback
    return std::make_unique<MeanRevStrategy>();
}

// ---- Knowledge base path ----

void Lab::set_knowledge_path(const std::string& path) {
    knowledge_path_ = path;
    knowledge_.load(path);
}

// ---- Hypothesis generation per strategy type ----

StrategyConfig Lab::generate_meanrev_config() {
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

StrategyConfig Lab::generate_situational_config() {
    StrategyConfig c;
    c.type = "situational";

    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    int side_mode = rand_int(0, 2);
    if (side_mode == 0)      c.sides = {"OVER", "UNDER"};
    else if (side_mode == 1) c.sides = {"OVER"};
    else                     c.sides = {"UNDER"};

    // Shared params
    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.35, 0.60);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);
    c.line_gap_threshold = rand_double(0.2, 1.0);

    // Situational-specific params
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

StrategyConfig Lab::generate_twostage_config() {
    StrategyConfig c;
    c.type = "twostage";

    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    int side_mode = rand_int(0, 2);
    if (side_mode == 0)      c.sides = {"OVER", "UNDER"};
    else if (side_mode == 1) c.sides = {"OVER"};
    else                     c.sides = {"UNDER"};

    // Shared params
    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.35, 0.60);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.08);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);

    // Twostage-specific params
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

// ---- Unified hypothesis generation ----

StrategyConfig Lab::generate_hypothesis(const std::string& queue_type) {
    // Distribute across strategy types:
    // 40% meanrev, 30% situational, 30% twostage
    int roll = rand_int(1, 100);
    if (roll <= 40) {
        return generate_meanrev_config();
    } else if (roll <= 70) {
        return generate_situational_config();
    } else {
        return generate_twostage_config();
    }
}

// ---- Evaluate result and update leaderboard ----

void Lab::evaluate_result(const StrategyConfig& config,
                           const ExperimentResult& result) {
    if (result.total_bets < 20) return;

    bool updated = knowledge_.update(config.target_market, result, config);

    if (updated) {
        printf("\n  ** NEW BEST [%s] via %s: ROI=%.1f%% WR=%.1f%% bets=%d p=%.4f **\n",
               config.target_market.c_str(), config.type.c_str(),
               result.roi * 100, result.win_rate * 100,
               result.total_bets, result.pvalue);

        // Persist knowledge base
        if (!knowledge_path_.empty()) {
            knowledge_.save(knowledge_path_);
        }

        // Save bet history for new best
        if (!result.bets.empty()) {
            bet_history::save(config.name, result.bets, "results/bet_history");
        }

        // Send notification if net_roi > 0 (actual profitable result)
        if (result.roi > 0.0) {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                "[FROM FEDORA] NBA Lab C++ breakthrough!\n"
                "Market: %s\nStrategy: %s\n"
                "ROI: %.1f%% | WR: %.1f%% | Bets: %d | p=%.4f",
                config.target_market.c_str(), config.name.c_str(),
                result.roi * 100, result.win_rate * 100,
                result.total_bets, result.pvalue);
            notify::send(msg);
        }
    }
}

// ---- Log experiment to JSONL ----

void Lab::log_experiment(const StrategyConfig& config,
                          const ExperimentResult& result) {
    fs::create_directories("results/lab");

    {
        nlohmann::json entry;
        entry["config"] = config.to_json();
        entry["total_bets"] = result.total_bets;
        entry["wins"] = result.wins;
        entry["win_rate"] = result.win_rate;
        entry["roi"] = result.roi;
        entry["pnl"] = result.pnl;
        entry["pvalue"] = result.pvalue;
        entry["bankroll"] = result.bankroll;
        entry["elapsed_seconds"] = result.elapsed_seconds;

        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream f("results/lab/experiments.jsonl", std::ios::app);
        f << entry.dump() << "\n";
    }

    knowledge_.increment_experiments();
    knowledge_.add_runtime(result.elapsed_seconds / 3600.0);
}

// ---- Print leaderboard ----

void Lab::print_leaderboard() const {
    auto lb = knowledge_.get_leaderboard();
    if (lb.empty()) {
        printf("No leaderboard entries yet.\n");
        return;
    }

    printf("\n=== KNOWLEDGE BASE LEADERBOARD ===\n");
    printf("Experiments run: %d | Runtime: %.1f hours\n\n",
           knowledge_.experiments_run(), knowledge_.total_runtime_hours());
    printf("%-22s %8s %8s %6s %8s  %s\n",
           "Market", "ROI%", "WR%", "Bets", "p-value", "Approach");
    printf("%-22s %8s %8s %6s %8s  %s\n",
           "------", "----", "---", "----", "-------", "--------");

    for (const auto& [market, entry] : lb) {
        printf("%-22s %7.1f%% %7.1f%% %6d %8.4f  %s\n",
               market.c_str(),
               entry.roi * 100, entry.wr * 100,
               entry.bets, entry.pvalue,
               entry.approach.c_str());
    }
    printf("\n");
}

// ---- Run single experiment ----

void Lab::run_single(const StrategyConfig& config) {
    auto strategy = create_strategy(config.type);
    auto result = strategy->run(config, store_, index_, kalshi_);

    printf("[%s] bets=%d WR=%.1f%% ROI=%.1f%% PnL=$%.0f p=%.4f (%.3fs)\n",
           config.name.c_str(),
           result.total_bets, result.win_rate * 100,
           result.roi * 100, result.pnl, result.pvalue,
           result.elapsed_seconds);

    evaluate_result(config, result);
    log_experiment(config, result);
    experiments_run_++;
}

// ---- Benchmark ----

void Lab::bench(int n) {
    printf("\n=== BENCHMARK: %d experiments (meanrev + situational + twostage) ===\n", n);
    printf("Workers: %d fast + %d slow = %d total\n",
           fast_workers_, slow_workers_, fast_workers_ + slow_workers_);

    // Generate all configs upfront — mixed strategy types
    std::vector<StrategyConfig> configs;
    configs.reserve(n);
    for (int i = 0; i < n; i++) {
        configs.push_back(generate_hypothesis("fast"));
    }

    // Count by type
    int n_meanrev = 0, n_sit = 0, n_twostage = 0;
    for (const auto& c : configs) {
        if (c.type == "meanrev") n_meanrev++;
        else if (c.type == "situational") n_sit++;
        else if (c.type == "twostage") n_twostage++;
    }
    printf("Mix: %d meanrev, %d situational, %d twostage\n",
           n_meanrev, n_sit, n_twostage);

    auto t0 = std::chrono::high_resolution_clock::now();

    std::atomic<int> next_idx{0};
    std::atomic<int> completed{0};
    std::atomic<double> total_experiment_time{0.0};

    int num_threads = fast_workers_ + slow_workers_;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&]() {
            while (true) {
                int idx = next_idx.fetch_add(1);
                if (idx >= n) break;

                auto strategy = create_strategy(configs[idx].type);
                auto result = strategy->run(configs[idx], store_, index_, kalshi_);

                evaluate_result(configs[idx], result);
                log_experiment(configs[idx], result);

                double prev = total_experiment_time.load();
                while (!total_experiment_time.compare_exchange_weak(
                    prev, prev + result.elapsed_seconds)) {}

                int done = completed.fetch_add(1) + 1;
                if (done % 10 == 0 || done == n) {
                    printf("  [%d/%d] %s — bets=%d WR=%.1f%% ROI=%.1f%% (%.3fs)\n",
                           done, n, configs[idx].name.c_str(),
                           result.total_bets, result.win_rate * 100,
                           result.roi * 100, result.elapsed_seconds);
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double wall_seconds = std::chrono::duration<double>(t1 - t0).count();
    double avg_per = total_experiment_time.load() / n;

    printf("\n=== BENCHMARK RESULTS ===\n");
    printf("Experiments:     %d\n", n);
    printf("Wall time:       %.2fs\n", wall_seconds);
    printf("Throughput:      %.1f experiments/sec\n", n / wall_seconds);
    printf("Avg per expt:    %.4fs (single-thread)\n", avg_per);
    printf("Threads used:    %d\n", num_threads);
    printf("Speedup:         %.1fx vs sequential\n",
           (avg_per * n) / wall_seconds);

    // Print leaderboard
    print_leaderboard();

    // Save knowledge if path is set
    if (!knowledge_path_.empty()) {
        knowledge_.save(knowledge_path_);
        printf("Knowledge saved to: %s\n", knowledge_path_.c_str());
    }
}

// ---- Run forever ----

void Lab::run() {
    printf("\n=== NBA LAB -- Parallel Experiment Runner ===\n");
    printf("Fast workers: %d | Slow workers: %d\n", fast_workers_, slow_workers_);
    printf("Strategies: meanrev, situational, twostage\n");
    printf("Running experiments forever. Press Ctrl+C to stop.\n\n");

    struct WorkQueue {
        std::queue<StrategyConfig> q;
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> stop{false};
    };

    WorkQueue fast_q, slow_q;

    auto producer = [&]() {
        while (!fast_q.stop.load()) {
            {
                std::lock_guard<std::mutex> lock(fast_q.mtx);
                while (fast_q.q.size() < static_cast<size_t>(fast_workers_ * 2)) {
                    fast_q.q.push(generate_hypothesis("fast"));
                }
            }
            fast_q.cv.notify_all();

            {
                std::lock_guard<std::mutex> lock(slow_q.mtx);
                while (slow_q.q.size() < static_cast<size_t>(slow_workers_ * 2)) {
                    slow_q.q.push(generate_hypothesis("slow"));
                }
            }
            slow_q.cv.notify_all();

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    auto worker = [&](WorkQueue& wq, const std::string& label) {
        while (!wq.stop.load()) {
            StrategyConfig config;
            {
                std::unique_lock<std::mutex> lock(wq.mtx);
                wq.cv.wait(lock, [&] { return !wq.q.empty() || wq.stop.load(); });
                if (wq.stop.load()) break;
                config = std::move(wq.q.front());
                wq.q.pop();
            }

            auto strategy = create_strategy(config.type);
            auto result = strategy->run(config, store_, index_, kalshi_);

            int exp_num = experiments_run_.fetch_add(1) + 1;
            printf("[#%d %s] %s — bets=%d WR=%.1f%% ROI=%.1f%% p=%.4f (%.3fs)\n",
                   exp_num, label.c_str(), config.name.c_str(),
                   result.total_bets, result.win_rate * 100,
                   result.roi * 100, result.pvalue,
                   result.elapsed_seconds);

            evaluate_result(config, result);
            log_experiment(config, result);

            // Periodic save every 50 experiments
            if (exp_num % 50 == 0 && !knowledge_path_.empty()) {
                knowledge_.save(knowledge_path_);
            }
        }
    };

    std::thread prod_thread(producer);

    std::vector<std::thread> fast_threads;
    for (int i = 0; i < fast_workers_; i++) {
        fast_threads.emplace_back(worker, std::ref(fast_q), "FAST");
    }

    std::vector<std::thread> slow_threads;
    for (int i = 0; i < slow_workers_; i++) {
        slow_threads.emplace_back(worker, std::ref(slow_q), "SLOW");
    }

    prod_thread.join();
    for (auto& t : fast_threads) t.join();
    for (auto& t : slow_threads) t.join();

    // Final save
    if (!knowledge_path_.empty()) {
        knowledge_.save(knowledge_path_);
    }
}

} // namespace nba
