#include "csv_parser.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

namespace nba {

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ',' && !in_quotes) {
            fields.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    fields.push_back(field);
    return fields;
}

double safe_double(const std::string& s) {
    if (s.empty()) return 0.0;
    try { return std::stod(s); }
    catch (...) { return 0.0; }
}

int safe_int(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoi(s); }
    catch (...) { return 0; }
}

// Extract YYYY-MM-DD from "2026-03-24T00:00:00"
static std::string extract_date(const std::string& s) {
    if (s.size() >= 10) return s.substr(0, 10);
    return s;
}

std::vector<PlayerGame> parse_gamelogs(const std::string& data_dir) {
    std::vector<PlayerGame> all;

    for (auto& entry : fs::directory_iterator(data_dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.find("player_gamelog_") != 0 || fname.find(".csv") == std::string::npos)
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        std::getline(f, line); // skip header

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split_csv_line(line);
            if (cols.size() < 30) continue;  // Min columns: some seasons have 32, others 70

            PlayerGame g;
            // Two formats: 32 cols (2022-24) and 70 cols (2025-26)
            // Common: [0]=SEASON, [1]=PLAYER_ID, [2]=PLAYER_NAME
            g.player_id   = safe_int(cols[1]);
            g.player_name = cols[2];

            if (cols.size() >= 70) {
                // 2025-26 extended format (70 cols)
                g.team        = cols[5];
                g.game_date   = extract_date(cols[8]);
                g.matchup     = cols[9];
                g.minutes     = safe_double(cols[11]);
                g.fgm         = safe_double(cols[12]);
                g.fga         = safe_double(cols[13]);
                g.fg3m        = safe_double(cols[15]);
                g.ftm         = safe_double(cols[18]);
                g.fta         = safe_double(cols[19]);
                g.reb         = safe_double(cols[23]);
                g.ast         = safe_double(cols[24]);
                g.tov         = safe_double(cols[25]);
                g.stl         = safe_double(cols[26]);
                g.blk         = safe_double(cols[27]);
                g.pts         = safe_double(cols[31]);
                g.plus_minus  = safe_double(cols[32]);
            } else {
                // Standard format (32 cols): 2022-23, 2023-24, 2024-25
                g.team        = cols[4];   // TEAM_ABBREVIATION
                g.game_date   = extract_date(cols[7]);  // GAME_DATE
                g.matchup     = cols[8];   // MATCHUP
                g.minutes     = safe_double(cols[10]);  // MIN
                g.fgm         = safe_double(cols[11]);
                g.fga         = safe_double(cols[12]);
                g.fg3m        = safe_double(cols[14]);
                g.ftm         = safe_double(cols[17]);
                g.fta         = safe_double(cols[18]);
                g.reb         = safe_double(cols[22]);
                g.ast         = safe_double(cols[23]);
                g.stl         = safe_double(cols[24]);
                g.blk         = safe_double(cols[25]);
                g.tov         = safe_double(cols[26]);
                g.pts         = safe_double(cols[28]);
                g.plus_minus  = safe_double(cols[29]);
            }
            g.is_home     = (g.matchup.find("vs.") != std::string::npos);

            all.push_back(std::move(g));
        }
    }

    return all;
}

std::map<std::string, std::vector<PropLine>> parse_props(const std::string& dir_path) {
    std::map<std::string, std::vector<PropLine>> result;

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path))
        return result;

    for (auto& entry : fs::directory_iterator(dir_path)) {
        std::string fname = entry.path().filename().string();
        if (fname.find("props_") != 0 || fname.find(".csv") == std::string::npos)
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        std::getline(f, line); // skip header

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split_csv_line(line);
            if (cols.size() < 12) continue;

            PropLine p;
            // date,event_id,commence_time,home_team,away_team,player_name,
            // market_type,line,over_odds,under_odds,bookmaker,player_id
            p.date        = cols[0];
            p.player_name = cols[5];
            p.market_type = cols[6];
            p.line        = safe_double(cols[7]);
            p.over_odds   = safe_double(cols[8]);
            p.under_odds  = safe_double(cols[9]);
            p.bookmaker   = cols[10];
            p.player_id   = safe_int(cols[11]);

            result[p.date].push_back(std::move(p));
        }
    }

    return result;
}

std::map<std::string, std::vector<OddsLine>> parse_odds(const std::string& dir_path) {
    std::map<std::string, std::vector<OddsLine>> result;

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path))
        return result;

    for (auto& entry : fs::directory_iterator(dir_path)) {
        std::string fname = entry.path().filename().string();
        if (fname.find("odds_") != 0 || fname.find(".csv") == std::string::npos)
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        std::getline(f, line); // skip header

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split_csv_line(line);
            if (cols.size() < 15) continue;

            OddsLine o;
            // date,game_id,commence_time,home_team,away_team,
            // home_team_id,away_team_id,home_team_abbr,away_team_abbr,
            // bookmaker,market_type,home_odds,away_odds,home_point,over_under_point
            o.date             = cols[0];
            o.home_team        = cols[3];
            o.away_team        = cols[4];
            o.home_abbr        = cols[7];
            o.away_abbr        = cols[8];
            o.market_type      = cols[10];
            o.home_odds        = safe_double(cols[11]);
            o.away_odds        = safe_double(cols[12]);
            o.home_point       = safe_double(cols[13]);
            o.over_under_point = safe_double(cols[14]);

            result[o.date].push_back(std::move(o));
        }
    }

    return result;
}

} // namespace nba
