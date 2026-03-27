#include "data/store.h"
#include "features/player_index.h"
#include "features/z_score.h"
#include "features/odds.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace std::chrono;

static std::string expand_home(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

int main(int argc, char* argv[]) {
    std::string data_dir = "~/Desktop/nba-modeling/data/raw";
    if (argc > 1) data_dir = argv[1];
    data_dir = expand_home(data_dir);

    printf("Loading data from: %s\n", data_dir.c_str());

    // --- Step 1: DataStore ---
    nba::DataStore store;
    auto t0 = high_resolution_clock::now();
    store.load_all(data_dir);
    auto t1 = high_resolution_clock::now();

    printf("\n=== NBA Lab Data Store ===\n");
    printf("Loaded in %ldms\n", duration_cast<milliseconds>(t1 - t0).count());
    printf("Players:    %zu\n", store.num_players());
    printf("Prop dates: %zu\n", store.num_prop_dates());
    printf("Total games: %zu\n", store.num_games());

    // Quick sanity check: look up Jokic
    auto& jokic_games = store.get_player_games_by_name("Nikola Joki\xc4\x87");
    if (!jokic_games.empty()) {
        printf("\nSanity check — Nikola Jokić:\n");
        printf("  Total games: %zu\n", jokic_games.size());
        auto& last = jokic_games.back();
        printf("  Last game: %s %s — %.0f pts, %.0f reb, %.0f ast\n",
               last.game_date.c_str(), last.matchup.c_str(),
               last.pts, last.reb, last.ast);
    }

    // --- Step 2: Feature Engine ---
    printf("\n=== Feature Engine ===\n");

    // 2a. PlayerIndex
    auto t2 = high_resolution_clock::now();
    nba::PlayerIndex player_index;
    player_index.build(store);
    auto t3 = high_resolution_clock::now();
    printf("Player index: %zu players (built in %ldms)\n",
           player_index.size(),
           duration_cast<milliseconds>(t3 - t2).count());

    // 2b. Test z-scores for Jokic PTS
    auto* jokic = player_index.get_by_name("Nikola Joki\xc4\x87");
    if (jokic) {
        printf("\nJokić feature snapshot (as of 2026-03-24):\n");
        int idx = jokic->find_date_index("2026-03-24");
        if (idx >= 0) {
            // Z-score uses games [0..idx+1) — idx+1 so we include the game at idx
            int eidx = idx + 1;
            double z = nba::features::z_score(jokic->pts, eidx, 5, 40);
            double season_avg = nba::features::rolling_avg(jokic->pts, eidx, 40);
            double recent_5 = nba::features::rolling_avg(jokic->pts, eidx, 5);
            double std_dev = nba::features::rolling_std(jokic->pts, eidx, 40);
            printf("  PTS — season avg: %.1f, recent 5: %.1f, std: %.1f, z-score: %.2f\n",
                   season_avg, recent_5, std_dev, z);

            // Rolling stats for other categories
            double reb_avg = nba::features::rolling_avg(jokic->reb, eidx, 10);
            double ast_avg = nba::features::rolling_avg(jokic->ast, eidx, 10);
            printf("  REB avg(10): %.1f, AST avg(10): %.1f\n", reb_avg, ast_avg);

            // Hit rates
            double hr_over_25 = nba::features::hit_rate_over(jokic->pts, eidx, 25.5, 20);
            double hr_under_25 = nba::features::hit_rate_under(jokic->pts, eidx, 25.5, 20);
            printf("  PTS hit rate over 25.5 (L20): %.0f%%, under: %.0f%%\n",
                   hr_over_25 * 100, hr_under_25 * 100);

            // Per-minute rate
            double pm_rate = nba::features::per_minute_rate(jokic->pts, jokic->minutes, eidx, 10);
            printf("  PTS per-minute rate (L10): %.3f\n", pm_rate);

            // Games count
            printf("  Games available at index: %d / %zu total\n", eidx,
                   jokic->num_games());
        } else {
            printf("  No games found on or before 2026-03-24\n");
        }
    } else {
        printf("\nJokić not found in PlayerIndex\n");
    }

    // 2c. Kalshi cache
    auto t4 = high_resolution_clock::now();
    nba::KalshiCache kalshi;
    std::string kalshi_dir = data_dir + "/../kalshi";
    // Also try the direct path
    if (!std::filesystem::exists(kalshi_dir)) {
        kalshi_dir = expand_home("~/Desktop/nba-modeling/data/raw/kalshi");
    }
    kalshi.load(kalshi_dir);
    auto t5 = high_resolution_clock::now();
    printf("Kalshi cache: %zu entries (loaded in %ldms)\n",
           kalshi.size(), duration_cast<milliseconds>(t5 - t4).count());

    // 2d. Test odds resolution
    if (jokic) {
        printf("\nOdds resolution test — Jokić PTS 25.5:\n");

        // Try Kalshi exact
        auto exact = kalshi.get("2026-03-25", "Nikola Joki\xc4\x87", "PTS", 25.0);
        if (exact) {
            printf("  Kalshi exact (line 25.0): yes_price=%.4f\n", *exact);
        }

        // Try interpolation for 25.5
        auto interp = kalshi.interpolate("2026-03-25", "Nikola Joki\xc4\x87", "PTS", 25.5);
        if (interp) {
            printf("  Kalshi interp (line 25.5): yes_price=%.4f\n", *interp);
        }

        // Full resolve: Kalshi-first, DK-fallback
        auto resolved = nba::odds::resolve(kalshi, "2026-03-25",
            "Nikola Joki\xc4\x87", "PTS", 25.5, "OVER", -115.0);
        printf("  Resolved: %.3f decimal odds (source: %s)\n",
               resolved.decimal, resolved.source.c_str());

        // Test american_to_decimal
        printf("\nOdds conversion sanity:\n");
        printf("  -110 → %.3f\n", nba::odds::american_to_decimal(-110.0));
        printf("  +150 → %.3f\n", nba::odds::american_to_decimal(150.0));
        printf("  -200 → %.3f\n", nba::odds::american_to_decimal(-200.0));
        printf("  +100 → %.3f\n", nba::odds::american_to_decimal(100.0));
    }

    printf("\n=== Step 2 Complete ===\n");
    return 0;
}
