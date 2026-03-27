#pragma once

#include <vector>

namespace nba {
namespace features {

// Compute z-score: (recent_avg - season_avg) / season_std
// Uses values [0..end_idx) — end_idx is EXCLUSIVE (games before that index).
// Returns 0.0 if not enough data or zero std dev.
double z_score(const std::vector<double>& vals, int end_idx,
               int lookback_recent = 5, int lookback_season = 40);

// Rolling average of last `window` values before end_idx.
// Uses vals[max(0, end_idx-window) .. end_idx).
// Returns 0.0 if end_idx <= 0.
double rolling_avg(const std::vector<double>& vals, int end_idx, int window);

// Rolling standard deviation of last `window` values before end_idx.
// Returns 0.0 if fewer than 2 values.
double rolling_std(const std::vector<double>& vals, int end_idx, int window);

// Hit rate: fraction of values above line in last `window` games before end_idx.
double hit_rate_over(const std::vector<double>& vals, int end_idx,
                     double line, int window = 20);

// Hit rate: fraction of values below line.
double hit_rate_under(const std::vector<double>& vals, int end_idx,
                      double line, int window = 20);

// Per-minute production rate over last `window` games.
// sum(stat) / sum(minutes) for the window.
// Returns 0.0 if total minutes is 0.
double per_minute_rate(const std::vector<double>& stat_vals,
                       const std::vector<double>& minutes,
                       int end_idx, int window = 10);

} // namespace features
} // namespace nba
