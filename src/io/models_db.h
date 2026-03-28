#pragma once

#include "knowledge.h"
#include <string>
#include <mutex>

namespace nba {

/// Writes proven configs directly to the unified models.db (SQLite).
/// Thread-safe — can be called from multiple worker threads.
class ModelsDB {
public:
    /// Open (or create) the SQLite database at `path`.
    /// Creates the models table if it doesn't exist.
    bool open(const std::string& path);

    /// Close the database connection.
    void close();

    /// Upsert a proven config into the models table.
    /// Uses INSERT OR REPLACE on the unique name column.
    bool upsert_model(const ProvenConfig& pc);

    bool is_open() const { return db_ != nullptr; }

    ~ModelsDB();

private:
    void* db_ = nullptr;  // sqlite3* (opaque to avoid header inclusion)
    mutable std::mutex mutex_;

    /// Extract stat abbreviation from market name (e.g. "player_points" → "PTS")
    static std::string market_to_stat(const std::string& market);

    /// Extract sides from the strategy config JSON
    static std::string extract_sides(const nlohmann::json& config);
};

} // namespace nba
