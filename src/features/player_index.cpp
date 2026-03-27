#include "player_index.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nba {

// ---------- PlayerStats ----------

const std::vector<double>& PlayerStats::get_stat(const std::string& stat) const {
    if (stat == "pts" || stat == "PTS")         return pts;
    if (stat == "reb" || stat == "REB")         return reb;
    if (stat == "ast" || stat == "AST")         return ast;
    if (stat == "fg3m" || stat == "FG3M" ||
        stat == "3pm" || stat == "threes")      return fg3m;
    if (stat == "stl" || stat == "STL")         return stl;
    if (stat == "blk" || stat == "BLK")         return blk;
    if (stat == "minutes" || stat == "MIN")     return minutes;
    throw std::runtime_error("PlayerStats::get_stat: unknown stat '" + stat + "'");
}

int PlayerStats::find_date_index(const std::string& date) const {
    if (dates.empty()) return -1;

    // upper_bound finds the first date strictly AFTER `date`.
    // Subtract 1 to get last date <= `date`.
    auto it = std::upper_bound(dates.begin(), dates.end(), date);
    if (it == dates.begin()) return -1;
    return static_cast<int>(std::distance(dates.begin(), it)) - 1;
}

// ---------- Extract opponent from matchup string ----------
// "DEN vs. NYK" → "NYK",  "DEN @ PHX" → "PHX"
static std::string extract_opponent(const std::string& matchup,
                                     const std::string& /*team*/) {
    // Try "vs."
    auto vs_pos = matchup.find("vs.");
    if (vs_pos != std::string::npos) {
        std::string rhs = matchup.substr(vs_pos + 4);
        // Trim leading space
        while (!rhs.empty() && rhs[0] == ' ') rhs.erase(0, 1);
        return rhs;
    }
    // Try "@"
    auto at_pos = matchup.find("@");
    if (at_pos != std::string::npos) {
        std::string rhs = matchup.substr(at_pos + 1);
        while (!rhs.empty() && rhs[0] == ' ') rhs.erase(0, 1);
        return rhs;
    }
    return "";
}

// ---------- PlayerIndex ----------

void PlayerIndex::build(const DataStore& store) {
    by_id_.clear();
    name_to_id_.clear();

    // We need to iterate all players. DataStore stores games_by_pid_ and
    // games_by_name_, but only exposes lookup by specific id/name.
    // Instead, we'll iterate by name using the names we can discover
    // from prop dates (which cover active players), BUT that would miss
    // players without props.
    //
    // Better approach: DataStore has get_player_games(pid) and
    // get_player_games_by_name(name). We need the internal maps.
    // Since DataStore doesn't expose an iterator, we'll use the
    // games_by_name approach — we need to get all player names.
    //
    // For now, we'll build from prop dates to discover player IDs,
    // then look up their games. We also need players who appear in
    // gamelogs but may not have props.
    //
    // Simplest: DataStore stores games internally. We'll use a
    // different strategy — rebuild from the raw gamelog data that
    // DataStore already parsed. We access via get_player_games_by_name.
    //
    // Actually, let's just gather unique player names from props and
    // from a full scan. The store already groups by pid — we just
    // can't iterate it. Let's add a helper that gathers unique IDs.
    //
    // WORKAROUND: Build by iterating prop dates to find (name, pid)
    // pairs, then supplement from games.

    // Collect all (name, pid) from props
    std::unordered_map<std::string, int> discovered;
    for (const auto& date : store.get_prop_dates()) {
        for (const auto& p : store.get_props(date)) {
            if (p.player_id != 0 && discovered.find(p.player_name) == discovered.end()) {
                discovered[p.player_name] = p.player_id;
            }
        }
    }

    // Also try to find players from gamelogs who don't appear in props.
    // We'll do this by checking games_by_name for each discovered name.
    // But we also want players WITHOUT props. The only way is if
    // DataStore exposes its full player list.
    // For now, props cover all active players we care about for betting.

    for (const auto& [name, pid] : discovered) {
        // Always register the name → pid mapping (handles aliases)
        name_to_id_[name] = pid;
        normalized_to_id_[normalize_name(name)] = pid;

        // Only build PlayerStats once per pid
        if (by_id_.count(pid)) continue;

        const auto& games = store.get_player_games(pid);
        if (games.empty()) continue;

        PlayerStats ps;
        ps.name = name;
        ps.player_id = pid;

        // Games are already sorted by date in DataStore
        ps.dates.reserve(games.size());
        ps.pts.reserve(games.size());
        ps.reb.reserve(games.size());
        ps.ast.reserve(games.size());
        ps.fg3m.reserve(games.size());
        ps.stl.reserve(games.size());
        ps.blk.reserve(games.size());
        ps.minutes.reserve(games.size());
        ps.teams.reserve(games.size());
        ps.opponents.reserve(games.size());
        ps.is_home.reserve(games.size());

        for (const auto& g : games) {
            ps.dates.push_back(g.game_date);
            ps.pts.push_back(g.pts);
            ps.reb.push_back(g.reb);
            ps.ast.push_back(g.ast);
            ps.fg3m.push_back(g.fg3m);
            ps.stl.push_back(g.stl);
            ps.blk.push_back(g.blk);
            ps.minutes.push_back(g.minutes);
            ps.teams.push_back(g.team);
            ps.opponents.push_back(extract_opponent(g.matchup, g.team));
            ps.is_home.push_back(g.is_home);
        }

        by_id_[pid] = std::move(ps);
    }

    printf("  PlayerIndex: %zu players, %zu name aliases\n",
           by_id_.size(), name_to_id_.size());
}

const PlayerStats* PlayerIndex::get_by_id(int pid) const {
    auto it = by_id_.find(pid);
    if (it != by_id_.end()) return &it->second;
    return nullptr;
}

const PlayerStats* PlayerIndex::get_by_name(const std::string& name) const {
    // Try exact match first
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) return get_by_id(it->second);
    // Try normalized name
    auto it2 = normalized_to_id_.find(normalize_name(name));
    if (it2 != normalized_to_id_.end()) return get_by_id(it2->second);
    return nullptr;
}

std::string PlayerIndex::normalize_name(const std::string& name) {
    std::string n = name;
    // Remove dots
    n.erase(std::remove(n.begin(), n.end(), '.'), n.end());
    // Remove common suffixes
    for (const char* suffix : {" Jr", " Jr.", " Sr", " II", " III", " IV", " V"}) {
        size_t pos = n.rfind(suffix);
        if (pos != std::string::npos && pos + strlen(suffix) == n.size()) {
            n = n.substr(0, pos);
        }
    }
    // Lowercase
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    // Trim whitespace
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(n.begin());
    return n;
}

} // namespace nba
