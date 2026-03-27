#include "data/store.h"
#include "features/player_index.h"
#include "features/z_score.h"
#include "features/odds.h"
#include "strategies/strategy.h"
#include "strategies/meanrev.h"
#include "engine/lab.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace std::chrono;

static std::string expand_home(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

static void print_usage() {
    printf("Usage: nba_lab <command> [options]\n\n");
    printf("Commands:\n");
    printf("  run                     Run the lab (parallel experiments forever)\n");
    printf("  single --config '{...}' Run one experiment from JSON config\n");
    printf("  bench  [N]              Benchmark: run N meanrev experiments (default 100)\n");
    printf("  info                    Load data and print summary (original behavior)\n");
    printf("\nOptions:\n");
    printf("  --data <dir>            Data directory (default: ~/Desktop/nba-modeling/data/raw)\n");
    printf("  --fast <N>              Fast worker threads (default: auto)\n");
    printf("  --slow <N>              Slow worker threads (default: 2)\n");
}

struct DataBundle {
    nba::DataStore store;
    nba::PlayerIndex player_index;
    nba::KalshiCache kalshi;
};

static DataBundle load_data(const std::string& data_dir) {
    DataBundle db;

    printf("Loading data from: %s\n", data_dir.c_str());
    auto t0 = high_resolution_clock::now();
    db.store.load_all(data_dir);
    auto t1 = high_resolution_clock::now();

    printf("  Data loaded in %ldms — %zu players, %zu prop dates, %zu games\n",
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
    std::string kalshi_dir = data_dir + "/../kalshi";
    if (!std::filesystem::exists(kalshi_dir)) {
        kalshi_dir = expand_home("~/Desktop/nba-modeling/data/raw/kalshi");
    }
    db.kalshi.load(kalshi_dir);
    auto t5 = high_resolution_clock::now();
    printf("  KalshiCache: %zu entries (%ldms)\n",
           db.kalshi.size(),
           duration_cast<milliseconds>(t5 - t4).count());

    auto total_ms = duration_cast<milliseconds>(t5 - t0).count();
    printf("  Total load time: %ldms\n\n", total_ms);

    return db;
}

int main(int argc, char* argv[]) {
    std::string data_dir = "~/Desktop/nba-modeling/data/raw";
    std::string command = "info";
    std::string config_json;
    int bench_n = 100;
    int fast_workers = static_cast<int>(std::thread::hardware_concurrency());
    if (fast_workers < 2) fast_workers = 6;
    int slow_workers = 2;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "run" || arg == "single" || arg == "bench" || arg == "info") {
            command = arg;
        } else if (arg == "--data" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_json = argv[++i];
        } else if (arg == "--fast" && i + 1 < argc) {
            fast_workers = std::atoi(argv[++i]);
        } else if (arg == "--slow" && i + 1 < argc) {
            slow_workers = std::atoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (command == "bench") {
            // Positional arg after bench = count
            bench_n = std::atoi(argv[i]);
            if (bench_n <= 0) bench_n = 100;
        }
    }

    data_dir = expand_home(data_dir);

    // Load all data
    auto db = load_data(data_dir);

    if (command == "info") {
        // Original info dump
        auto& jokic_games = db.store.get_player_games_by_name("Nikola Joki\xc4\x87");
        if (!jokic_games.empty()) {
            printf("Sanity check — Nikola Jokić:\n");
            printf("  Total games: %zu\n", jokic_games.size());
            auto& last = jokic_games.back();
            printf("  Last game: %s %s — %.0f pts, %.0f reb, %.0f ast\n",
                   last.game_date.c_str(), last.matchup.c_str(),
                   last.pts, last.reb, last.ast);
        }

        auto* jokic = db.player_index.get_by_name("Nikola Joki\xc4\x87");
        if (jokic) {
            int idx = jokic->find_date_index("2026-03-24");
            if (idx >= 0) {
                int eidx = idx + 1;
                double z = nba::features::z_score(jokic->pts, eidx, 5, 40);
                double avg = nba::features::rolling_avg(jokic->pts, eidx, 40);
                double rec = nba::features::rolling_avg(jokic->pts, eidx, 5);
                printf("  PTS z-score: %.2f (season: %.1f, recent5: %.1f)\n",
                       z, avg, rec);
            }
        }

    } else if (command == "single") {
        if (config_json.empty()) {
            fprintf(stderr, "Error: --config required for 'single' command\n");
            print_usage();
            return 1;
        }

        auto j = nlohmann::json::parse(config_json);
        auto config = nba::StrategyConfig::from_json(j);

        nba::Lab lab(db.store, db.player_index, db.kalshi,
                     fast_workers, slow_workers);
        lab.run_single(config);

    } else if (command == "bench") {
        nba::Lab lab(db.store, db.player_index, db.kalshi,
                     fast_workers, slow_workers);
        lab.bench(bench_n);

    } else if (command == "run") {
        nba::Lab lab(db.store, db.player_index, db.kalshi,
                     fast_workers, slow_workers);
        lab.run();
    }

    return 0;
}
