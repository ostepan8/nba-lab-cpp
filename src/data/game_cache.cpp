#include "game_cache.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <chrono>

namespace fs = std::filesystem;
namespace nba {

void GameCache::build(const std::string& data_dir) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // Load all games_YYYY-YY.csv files
    for (const auto& entry : fs::directory_iterator(data_dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.starts_with("games_") && fname.ends_with(".csv")) {
            load_file(entry.path().string());
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    printf("  GameCache: %zu game results (%ldms)\n", results_.size(), ms);
}

void GameCache::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string line;
    std::getline(f, line); // skip header

    // games CSVs have two rows per game (one per team)
    // We need to pair them by GAME_ID to get both scores.
    // Format: SEASON_ID,TEAM_ID,TEAM_ABBREVIATION,TEAM_NAME,GAME_ID,GAME_DATE,
    //         MATCHUP,WL,MIN,PTS,...,PLUS_MINUS

    struct RawGame {
        std::string game_id;
        std::string date;
        std::string team_abbr;
        std::string matchup; // "BOS vs. DAL" (home) or "DAL @ BOS" (away)
        bool won = false;
        int pts = 0;
        double plus_minus = 0.0;
    };

    // Collect all rows, then pair by game_id
    std::unordered_map<std::string, std::vector<RawGame>> by_game;

    while (std::getline(f, line)) {
        if (line.empty()) continue;

        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, ',')) {
            cols.push_back(col);
        }

        if (cols.size() < 28) continue;

        RawGame rg;
        rg.team_abbr = cols[2];     // TEAM_ABBREVIATION
        rg.game_id = cols[4];       // GAME_ID
        rg.date = cols[5];          // GAME_DATE
        rg.matchup = cols[6];       // MATCHUP
        rg.won = (cols[7] == "W");  // WL
        try { rg.pts = std::stoi(cols[9]); } catch (...) {}           // PTS
        try { rg.plus_minus = std::stod(cols[27]); } catch (...) {}   // PLUS_MINUS

        by_game[rg.game_id].push_back(rg);
    }

    // Pair teams and create GameResult entries
    for (auto& [gid, teams] : by_game) {
        if (teams.size() != 2) continue;

        // Determine home vs away from matchup string
        // "BOS vs. DAL" → BOS is home, DAL is away
        // "DAL @ BOS" → DAL is away, BOS is home
        RawGame* home = nullptr;
        RawGame* away = nullptr;

        for (auto& t : teams) {
            if (t.matchup.find("vs.") != std::string::npos) {
                home = &t;
            } else if (t.matchup.find("@") != std::string::npos) {
                away = &t;
            }
        }

        if (!home || !away) continue;

        GameResult gr;
        gr.date = home->date;
        gr.game_id = gid;
        gr.team_abbr = home->team_abbr;
        gr.opponent_abbr = away->team_abbr;
        gr.is_home = true;
        gr.won = home->won;
        gr.pts = home->pts;
        gr.opp_pts = away->pts;
        gr.margin = home->pts - away->pts;
        gr.plus_minus = home->plus_minus;

        std::string key = make_key(home->date, home->team_abbr, away->team_abbr);
        results_[key] = gr;
    }
}

const GameResult* GameCache::get(const std::string& date,
                                  const std::string& home_abbr,
                                  const std::string& away_abbr) const {
    auto it = results_.find(make_key(date, home_abbr, away_abbr));
    return (it != results_.end()) ? &it->second : nullptr;
}

const GameResult* GameCache::get_by_matchup(const std::string& date,
                                             const std::string& home_team,
                                             const std::string& away_team) const {
    return get(date, home_team, away_team);
}

} // namespace nba
