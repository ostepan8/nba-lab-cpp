#include "data/store.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

    nba::DataStore store;
    auto t0 = high_resolution_clock::now();
    store.load_all(data_dir);
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - t0);

    printf("\n=== NBA Lab Data Store ===\n");
    printf("Loaded in %ldms\n", elapsed.count());
    printf("Players:    %zu\n", store.num_players());
    printf("Prop dates: %zu\n", store.num_prop_dates());
    printf("Total games: %zu\n", store.num_games());

    // Quick sanity check: look up Jokic
    auto& jokic = store.get_player_games_by_name("Nikola Jokić");
    if (!jokic.empty()) {
        printf("\nSanity check — Nikola Jokić:\n");
        printf("  Total games: %zu\n", jokic.size());
        auto& last = jokic.back();
        printf("  Last game: %s %s — %.0f pts, %.0f reb, %.0f ast\n",
               last.game_date.c_str(), last.matchup.c_str(),
               last.pts, last.reb, last.ast);
    }

    return 0;
}
