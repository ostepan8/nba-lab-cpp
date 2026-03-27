# NBA Model Lab (C++)

High-performance NBA player props backtesting lab. Generates random strategy hypotheses, runs walk-forward backtests across 6 prop markets, and maintains a persistent knowledge base of best-performing configurations. C++ rewrite of Python `model_lab.py` for ~100x throughput.

## Performance

- **191 experiments/sec** on 24-core Ryzen (8 worker threads)
- **3.4s** full data load (23K gamelogs, 1.5M props, 100K Kalshi entries)
- **~690K experiments/hour** sustained

## Build

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

Requires: C++20 compiler, CMake 3.16+, pthreads. No external dependencies (nlohmann/json is vendored as header-only in `include/`).

## Usage

```bash
# Print data stats and config
./nba_lab info

# Run 100 experiments and report throughput
./nba_lab bench --count 100

# Run the lab forever (Ctrl+C for graceful shutdown)
./nba_lab run

# Run a single experiment from JSON config
./nba_lab single --config '{"type":"meanrev","target_stat":"PTS","target_market":"player_points","sides":["OVER","UNDER"],"min_dev":0.8,"lookback_recent":5,"lookback_season":40}'

# Print the current leaderboard
./nba_lab leaderboard

# Use a custom config file
./nba_lab run --config /path/to/config.json
```

## Config File

Default: `config.json` in the working directory.

```json
{
    "data_dir": "~/Desktop/nba-modeling/data/raw",
    "kalshi_dir": "~/Desktop/nba-modeling/data/raw/kalshi",
    "output_dir": "~/Desktop/nba-modeling/results",
    "knowledge_path": "~/Desktop/nba-modeling/results/lab/knowledge_cpp.json",
    "notify_script": "~/claude-code-linux-harness/notify.sh",
    "fast_workers": 6,
    "slow_workers": 2,
    "meanrev_weight": 0.40,
    "situational_weight": 0.30,
    "twostage_weight": 0.30,
    "notify_enabled": true,
    "notify_min_roi": 0.0,
    "kalshi_fee_rate": 0.038
}
```

All `~` paths are expanded to `$HOME` at load time. Missing fields fall back to defaults.

| Field | Description |
|-------|-------------|
| `data_dir` | Directory containing `player_gamelog_*.csv`, `player_props/`, `odds/` |
| `kalshi_dir` | Directory containing `kalshi_*_settled.csv` files |
| `output_dir` | Where to write `lab/experiments.jsonl` and `bet_history/` |
| `knowledge_path` | Persistent JSON leaderboard (survives restarts) |
| `notify_script` | Script called with breakthrough notifications |
| `fast_workers` / `slow_workers` | Thread pool sizes |
| `*_weight` | Probability of generating each strategy type (must sum to 1.0) |
| `notify_enabled` | Toggle Telegram notifications |
| `notify_min_roi` | Only notify if ROI exceeds this threshold |
| `kalshi_fee_rate` | Kalshi fee rate for net ROI computation |

## Architecture

```
                ┌─────────────────┐
                │    main.cpp     │  CLI parsing, signal handling, data loading
                │   (273 lines)   │
                └────────┬────────┘
                         │
              ┌──────────┴──────────┐
              │                     │
   ┌──────────┴──────────┐  ┌──────┴──────┐
   │   config/config     │  │  engine/lab  │  Parallel orchestrator
   │    (171 lines)      │  │ (331 lines)  │  Producer-consumer work queues
   └─────────────────────┘  └──────┬───────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
            ┌───────┴───────┐ ┌───┴────┐ ┌───────┴────────┐
            │engine/hypothe-│ │engine/ │ │  io/knowledge   │
            │sis (183 lines)│ │walkfwd │ │  (193 lines)    │
            └───────────────┘ │(180 ln)│ └─────────────────┘
                              └───┬────┘
                                  │
                    ┌─────────────┼─────────────┐
                    │             │             │
           ┌────────┴───┐ ┌──────┴────┐ ┌──────┴──────┐
           │  meanrev    │ │situational│ │  twostage   │
           │ (147 lines) │ │(258 lines)│ │ (201 lines) │
           └─────────────┘ └───────────┘ └─────────────┘
                    │             │             │
           ┌────────┴─────────────┴─────────────┘
           │
   ┌───────┴────────┐  ┌──────────────┐  ┌─────────────┐
   │ features/z_score│  │features/odds │  │features/     │
   │   (150 lines)   │  │ (251 lines)  │  │player_index  │
   └─────────────────┘  └──────────────┘  │ (212 lines)  │
                                          └──────────────┘
```

## File Listing

```
src/
├── main.cpp                     273 lines   CLI + signal handling
├── config/
│   ├── config.h                  44 lines   LabConfig struct
│   └── config.cpp               127 lines   JSON load/save, path expansion
├── data/
│   ├── types.h                   67 lines   PlayerGame, PropLine, OddsLine, Bet
│   ├── csv_parser.h              28 lines   CSV parsing interface
│   ├── csv_parser.cpp           185 lines   Fast CSV loader (gamelogs, props, odds)
│   ├── store.h                   47 lines   DataStore interface
│   └── store.cpp                104 lines   In-memory data store
├── engine/
│   ├── hypothesis.h              33 lines   HypothesisGenerator interface
│   ├── hypothesis.cpp           150 lines   Random strategy config generation
│   ├── lab.h                     60 lines   Lab orchestrator interface
│   ├── lab.cpp                  271 lines   Parallel runner, bench, leaderboard
│   ├── walkforward.h             35 lines   Walk-forward backtester interface
│   └── walkforward.cpp          145 lines   Walk-forward engine
├── features/
│   ├── z_score.h                 39 lines   Rolling stats interface
│   ├── z_score.cpp              111 lines   Z-score, rolling avg/std, hit rates
│   ├── odds.h                    61 lines   KalshiCache + odds utilities
│   ├── odds.cpp                 190 lines   Kalshi load, interpolation, DK fallback
│   ├── player_index.h            51 lines   PlayerStats + PlayerIndex interface
│   └── player_index.cpp         161 lines   Per-player stat arrays with binary search
├── strategies/
│   ├── strategy.h               139 lines   StrategyConfig, ExperimentResult, base class
│   ├── meanrev.h                 15 lines   MeanRevStrategy interface
│   ├── meanrev.cpp              132 lines   Mean reversion (z-score deviation)
│   ├── situational.h             15 lines   SituationalStrategy interface
│   ├── situational.cpp          243 lines   Multi-factor situational analysis
│   ├── twostage.h                15 lines   TwostageStrategy interface
│   └── twostage.cpp             186 lines   Minutes-conditioned rate prediction
└── io/
    ├── knowledge.h               49 lines   KnowledgeBase interface
    ├── knowledge.cpp            144 lines   JSON leaderboard persistence
    ├── bet_history.h             16 lines   Bet history export interface
    ├── bet_history.cpp           44 lines   JSONL bet export
    ├── notify.h                  13 lines   Notification interface
    └── notify.cpp                33 lines   Shell notification via notify.sh

Total: 3,226 lines across 34 files
```

## Strategies

### Mean Reversion (`meanrev`)
Detects when a player's recent performance deviates from their season average (via z-score). Bets on reversion to the mean: hot streaks trigger UNDER bets, cold streaks trigger OVER bets. Filtered by hit rate and sized via Kelly criterion.

### Situational (`situational`)
Multi-factor model evaluating: z-score deviation, back-to-back games, fatigue (high minutes), line vs. season average, trend mode, and consistency filters. Requires a minimum number of agreeing factors before placing a bet. Factor count scales the Kelly fraction.

### Two-Stage (`twostage`)
Predicts minutes played (stage 1) and per-minute production rate (stage 2) separately, then combines them for a stat prediction. Compares prediction to the line to find edges. Handles B2B minutes adjustments.

## Odds Resolution

For each bet, odds are resolved in priority order:
1. **Kalshi exact** -- exact match on (date, player, stat, line)
2. **Kalshi interpolated** -- linear interpolation between bracketing lines
3. **DraftKings fallback** -- American odds converted to decimal

## Graceful Shutdown

`Ctrl+C` (SIGINT) or `SIGTERM` triggers graceful shutdown:
- Workers finish their current experiment
- Knowledge base is saved to disk
- Final leaderboard is printed
- No data is lost
