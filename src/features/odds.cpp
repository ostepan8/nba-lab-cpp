#include "odds.h"
#include "../data/csv_parser.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;

namespace nba {

// ---------- KalshiCache ----------

std::string KalshiCache::make_key(const std::string& date, const std::string& player,
                                  const std::string& stat, double line) {
    // Round line to 1 decimal for consistent keys
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", line);
    return date + "|" + player + "|" + stat + "|" + buf;
}

std::string KalshiCache::make_group_key(const std::string& date, const std::string& player,
                                        const std::string& stat) {
    return date + "|" + player + "|" + stat;
}

void KalshiCache::load(const std::string& kalshi_dir) {
    cache_.clear();
    player_lines_.clear();

    if (!fs::exists(kalshi_dir) || !fs::is_directory(kalshi_dir)) {
        printf("  KalshiCache: directory not found: %s\n", kalshi_dir.c_str());
        return;
    }

    size_t rows_loaded = 0;

    for (auto& entry : fs::directory_iterator(kalshi_dir)) {
        std::string fname = entry.path().filename().string();
        // Match kalshi_*_settled.csv
        if (fname.find("kalshi_") != 0 || fname.find("_settled.csv") == std::string::npos)
            continue;

        std::ifstream f(entry.path());
        if (!f.is_open()) continue;

        std::string line;
        std::getline(f, line); // skip header: game_date,stat,player,line,yes_price,result,volume,ticker

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            auto cols = split_csv_line(line);
            if (cols.size() < 5) continue;

            // cols: game_date, stat, player, line, yes_price, result, volume, ticker
            const std::string& game_date = cols[0];
            std::string stat = cols[1];
            const std::string& player = cols[2];
            double line_val = safe_double(cols[3]);
            double yes_price = safe_double(cols[4]);

            // Skip invalid
            if (player.empty() || game_date.empty()) continue;
            if (yes_price < 0.0 || yes_price > 1.0) continue;

            std::string key = make_key(game_date, player, stat, line_val);
            cache_[key] = yes_price;

            std::string gkey = make_group_key(game_date, player, stat);
            player_lines_[gkey].emplace_back(line_val, yes_price);

            ++rows_loaded;
        }
    }

    // Sort each player's lines by line value for interpolation
    for (auto& [gkey, lines] : player_lines_) {
        std::sort(lines.begin(), lines.end());
        // Deduplicate: keep last (in case of duplicates, last write wins)
        auto last = std::unique(lines.begin(), lines.end(),
            [](const auto& a, const auto& b) {
                return std::abs(a.first - b.first) < 0.01;
            });
        lines.erase(last, lines.end());
    }

    printf("  KalshiCache: %zu rows → %zu unique keys, %zu player-date-stat groups\n",
           rows_loaded, cache_.size(), player_lines_.size());
}

std::optional<double> KalshiCache::get(const std::string& date, const std::string& player,
                                       const std::string& stat, double line) const {
    std::string key = make_key(date, player, stat, line);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    return std::nullopt;
}

std::optional<double> KalshiCache::interpolate(const std::string& date, const std::string& player,
                                                const std::string& stat, double line) const {
    // First try exact match
    auto exact = get(date, player, stat, line);
    if (exact) return exact;

    std::string gkey = make_group_key(date, player, stat);
    auto it = player_lines_.find(gkey);
    if (it == player_lines_.end() || it->second.size() < 2) return std::nullopt;

    const auto& lines = it->second;

    // Find bracketing lines using binary search
    auto upper = std::lower_bound(lines.begin(), lines.end(),
        std::make_pair(line, 0.0),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    if (upper == lines.begin() || upper == lines.end()) {
        // line is outside the range of available lines — no interpolation
        return std::nullopt;
    }

    auto lower = std::prev(upper);
    double lo_line = lower->first;
    double hi_line = upper->first;
    double lo_price = lower->second;
    double hi_price = upper->second;

    if (std::abs(hi_line - lo_line) < 0.01) return lo_price;

    // Linear interpolation
    double t = (line - lo_line) / (hi_line - lo_line);
    return lo_price + t * (hi_price - lo_price);
}

// ---------- Odds utilities ----------

namespace odds {

double american_to_decimal(double ml) {
    if (ml >= 100.0) {
        return 1.0 + ml / 100.0;
    } else if (ml <= -100.0) {
        return 1.0 + 100.0 / std::abs(ml);
    }
    // Invalid ML (between -100 and +100)
    return 1.0;
}

double kalshi_to_decimal(double yes_price, const std::string& side) {
    if (side == "OVER" || side == "over") {
        if (yes_price < 0.01) return 100.0;  // cap at 100x
        return 1.0 / yes_price;
    } else {
        // UNDER: prob = 1 - yes_price
        double no_price = 1.0 - yes_price;
        if (no_price < 0.01) return 100.0;
        return 1.0 / no_price;
    }
}

ResolvedOdds resolve(const KalshiCache& cache,
                     const std::string& date, const std::string& player,
                     const std::string& stat, double line,
                     const std::string& side, double dk_ml) {
    ResolvedOdds result;

    // Try Kalshi exact
    auto exact = cache.get(date, player, stat, line);
    if (exact) {
        result.decimal = kalshi_to_decimal(*exact, side);
        result.source = "kalshi";
        return result;
    }

    // Try Kalshi interpolated
    auto interp = cache.interpolate(date, player, stat, line);
    if (interp) {
        result.decimal = kalshi_to_decimal(*interp, side);
        result.source = "kalshi_interp";
        return result;
    }

    // Fall back to DraftKings
    result.decimal = american_to_decimal(dk_ml);
    result.source = "dk";
    return result;
}

} // namespace odds
} // namespace nba
