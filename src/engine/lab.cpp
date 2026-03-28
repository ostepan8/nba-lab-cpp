#include "lab.h"
#include "../strategies/meanrev.h"
#include "../strategies/situational.h"
#include "../strategies/twostage.h"
#include "../strategies/crossmarket.h"
#include "../strategies/meta_ensemble.h"
#include "../strategies/bayesian.h"
#include "../strategies/ml_props.h"
#include "../strategies/moneyline.h"
#include "../strategies/compound.h"
#include "../strategies/residual.h"
#include "../strategies/ensemble.h"
#include "../strategies/timeseries.h"
#include "../strategies/neural_props.h"
#include "../strategies/spreads.h"
#include "../strategies/totals.h"
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
#include <map>

namespace fs = std::filesystem;
namespace nba {

Lab::Lab(const DataStore& store, const PlayerIndex& index,
         const KalshiCache& kalshi, const LabConfig& config,
         const PropCache& prop_cache)
    : store_(store), index_(index), kalshi_(kalshi), prop_cache_(prop_cache),
      config_(config), hypothesis_gen_(config) {
    if (!config_.knowledge_path.empty())
        knowledge_.load(config_.knowledge_path);
    if (!config_.models_db_path.empty())
        models_db_.open(config_.models_db_path);
}

std::unique_ptr<Strategy> Lab::create_strategy(const std::string& type) {
    if (type == "meanrev")        return std::make_unique<MeanRevStrategy>();
    if (type == "situational")    return std::make_unique<SituationalStrategy>();
    if (type == "twostage")       return std::make_unique<TwostageStrategy>();
    if (type == "crossmarket")    return std::make_unique<CrossMarketStrategy>();
    if (type == "meta_ensemble")  return std::make_unique<MetaEnsembleStrategy>();
    if (type == "bayesian")       return std::make_unique<BayesianStrategy>();
    if (type == "ml_props")       return std::make_unique<MlPropsStrategy>();
    if (type == "moneyline")      return std::make_unique<MoneylineStrategy>();
    if (type == "compound")       return std::make_unique<CompoundStrategy>();
    if (type == "residual")       return std::make_unique<ResidualStrategy>();
    if (type == "ensemble")       return std::make_unique<EnsembleStrategy>();
    if (type == "timeseries")     return std::make_unique<TimeseriesStrategy>();
    if (type == "neural_props")   return std::make_unique<NeuralPropsStrategy>();
    if (type == "spreads")        return std::make_unique<SpreadsStrategy>();
    if (type == "totals")         return std::make_unique<TotalsStrategy>();
    return std::make_unique<MeanRevStrategy>();
}

void Lab::request_stop() { running_.store(false); }

void Lab::evaluate_result(const StrategyConfig& config,
                           const ExperimentResult& result) {
    if (result.total_bets < 30) return;

    double net_roi = result.roi - config_.kalshi_fee_rate;

    // Sanity check: flag suspicious results with ROI > 25%
    if (net_roi > 0.25) {
        printf("  [SUSPICIOUS] %s (%s): %.1f%% ROI with %d bets — likely overfit\n",
               config.target_market.c_str(), config.type.c_str(),
               net_roi * 100, result.total_bets);
        return;  // Do not save suspicious results
    }

    // Only save proven configs (p < 0.05, net ROI > 0)
    if (result.pvalue >= 0.05 || net_roi <= 0) return;

    ProvenConfig pc;
    pc.market = config.target_market;
    pc.approach = config.type;
    pc.name = config.name;
    pc.roi = result.roi;
    pc.net_roi = net_roi;
    pc.pvalue = result.pvalue;
    pc.bets = result.total_bets;
    pc.wr = result.win_rate;
    pc.config = config.to_json();

    // knowledge_.add_proven(pc);  // disabled: SQLite is sole storage
    bool top_changed = false;

    printf("\n  ** PROVEN [%s] %s: %.1f%% net | %d bets | p=%.4f **\n",
           config.target_market.c_str(), config.type.c_str(),
           net_roi * 100, result.total_bets, result.pvalue);

    if (!config_.knowledge_path.empty())
        knowledge_.save(config_.knowledge_path);
    if (models_db_.is_open())
        models_db_.upsert_model(pc);
    if (!result.bets.empty())
        bet_history::save(config.name, result.bets, config_.output_dir + "/bet_history");

    // Only notify when top 5 of either leaderboard changes
    if (top_changed && config_.notify_enabled) {
        auto roi_top = knowledge_.top_by_roi(5);
        auto sig_top = knowledge_.top_by_significance(5);

        std::string msg = "[FROM FEDORA] TOP 10 CHANGED\n\nHIGHEST ROI:\n";
        for (int i = 0; i < (int)roi_top.size(); i++) {
            auto& e = roi_top[i];
            char line[256];
            std::snprintf(line, sizeof(line), "%d. %s (%s): %.1f%% net | %d bets | p=%.4f\n",
                i+1, e.market.c_str(), e.approach.c_str(), e.net_roi*100, e.bets, e.pvalue);
            msg += line;
        }
        msg += "\nMOST SIGNIFICANT:\n";
        for (int i = 0; i < (int)sig_top.size(); i++) {
            auto& e = sig_top[i];
            char line[256];
            std::snprintf(line, sizeof(line), "%d. %s (%s): %.1f%% net | %d bets | p=%.6f\n",
                i+1, e.market.c_str(), e.approach.c_str(), e.net_roi*100, e.bets, e.pvalue);
            msg += line;
        }
        notify::send(msg, config_.notify_script);
    }
}

void Lab::log_experiment(const StrategyConfig& config,
                          const ExperimentResult& result) {
    std::string lab_dir = config_.output_dir + "/lab";
    fs::create_directories(lab_dir);
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
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::ofstream f(lab_dir + "/experiments.jsonl", std::ios::app);
        f << entry.dump() << "\n";
    }
    knowledge_.increment_experiments();
    knowledge_.add_runtime(result.elapsed_seconds / 3600.0);
}

void Lab::print_leaderboard() const {
    auto roi_top = knowledge_.top_by_roi(5);
    auto sig_top = knowledge_.top_by_significance(5);
    if (roi_top.empty()) { printf("No proven configs yet.\n"); return; }

    printf("\nExperiments: %d | Proven: %zu\n",
           knowledge_.experiments_run(), knowledge_.all_proven().size());

    printf("\n=== HIGHEST ROI ===\n");
    for (int i = 0; i < (int)roi_top.size(); i++) {
        auto& e = roi_top[i];
        printf("%d. %s (%s): %.1f%% net | %d bets | p=%.4f\n",
               i+1, e.market.c_str(), e.approach.c_str(),
               e.net_roi*100, e.bets, e.pvalue);
    }
    printf("\n=== MOST SIGNIFICANT ===\n");
    for (int i = 0; i < (int)sig_top.size(); i++) {
        auto& e = sig_top[i];
        printf("%d. %s (%s): p=%.6f | %.1f%% net | %d bets\n",
               i+1, e.market.c_str(), e.approach.c_str(),
               e.pvalue, e.net_roi*100, e.bets);
    }
    printf("\n");
}

void Lab::run_single(const StrategyConfig& config) {
    auto strategy = create_strategy(config.type);
    auto result = strategy->run(config, store_, index_, kalshi_, &prop_cache_);
    printf("[%s] bets=%d WR=%.1f%% ROI=%.1f%% PnL=$%.0f p=%.4f (%.3fs)\n",
           config.name.c_str(), result.total_bets, result.win_rate * 100,
           result.roi * 100, result.pnl, result.pvalue, result.elapsed_seconds);
    evaluate_result(config, result);
    // log_experiment(config, result);  // disabled: SQLite is source of truth
    experiments_run_++;
}

void Lab::bench(int n) {
    int nw = config_.fast_workers + config_.slow_workers;
    printf("\n=== BENCHMARK: %d experiments ===\n", n);
    printf("Workers: %d fast + %d slow = %d total\n",
           config_.fast_workers, config_.slow_workers, nw);

    std::vector<StrategyConfig> configs;
    configs.reserve(n);
    for (int i = 0; i < n; i++)
        configs.push_back(hypothesis_gen_.generate("fast"));

    std::map<std::string, int> type_counts;
    for (const auto& c : configs) type_counts[c.type]++;
    printf("Mix:");
    for (const auto& [t, cnt] : type_counts) printf(" %d %s", cnt, t.c_str());
    printf("\n");

    auto t0 = std::chrono::high_resolution_clock::now();
    std::atomic<int> next{0}, done{0};
    std::atomic<double> cpu_time{0.0};
    std::vector<std::thread> threads;
    threads.reserve(nw);

    for (int t = 0; t < nw; t++) {
        threads.emplace_back([&]() {
            while (running_.load()) {
                int idx = next.fetch_add(1);
                if (idx >= n) break;
                auto s = create_strategy(configs[idx].type);
                auto r = s->run(configs[idx], store_, index_, kalshi_, &prop_cache_);
                evaluate_result(configs[idx], r);
                // log_experiment(configs[idx], r);
                double prev = cpu_time.load();
                while (!cpu_time.compare_exchange_weak(prev, prev + r.elapsed_seconds)) {}
                int d = done.fetch_add(1) + 1;
                if (d % 10 == 0 || d == n)
                    printf("  [%d/%d] %s -- bets=%d WR=%.1f%% ROI=%.1f%% (%.3fs)\n",
                           d, n, configs[idx].name.c_str(), r.total_bets,
                           r.win_rate * 100, r.roi * 100, r.elapsed_seconds);
            }
        });
    }
    for (auto& t : threads) t.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double wall = std::chrono::duration<double>(t1 - t0).count();
    double avg = cpu_time.load() / n;
    printf("\n=== BENCHMARK RESULTS ===\n");
    printf("Experiments:     %d\n", n);
    printf("Wall time:       %.2fs\n", wall);
    printf("Throughput:      %.1f experiments/sec\n", n / wall);
    printf("Avg per expt:    %.4fs (single-thread)\n", avg);
    printf("Threads used:    %d\n", nw);
    printf("Speedup:         %.1fx vs sequential\n", (avg * n) / wall);
    print_leaderboard();
    if (!config_.knowledge_path.empty()) {
        knowledge_.save(config_.knowledge_path);
        printf("Knowledge saved to: %s\n", config_.knowledge_path.c_str());
    }
}

void Lab::run() {
    int nw = config_.fast_workers + config_.slow_workers;
    printf("\n=== NBA LAB v1.0 -- Parallel Experiment Runner ===\n");
    printf("Fast workers: %d | Slow workers: %d | Total: %d\n",
           config_.fast_workers, config_.slow_workers, nw);
    printf("Weights: mr=%.0f%% sit=%.0f%% ts2=%.0f%% xm=%.0f%% meta=%.0f%% "
           "bay=%.0f%% mlp=%.0f%% ml=%.0f%% cmp=%.0f%% res=%.0f%% ens=%.0f%% "
           "tseries=%.0f%% neural=%.0f%% spr=%.0f%% tot=%.0f%%\n",
           config_.meanrev_weight * 100, config_.situational_weight * 100,
           config_.twostage_weight * 100, config_.crossmarket_weight * 100,
           config_.meta_weight * 100, config_.bayesian_weight * 100,
           config_.ml_props_weight * 100, config_.moneyline_weight * 100,
           config_.compound_weight * 100, config_.residual_weight * 100,
           config_.ensemble_weight * 100, config_.timeseries_weight * 100,
           config_.neural_weight * 100, config_.spreads_weight * 100,
           config_.totals_weight * 100);
    printf("Output:  %s\n", config_.output_dir.c_str());
    printf("Running experiments. Press Ctrl+C for graceful shutdown.\n");
    if (config_.max_runtime_seconds > 0)
        printf("Auto-stop after %.0f seconds.\n", config_.max_runtime_seconds);
    printf("\n");

    struct WQ {
        std::queue<StrategyConfig> q;
        std::mutex mtx;
        std::condition_variable cv;
    };
    WQ fast_q, slow_q;

    auto t_start_run = std::chrono::steady_clock::now();
    auto producer = [&]() {
        while (running_.load()) {
            // Check max runtime
            if (config_.max_runtime_seconds > 0) {
                double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t_start_run).count();
                if (elapsed >= config_.max_runtime_seconds) {
                    printf("\n  Max runtime (%.0fs) reached. Shutting down gracefully...\n",
                           config_.max_runtime_seconds);
                    running_.store(false);
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(fast_q.mtx);
                while (fast_q.q.size() < size_t(config_.fast_workers * 2))
                    fast_q.q.push(hypothesis_gen_.generate("fast"));
            }
            fast_q.cv.notify_all();
            {
                std::lock_guard<std::mutex> lock(slow_q.mtx);
                while (slow_q.q.size() < size_t(config_.slow_workers * 2))
                    slow_q.q.push(hypothesis_gen_.generate("slow"));
            }
            slow_q.cv.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        fast_q.cv.notify_all();
        slow_q.cv.notify_all();
    };

    auto t_start = std::chrono::steady_clock::now();
    auto worker = [&](WQ& wq, const std::string& label) {
        while (running_.load()) {
            StrategyConfig cfg;
            {
                std::unique_lock<std::mutex> lock(wq.mtx);
                wq.cv.wait_for(lock, std::chrono::milliseconds(500),
                    [&] { return !wq.q.empty() || !running_.load(); });
                if (!running_.load()) break;
                if (wq.q.empty()) continue;
                cfg = std::move(wq.q.front());
                wq.q.pop();
            }
            auto s = create_strategy(cfg.type);
            auto r = s->run(cfg, store_, index_, kalshi_, &prop_cache_);
            int exp = experiments_run_.fetch_add(1) + 1;
            // restored per-experiment output
            (void)exp; (void)label;
            evaluate_result(cfg, r);
            // log_experiment(cfg, r);  // disabled: SQLite is source of truth
            if (exp % 100 == 0) {
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - t_start).count();
                printf("\n--- SUMMARY after %d experiments (%.1f exp/sec, %.0fs) ---\n",
                       exp, exp / elapsed, elapsed);
                print_leaderboard();
            }
            // periodic knowledge save disabled -- SQLite is source of truth
        }
    };

    std::thread prod(producer);
    std::vector<std::thread> fthreads, sthreads;
    for (int i = 0; i < config_.fast_workers; i++)
        fthreads.emplace_back(worker, std::ref(fast_q), "FAST");
    for (int i = 0; i < config_.slow_workers; i++)
        sthreads.emplace_back(worker, std::ref(slow_q), "SLOW");

    prod.join();
    for (auto& t : fthreads) t.join();
    for (auto& t : sthreads) t.join();

    printf("\nShutting down... saving state.\n");
    if (!config_.knowledge_path.empty()) {
        knowledge_.save(config_.knowledge_path);
        printf("Knowledge saved to: %s\n", config_.knowledge_path.c_str());
    }
    printf("Total experiments: %d\n", experiments_run_.load());
    print_leaderboard();
}

} // namespace nba
