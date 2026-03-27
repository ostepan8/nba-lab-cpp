# NBA Model Lab (C++)

High-performance NBA player props model lab. C++ rewrite of the Python model_lab.py for 80x throughput.

## Architecture

```
src/
├── main.cpp              # CLI entry point
├── data/
│   ├── csv_parser.h/cpp  # Fast CSV loading
│   ├── store.h/cpp       # In-memory data store (gamelogs, props, odds)
│   └── types.h           # Core data types (Player, Game, Prop, Bet)
├── features/
│   ├── player_index.h/cpp  # Pre-computed per-player stats
│   ├── z_score.h/cpp       # Rolling z-score computation
│   └── odds.h/cpp          # Kalshi/DK odds resolution
├── strategies/
│   ├── strategy.h          # Base strategy interface
│   ├── meanrev.h/cpp       # Mean reversion
│   ├── situational.h/cpp   # Multi-factor situational
│   └── twostage.h/cpp      # Minutes-conditioned
├── engine/
│   ├── walkforward.h/cpp   # Walk-forward backtester
│   ├── experiment.h/cpp    # Single experiment runner
│   └── lab.h/cpp           # Parallel experiment orchestrator
├── io/
│   ├── knowledge.h/cpp     # Knowledge base (JSON read/write)
│   ├── bet_history.h/cpp   # JSONL bet export
│   └── notify.h/cpp        # Telegram notifications
└── config/
    └── config.h/cpp        # Runtime configuration
```

## Build
```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

## Target: 20,000+ experiments/hour on 24-core machine
