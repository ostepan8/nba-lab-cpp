#include "models_db.h"
#include <sqlite3.h>
#include <cstdio>

namespace nba {

ModelsDB::~ModelsDB() { close(); }

bool ModelsDB::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return true;

    sqlite3* raw = nullptr;
    int rc = sqlite3_open(path.c_str(), &raw);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ModelsDB: cannot open %s: %s\n", path.c_str(),
                sqlite3_errmsg(raw));
        if (raw) sqlite3_close(raw);
        return false;
    }
    db_ = raw;

    // WAL mode for concurrent reads
    sqlite3_exec(raw, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);

    // Create table if not exists (matches Python ModelsDB schema exactly)
    const char* ddl = R"SQL(
        CREATE TABLE IF NOT EXISTS models (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT NOT NULL,
            stat TEXT NOT NULL,
            market TEXT NOT NULL,
            sides TEXT DEFAULT 'BOTH',
            config_json TEXT NOT NULL,
            roi_raw REAL DEFAULT 0,
            roi_net REAL DEFAULT 0,
            win_rate REAL DEFAULT 0,
            total_bets INTEGER DEFAULT 0,
            p_value REAL DEFAULT 1.0,
            is_active INTEGER DEFAULT 1,
            source TEXT DEFAULT 'cpp_lab',
            created_at TEXT DEFAULT (datetime('now')),
            updated_at TEXT DEFAULT (datetime('now')),
            UNIQUE(name)
        );
        CREATE INDEX IF NOT EXISTS idx_models_active ON models(is_active);
    )SQL";

    char* err = nullptr;
    rc = sqlite3_exec(raw, ddl, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ModelsDB: schema error: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    printf("  ModelsDB: opened %s\n", path.c_str());
    return true;
}

void ModelsDB::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(static_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
}

bool ModelsDB::upsert_model(const ProvenConfig& pc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    auto* raw = static_cast<sqlite3*>(db_);
    std::string stat = market_to_stat(pc.market);
    std::string sides = extract_sides(pc.config);
    std::string config_str = pc.config.dump();

    // Insert as INACTIVE (is_active=0) -- the leaderboard_watcher promotes
    // models that beat the current baselines. On conflict, update stats but
    // NEVER change is_active (preserve manual activation/deactivation).
    const char* sql = R"SQL(
        INSERT INTO models (name, type, stat, market, sides, config_json,
                           roi_raw, roi_net, win_rate, total_bets, p_value, is_active, source)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 'cpp_lab')
        ON CONFLICT(name) DO UPDATE SET
            config_json=excluded.config_json,
            roi_raw=excluded.roi_raw,
            roi_net=excluded.roi_net,
            win_rate=excluded.win_rate,
            total_bets=excluded.total_bets,
            p_value=excluded.p_value,
            updated_at=datetime('now')
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "ModelsDB: prepare error: %s\n", sqlite3_errmsg(raw));
        return false;
    }

    sqlite3_bind_text(stmt, 1, pc.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pc.approach.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, stat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, pc.market.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, sides.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, config_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 7, pc.roi);
    sqlite3_bind_double(stmt, 8, pc.net_roi);
    sqlite3_bind_double(stmt, 9, pc.wr);
    sqlite3_bind_int(stmt, 10, pc.bets);
    sqlite3_bind_double(stmt, 11, pc.pvalue);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "ModelsDB: insert error: %s\n", sqlite3_errmsg(raw));
        return false;
    }
    return true;
}

std::string ModelsDB::market_to_stat(const std::string& market) {
    if (market == "player_points")   return "PTS";
    if (market == "player_rebounds")  return "REB";
    if (market == "player_assists")   return "AST";
    if (market == "player_threes")    return "FG3M";
    if (market == "player_steals")    return "STL";
    if (market == "player_blocks")    return "BLK";
    if (market == "h2h")              return "H2H";
    if (market == "spreads")          return "SPREADS";
    if (market == "totals")           return "TOTALS";
    // Fallback: uppercase the market name
    std::string s = market;
    for (auto& c : s) c = toupper(c);
    return s;
}

std::string ModelsDB::extract_sides(const nlohmann::json& config) {
    if (!config.is_object()) return "BOTH";

    // Check the config's "sides" field, or infer from name
    if (config.contains("sides")) {
        auto& s = config["sides"];
        if (s.is_array()) {
            std::string result;
            for (auto& v : s) {
                if (!result.empty()) result += ",";
                result += v.get<std::string>();
            }
            return result;
        }
        if (s.is_string()) return s.get<std::string>();
    }

    // Check nested config
    if (config.contains("config") && config["config"].is_object()) {
        return extract_sides(config["config"]);
    }

    return "BOTH";
}

} // namespace nba
