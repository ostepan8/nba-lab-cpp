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

    // Build team history index
    for (auto& [key, gr] : results_) {
        team_games_[gr.team_abbr].push_back(&gr);
        team_games_[gr.opponent_abbr].push_back(&gr);
    }
    // Sort each team's history by date
    for (auto& [team, games] : team_games_) {
        std::sort(games.begin(), games.end(),
            [](const GameResult* a, const GameResult* b) { return a->date < b->date; });
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    printf("  GameCache: %zu game results, %zu teams (%ldms)\n",
           results_.size(), team_games_.size(), ms);
}

void GameCache::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;

    std::string header_line;
    std::getline(f, header_line);

    // Parse header to find column indices dynamically
    std::vector<std::string> headers;
    {
        std::stringstream hss(header_line);
        std::string h;
        while (std::getline(hss, h, ',')) headers.push_back(h);
    }

    int col_abbr = -1, col_game_id = -1, col_date = -1, col_matchup = -1;
    int col_wl = -1, col_pts = -1, col_pm = -1;
    int col_fgm = -1, col_fga = -1, col_fg3m = -1, col_fg3a = -1;
    int col_ftm = -1, col_fta = -1;
    int col_oreb = -1, col_dreb = -1, col_reb = -1;
    int col_ast = -1, col_stl = -1, col_blk = -1, col_tov = -1;
    for (int i = 0; i < (int)headers.size(); i++) {
        if (headers[i] == "TEAM_ABBREVIATION") col_abbr = i;
        else if (headers[i] == "GAME_ID") col_game_id = i;
        else if (headers[i] == "GAME_DATE") col_date = i;
        else if (headers[i] == "MATCHUP") col_matchup = i;
        else if (headers[i] == "WL") col_wl = i;
        else if (headers[i] == "PTS") col_pts = i;
        else if (headers[i] == "PLUS_MINUS") col_pm = i;
        else if (headers[i] == "FGM") col_fgm = i;
        else if (headers[i] == "FGA") col_fga = i;
        else if (headers[i] == "FG3M") col_fg3m = i;
        else if (headers[i] == "FG3A") col_fg3a = i;
        else if (headers[i] == "FTM") col_ftm = i;
        else if (headers[i] == "FTA") col_fta = i;
        else if (headers[i] == "OREB") col_oreb = i;
        else if (headers[i] == "DREB") col_dreb = i;
        else if (headers[i] == "REB") col_reb = i;
        else if (headers[i] == "AST") col_ast = i;
        else if (headers[i] == "STL") col_stl = i;
        else if (headers[i] == "BLK") col_blk = i;
        else if (headers[i] == "TOV") col_tov = i;
    }

    if (col_pts < 0 || col_game_id < 0) return;

    auto safe_int = [](const std::vector<std::string>& cols, int idx) -> int {
        if (idx < 0 || idx >= (int)cols.size()) return 0;
        try { return std::stoi(cols[idx]); } catch (...) { return 0; }
    };

    struct RawGame {
        std::string game_id;
        std::string date;
        std::string team_abbr;
        std::string matchup;
        bool won = false;
        int pts = 0;
        double plus_minus = 0.0;
        int fgm = 0, fga = 0, fg3m = 0, fg3a = 0;
        int ftm = 0, fta = 0;
        int oreb = 0, dreb = 0, reb = 0;
        int ast = 0, stl = 0, blk = 0, tov = 0;
    };

    std::unordered_map<std::string, std::vector<RawGame>> by_game;
    std::string line;

    while (std::getline(f, line)) {
        if (line.empty()) continue;

        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, ',')) {
            cols.push_back(col);
        }

        if ((int)cols.size() <= std::max({col_abbr, col_game_id, col_date, col_matchup, col_wl, col_pts}))
            continue;

        RawGame rg;
        rg.team_abbr = cols[col_abbr];
        rg.game_id = cols[col_game_id];
        rg.date = cols[col_date];
        rg.matchup = cols[col_matchup];
        rg.won = (cols[col_wl] == "W");
        rg.pts = safe_int(cols, col_pts);
        rg.fgm = safe_int(cols, col_fgm);
        rg.fga = safe_int(cols, col_fga);
        rg.fg3m = safe_int(cols, col_fg3m);
        rg.fg3a = safe_int(cols, col_fg3a);
        rg.ftm = safe_int(cols, col_ftm);
        rg.fta = safe_int(cols, col_fta);
        rg.oreb = safe_int(cols, col_oreb);
        rg.dreb = safe_int(cols, col_dreb);
        rg.reb = safe_int(cols, col_reb);
        rg.ast = safe_int(cols, col_ast);
        rg.stl = safe_int(cols, col_stl);
        rg.blk = safe_int(cols, col_blk);
        rg.tov = safe_int(cols, col_tov);
        if (col_pm >= 0 && col_pm < (int)cols.size()) {
            try { rg.plus_minus = std::stod(cols[col_pm]); } catch (...) {}
        }

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
        gr.fgm = home->fgm; gr.fga = home->fga;
        gr.fg3m = home->fg3m; gr.fg3a = home->fg3a;
        gr.ftm = home->ftm; gr.fta = home->fta;
        gr.oreb = home->oreb; gr.dreb = home->dreb; gr.reb = home->reb;
        gr.ast = home->ast; gr.stl = home->stl; gr.blk = home->blk; gr.tov = home->tov;

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

const std::vector<const GameResult*> GameCache::empty_history_;

const std::vector<const GameResult*>& GameCache::team_history(const std::string& team_abbr) const {
    auto it = team_games_.find(team_abbr);
    return (it != team_games_.end()) ? it->second : empty_history_;
}

} // namespace nba
