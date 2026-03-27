#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <utility>

namespace nba {

class KalshiCache {
public:
    // Load all kalshi_*_settled.csv files from kalshi_dir
    void load(const std::string& kalshi_dir);

    // Exact match: (date, player, stat, line) → yes_price
    std::optional<double> get(const std::string& date, const std::string& player,
                              const std::string& stat, double line) const;

    // Interpolated: find nearest lines for (date, player, stat), interpolate yes_price
    std::optional<double> interpolate(const std::string& date, const std::string& player,
                                       const std::string& stat, double line) const;

    size_t size() const { return cache_.size(); }

private:
    // key = "date|player|stat|line" → yes_price
    std::unordered_map<std::string, double> cache_;
    // key = "date|player|stat" → sorted [(line, yes_price), ...]
    std::unordered_map<std::string, std::vector<std::pair<double, double>>> player_lines_;

    static std::string make_key(const std::string& date, const std::string& player,
                                const std::string& stat, double line);
    static std::string make_group_key(const std::string& date, const std::string& player,
                                      const std::string& stat);
};

namespace odds {

// Convert American odds to decimal odds
// +150 → 2.50,  -150 → 1.667
double american_to_decimal(double ml);

// Convert Kalshi yes_price (0-1 probability) to decimal odds for a given side
// side = "OVER" → decimal = 1.0 / yes_price
// side = "UNDER" → decimal = 1.0 / (1.0 - yes_price)
double kalshi_to_decimal(double yes_price, const std::string& side);

struct ResolvedOdds {
    double decimal = 0.0;
    std::string source;  // "kalshi", "kalshi_interp", or "dk"
};

// Resolve odds: try Kalshi exact, then interpolated, then fall back to DK
ResolvedOdds resolve(const KalshiCache& cache,
                     const std::string& date, const std::string& player,
                     const std::string& stat, double line,
                     const std::string& side, double dk_ml);

} // namespace odds
} // namespace nba
