#include "z_score.h"
#include <cmath>
#include <algorithm>

namespace nba {
namespace features {

double rolling_avg(const std::vector<double>& vals, int end_idx, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = static_cast<int>(vals.size());
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count <= 0) return 0.0;

    double sum = 0.0;
    for (int i = start; i < end_idx; ++i)
        sum += vals[i];
    return sum / count;
}

double rolling_std(const std::vector<double>& vals, int end_idx, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = static_cast<int>(vals.size());
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count < 2) return 0.0;

    double sum = 0.0;
    for (int i = start; i < end_idx; ++i)
        sum += vals[i];
    double mean = sum / count;

    double var_sum = 0.0;
    for (int i = start; i < end_idx; ++i) {
        double diff = vals[i] - mean;
        var_sum += diff * diff;
    }
    return std::sqrt(var_sum / (count - 1));  // sample std dev
}

double z_score(const std::vector<double>& vals, int end_idx,
               int lookback_recent, int lookback_season) {
    if (end_idx <= 0 || vals.empty()) return 0.0;

    double season_avg = rolling_avg(vals, end_idx, lookback_season);
    double season_std = rolling_std(vals, end_idx, lookback_season);
    double recent_avg = rolling_avg(vals, end_idx, lookback_recent);

    // Guard against zero or near-zero std dev
    if (season_std < 1e-9) return 0.0;

    return (recent_avg - season_avg) / season_std;
}

double hit_rate_over(const std::vector<double>& vals, int end_idx,
                     double line, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = static_cast<int>(vals.size());
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count <= 0) return 0.0;

    int hits = 0;
    for (int i = start; i < end_idx; ++i) {
        if (vals[i] > line) ++hits;
    }
    return static_cast<double>(hits) / count;
}

double hit_rate_under(const std::vector<double>& vals, int end_idx,
                      double line, int window) {
    if (end_idx <= 0 || vals.empty()) return 0.0;
    int n = static_cast<int>(vals.size());
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count <= 0) return 0.0;

    int hits = 0;
    for (int i = start; i < end_idx; ++i) {
        if (vals[i] < line) ++hits;
    }
    return static_cast<double>(hits) / count;
}

double per_minute_rate(const std::vector<double>& stat_vals,
                       const std::vector<double>& minutes,
                       int end_idx, int window) {
    if (end_idx <= 0 || stat_vals.empty() || minutes.empty()) return 0.0;
    int n = std::min(static_cast<int>(stat_vals.size()),
                     static_cast<int>(minutes.size()));
    end_idx = std::min(end_idx, n);
    int start = std::max(0, end_idx - window);
    int count = end_idx - start;
    if (count <= 0) return 0.0;

    double stat_sum = 0.0;
    double min_sum = 0.0;
    for (int i = start; i < end_idx; ++i) {
        stat_sum += stat_vals[i];
        min_sum += minutes[i];
    }
    if (min_sum < 1e-9) return 0.0;
    return stat_sum / min_sum;
}

} // namespace features
} // namespace nba
