#include "config/config.h"
#include "data/store.h"
#include "features/player_index.h"
#include "features/z_score.h"
#include "features/odds.h"
#include "strategies/strategy.h"
#include "engine/lab.h"
#include "io/knowledge.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace std::chrono;

// Global lab pointer for signal handler
static nba::Lab* g_lab = nullptr;

static void signal_handler(int sig) {
    const char* name = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
    // Write is async-signal-safe
    write(STDOUT_FILENO, "\nCaught ", 8);
    write(STDOUT_FILENO, name, (sig == SIGINT) ? 6 : 7);
    write(STDOUT_FILENO, ", shutting down gracefully...\n", 30);
    if (g_lab) g_lab->request_stop();
}

// ---- Argument parser ----

struct Args {
    std::string command = "info";
    std::string config_path = "config.json";
    int bench_count = 100;
    bool quiet = false;
    bool verbose = false;
    // For single:
    std::string stat;
    std::string type = "meanrev";
    std::string single_json;

    static Args parse(int argc, char* argv[]);
    static void print_usage();
};

void Args::print_usage() {
    printf(
        "NBA Model Lab v1.0 (C++)\n\n"
        "Commands:\n"
        "  nba_lab run [--config config.json]          "
            "Run the lab (parallel experiments forever)\n"
        "  nba_lab bench [--count N]                    "
            "Benchmark N experiments (default 100)\n"
        "  nba_lab single --config '{...}'              "
            "Run one experiment from JSON config\n"
        "  nba_lab leaderboard                          "
            "Print current leaderboard\n"
        "  nba_lab info                                 "
            "Print data stats + config\n\n"
        "Options:\n"
        "  --config PATH    Config file (default: config.json)\n"
        "  --count N        Number of benchmark experiments (default: 100)\n"
        "  --quiet          Suppress per-experiment output\n"
        "  --verbose        Show detailed per-bet output\n"
        "  -h, --help       Show this help message\n"
    );
}

Args Args::parse(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "run" || a == "bench" || a == "single" ||
            a == "leaderboard" || a == "info") {
            args.command = a;
        } else if (a == "--config" && i + 1 < argc) {
            // For single command, check if JSON
            std::string next = argv[i + 1];
            if (!next.empty() && next[0] == '{') {
                args.single_json = next;
            } else {
                args.config_path = next;
            }
            i++;
        } else if (a == "--count" && i + 1 < argc) {
            args.bench_count = std::atoi(argv[++i]);
            if (args.bench_count <= 0) args.bench_count = 100;
        } else if (a == "--stat" && i + 1 < argc) {
            args.stat = argv[++i];
        } else if (a == "--type" && i + 1 < argc) {
            args.type = argv[++i];
        } else if (a == "--quiet") {
            args.quiet = true;
        } else if (a == "--verbose") {
            args.verbose = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            std::exit(0);
        } else if (args.command == "bench") {
            // Positional arg for bench count
            int n = std::atoi(argv[i]);
            if (n > 0) args.bench_count = n;
        }
    }
    return args;
}

// ---- Data loading ----

struct DataBundle {
    nba::DataStore store;
    nba::PlayerIndex player_index;
    nba::KalshiCache kalshi;
};

static DataBundle load_data(const nba::LabConfig& config) {
    DataBundle db;

    printf("Loading data from: %s\n", config.data_dir.c_str());
    auto t0 = high_resolution_clock::now();
    db.store.load_all(config.data_dir);
    auto t1 = high_resolution_clock::now();

    printf("  Data loaded in %ldms -- %zu players, %zu prop dates, %zu games\n",
           duration_cast<milliseconds>(t1 - t0).count(),
           db.store.num_players(), db.store.num_prop_dates(),
           db.store.num_games());

    auto t2 = high_resolution_clock::now();
    db.player_index.build(db.store);
    auto t3 = high_resolution_clock::now();
    printf("  PlayerIndex: %zu players (%ldms)\n",
           db.player_index.size(),
           duration_cast<milliseconds>(t3 - t2).count());

    auto t4 = high_resolution_clock::now();
    db.kalshi.load(config.kalshi_dir);
    auto t5 = high_resolution_clock::now();
    printf("  KalshiCache: %zu entries (%ldms)\n",
           db.kalshi.size(),
           duration_cast<milliseconds>(t5 - t4).count());

    auto total_ms = duration_cast<milliseconds>(t5 - t0).count();
    printf("  Total load time: %ldms\n\n", total_ms);

    return db;
}

// ---- Main ----

int main(int argc, char* argv[]) {
    auto args = Args::parse(argc, argv);

    // Load config
    auto config = nba::LabConfig::load(args.config_path);
    config.expand_paths();

    // Validate data directory
    if (!std::filesystem::exists(config.data_dir)) {
        fprintf(stderr, "Error: data directory not found: %s\n", config.data_dir.c_str());
        fprintf(stderr, "Set data_dir in %s or create the directory.\n",
                args.config_path.c_str());
        return 1;
    }

    // For leaderboard command, only need the knowledge base
    if (args.command == "leaderboard") {
        nba::KnowledgeBase kb;
        kb.load(config.knowledge_path);
        auto lb = kb.get_leaderboard();
        if (lb.empty()) {
            printf("No leaderboard entries yet at: %s\n", config.knowledge_path.c_str());
            return 0;
        }

        printf("\n=== KNOWLEDGE BASE LEADERBOARD ===\n");
        printf("Path: %s\n", config.knowledge_path.c_str());
        printf("Experiments run: %d | Runtime: %.1f hours\n\n",
               kb.experiments_run(), kb.total_runtime_hours());
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
        return 0;
    }

    // All other commands need data
    auto db = load_data(config);

    if (args.command == "info") {
        printf("=== CONFIG ===\n");
        printf("  Config file:    %s\n", args.config_path.c_str());
        printf("  Data dir:       %s\n", config.data_dir.c_str());
        printf("  Kalshi dir:     %s\n", config.kalshi_dir.c_str());
        printf("  Output dir:     %s\n", config.output_dir.c_str());
        printf("  Knowledge:      %s\n", config.knowledge_path.c_str());
        printf("  Notify script:  %s\n", config.notify_script.c_str());
        printf("  Fast workers:   %d\n", config.fast_workers);
        printf("  Slow workers:   %d\n", config.slow_workers);
        printf("  Weights:        meanrev=%.0f%% sit=%.0f%% twostage=%.0f%%\n",
               config.meanrev_weight * 100, config.situational_weight * 100,
               config.twostage_weight * 100);
        printf("  Notify:         %s (min ROI: %.1f%%)\n",
               config.notify_enabled ? "on" : "off", config.notify_min_roi * 100);
        printf("  Kalshi fee:     %.1f%%\n", config.kalshi_fee_rate * 100);
        printf("\n");

        printf("=== DATA ===\n");
        printf("  Players:        %zu\n", db.store.num_players());
        printf("  Prop dates:     %zu\n", db.store.num_prop_dates());
        printf("  Total games:    %zu\n", db.store.num_games());
        printf("  Kalshi entries: %zu\n", db.kalshi.size());
        printf("  Hardware cores: %u\n\n", std::thread::hardware_concurrency());

        // Quick sanity check with a known player
        auto& jokic_games = db.store.get_player_games_by_name("Nikola Joki\xc4\x87");
        if (!jokic_games.empty()) {
            printf("=== SANITY CHECK: Nikola Jokic ===\n");
            printf("  Total games: %zu\n", jokic_games.size());
            auto& last = jokic_games.back();
            printf("  Last game:   %s %s -- %.0f pts, %.0f reb, %.0f ast\n",
                   last.game_date.c_str(), last.matchup.c_str(),
                   last.pts, last.reb, last.ast);
        }

    } else if (args.command == "single") {
        if (args.single_json.empty()) {
            fprintf(stderr, "Error: --config '{...}' required for 'single' command\n");
            Args::print_usage();
            return 1;
        }

        auto j = nlohmann::json::parse(args.single_json);
        auto strat_config = nba::StrategyConfig::from_json(j);

        nba::Lab lab(db.store, db.player_index, db.kalshi, config);
        lab.run_single(strat_config);

    } else if (args.command == "bench") {
        nba::Lab lab(db.store, db.player_index, db.kalshi, config);

        // Install signal handler for graceful interrupt
        g_lab = &lab;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        lab.bench(args.bench_count);

    } else if (args.command == "run") {
        nba::Lab lab(db.store, db.player_index, db.kalshi, config);

        // Install signal handler for graceful shutdown
        g_lab = &lab;
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        lab.run();
    }

    return 0;
}
