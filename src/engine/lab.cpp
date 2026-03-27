#include "lab.h"
#include "../strategies/meanrev.h"
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

static const std::vector<std::string> SIDE_OPTIONS = {
    "OVER", "UNDER"
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
    // Future: "situational", "combo", etc.
    return std::make_unique<MeanRevStrategy>();
}

// ---- Hypothesis generation ----

StrategyConfig Lab::generate_hypothesis(const std::string& queue_type) {
    StrategyConfig c;
    c.type = "meanrev";

    // Pick a random market/stat pair
    int pair_idx = rand_int(0, static_cast<int>(MARKET_STAT_PAIRS.size()) - 1);
    c.target_market = MARKET_STAT_PAIRS[pair_idx].first;
    c.target_stat = MARKET_STAT_PAIRS[pair_idx].second;

    // Random sides: both, over-only, or under-only
    int side_mode = rand_int(0, 2);
    if (side_mode == 0) {
        c.sides = {"OVER", "UNDER"};
    } else if (side_mode == 1) {
        c.sides = {"OVER"};
    } else {
        c.sides = {"UNDER"};
    }

    // Randomize parameters
    c.min_dev = rand_double(0.3, 1.5);
    c.lookback_recent = rand_int(3, 10);
    c.lookback_season = rand_int(15, 60);
    c.min_hit_rate = rand_double(0.4, 0.65);
    c.min_games = rand_int(8, 25);
    c.kelly = rand_double(0.01, 0.10);
    c.max_odds = rand_double(1.5, 4.0);
    c.hit_rate_window = rand_int(10, 30);
    c.line_gap_threshold = rand_double(0.2, 1.0);

    // Generate a descriptive name
    char buf[256];
    std::snprintf(buf, sizeof(buf), "meanrev_%s_dev%.1f_lr%d_ls%d_hr%.2f_k%.3f",
                  c.target_stat.c_str(), c.min_dev, c.lookback_recent,
                  c.lookback_season, c.min_hit_rate, c.kelly);
    c.name = buf;

    return c;
}

// ---- Evaluate result and update leaderboard ----

void Lab::evaluate_result(const StrategyConfig& config,
                           const ExperimentResult& result) {
    if (result.total_bets < 20) return;  // too few bets to be meaningful

    std::lock_guard<std::mutex> lock(leaderboard_mutex_);
    auto& best = leaderboard_[config.target_market];

    // Update if better ROI with reasonable p-value
    if (result.roi > best.roi && result.pvalue < 0.15) {
        best.roi = result.roi;
        best.pvalue = result.pvalue;
        best.bets = result.total_bets;
        best.config = config;

        printf("\n  ** NEW BEST [%s]: ROI=%.1f%% WR=%.1f%% bets=%d p=%.4f **\n",
               config.target_market.c_str(),
               result.roi * 100, result.win_rate * 100,
               result.total_bets, result.pvalue);
    }
}

// ---- Log experiment to JSONL ----

void Lab::log_experiment(const StrategyConfig& config,
                          const ExperimentResult& result) {
    // Ensure results directories exist
    fs::create_directories("results/lab");
    fs::create_directories("results/bet_history");

    // Append to experiments.jsonl
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

    // Save bet history if there are bets
    if (!result.bets.empty() && !config.name.empty()) {
        nlohmann::json bets_json = nlohmann::json::array();
        for (const auto& b : result.bets) {
            nlohmann::json bj;
            bj["date"] = b.date;
            bj["player"] = b.player;
            bj["stat"] = b.stat;
            bj["line"] = b.line;
            bj["side"] = b.side;
            bj["odds"] = b.odds;
            bj["bet_size"] = b.bet_size;
            bj["won"] = b.won;
            bj["pnl"] = b.pnl;
            bj["actual"] = b.actual;
            bets_json.push_back(std::move(bj));
        }

        std::string path = "results/bet_history/" + config.name + ".jsonl";
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream f(path);
        for (const auto& bj : bets_json) {
            f << bj.dump() << "\n";
        }
    }
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
    printf("\n=== BENCHMARK: %d meanrev experiments ===\n", n);
    printf("Workers: %d fast + %d slow = %d total\n",
           fast_workers_, slow_workers_, fast_workers_ + slow_workers_);

    // Generate all configs upfront
    std::vector<StrategyConfig> configs;
    configs.reserve(n);
    for (int i = 0; i < n; i++) {
        configs.push_back(generate_hypothesis("fast"));
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // Parallel execution using a thread pool with a shared queue
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

                // Atomic add for total time
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
    printf("\n=== LEADERBOARD ===\n");
    {
        std::lock_guard<std::mutex> lock(leaderboard_mutex_);
        for (const auto& [market, best] : leaderboard_) {
            if (best.bets > 0) {
                printf("  %s: ROI=%.1f%% bets=%d p=%.4f (%s)\n",
                       market.c_str(), best.roi * 100, best.bets,
                       best.pvalue, best.config.name.c_str());
            }
        }
    }
}

// ---- Run forever ----

void Lab::run() {
    printf("\n=== NBA LAB — Parallel Experiment Runner ===\n");
    printf("Fast workers: %d | Slow workers: %d\n", fast_workers_, slow_workers_);
    printf("Running experiments forever. Press Ctrl+C to stop.\n\n");

    // Two queues: fast (meanrev, situational) and slow (future heavy strategies)
    struct WorkQueue {
        std::queue<StrategyConfig> q;
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> stop{false};
    };

    WorkQueue fast_q, slow_q;

    // Producer: continuously generate hypotheses
    auto producer = [&]() {
        while (!fast_q.stop.load()) {
            // Keep fast queue fed
            {
                std::lock_guard<std::mutex> lock(fast_q.mtx);
                while (fast_q.q.size() < static_cast<size_t>(fast_workers_ * 2)) {
                    fast_q.q.push(generate_hypothesis("fast"));
                }
            }
            fast_q.cv.notify_all();

            // Keep slow queue fed (placeholder for future strategies)
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

    // Worker: pull from queue, run, log, repeat
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
        }
    };

    // Start producer
    std::thread prod_thread(producer);

    // Start fast workers
    std::vector<std::thread> fast_threads;
    for (int i = 0; i < fast_workers_; i++) {
        fast_threads.emplace_back(worker, std::ref(fast_q), "FAST");
    }

    // Start slow workers
    std::vector<std::thread> slow_threads;
    for (int i = 0; i < slow_workers_; i++) {
        slow_threads.emplace_back(worker, std::ref(slow_q), "SLOW");
    }

    // Wait (runs forever until Ctrl+C)
    prod_thread.join();
    for (auto& t : fast_threads) t.join();
    for (auto& t : slow_threads) t.join();
}

} // namespace nba
