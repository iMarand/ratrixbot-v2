# DerivBot Advanced Self-Learning Trading System

Transform derivbot from a 2-strategy backtester into an advanced self-optimizing system with multiple indicators, reinforcement learning, and automated strategy discovery.

## User Review Required

> [!IMPORTANT]
> **Scope is very large.** This plan adds ~2500 lines of C++ across multiple new files. The build command stays the same single `g++` invocation (no cmake/make needed). Everything stays header-only except `derivbot.cpp`.

> [!WARNING]
> **RL (Q-learning) on synthetic indices:** Deriv's synthetic indices are RNG-generated. RL *will* find patterns in historical data (that's what it does), but those patterns may not generalize to future data. The system will report in-sample vs out-of-sample metrics so you can judge for yourself. This is an honest tool, not a money printer.

> [!IMPORTANT]
> **Forex/Gold/Stocks real candle data:** Deriv's `ticks_history` API works for forex and commodities only when markets are open. The system will handle "Failed to fetch" gracefully by skipping that symbol and moving on. For stocks, Deriv doesn't offer stock tick history through the same API — only their synthetic/forex/commodity instruments are available.

## Open Questions

> [!IMPORTANT]
> 1. **Training duration**: You mentioned "continuous learning for hours." Should `--train` run indefinitely until Ctrl+C, or should there be a `--train-hours 4` flag to auto-stop?
> 2. **Q-learning state space**: I plan to use discretized recent-performance features (recent win rate, volatility regime, trend direction) as RL state. Should the Q-table be saved to disk between runs so it accumulates knowledge across sessions?
> 3. **Real money integration**: You mentioned wanting the best strategies to actually trade. Should I add a `--mode live` that uses the best discovered strategy, or keep everything paper-only for now?

---

## Architecture Overview

```
derivbot/
├── derivbot.cpp              [MODIFY] - Add --train, --autoadjust, --all-symbols CLI + train orchestrator
├── signal_types.hpp           [KEEP]   - No changes
├── rsi.hpp                    [KEEP]   - No changes
├── confluence_strategy.hpp    [KEEP]   - No changes
├── indicators/
│   ├── ema.hpp                [NEW] - Exponential Moving Average
│   ├── macd.hpp               [NEW] - MACD (EMA12/EMA26 + signal line)
│   ├── bollinger.hpp          [NEW] - Bollinger Bands (mean ± k*σ)
│   ├── stochastic.hpp         [NEW] - Stochastic Oscillator (%K/%D)
│   ├── atr.hpp                [NEW] - Average True Range (volatility)
│   └── adx.hpp                [NEW] - Average Directional Index (trend strength)
├── strategies/
│   ├── strategy_base.hpp      [NEW] - Common interface for all strategies
│   ├── ema_crossover.hpp      [NEW] - EMA fast/slow crossover
│   ├── macd_strategy.hpp      [NEW] - MACD histogram reversal
│   ├── bollinger_strategy.hpp [NEW] - Bollinger band bounce/breakout
│   ├── stochastic_strategy.hpp[NEW] - Stochastic overbought/oversold
│   ├── multi_confluence.hpp   [NEW] - Configurable multi-indicator confluence
│   └── strategy_registry.hpp  [NEW] - Registry of all strategies + param ranges for grid search
├── rl/
│   ├── q_learning.hpp         [NEW] - Tabular Q-learning agent
│   ├── state_features.hpp     [NEW] - Market regime feature extraction
│   └── rl_trainer.hpp         [NEW] - RL training loop wrapper
└── train/
    ├── grid_search.hpp        [NEW] - Parameter grid search engine
    ├── train_engine.hpp       [NEW] - Full training orchestrator
    └── report.hpp             [NEW] - Report generator (console + CSV + summary files)
```

---

## Proposed Changes

### 1. New Indicators (`indicators/`)

#### [NEW] [ema.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/ema.hpp)
Exponential Moving Average with configurable period. Supports multiple instances for fast/slow crossover detection.
- `EMA(int period)` — incremental update, returns smoothed value once seeded
- Used by: EMA crossover strategy, MACD

#### [NEW] [macd.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/macd.hpp)
MACD = EMA(12) − EMA(26), signal line = EMA(9) of MACD, histogram = MACD − signal.
- Returns `{macd, signal, histogram}` struct
- Used by: MACD strategy, multi-confluence

#### [NEW] [bollinger.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/bollinger.hpp)
Bollinger Bands = SMA(20) ± 2×σ. Tracks upper/lower/middle bands.
- Returns `{upper, middle, lower, bandwidth}` struct
- Used by: Bollinger strategy, multi-confluence

#### [NEW] [stochastic.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/stochastic.hpp)
Stochastic Oscillator: %K = (close − lowest) / (highest − lowest) × 100, %D = SMA(%K, 3).
- Configurable lookback period (default 14)
- Used by: Stochastic strategy, multi-confluence

#### [NEW] [atr.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/atr.hpp)
Average True Range — measures volatility. Since we only have tick data (not OHLC), we'll approximate using rolling price range over N ticks.
- Used by: RL state features (volatility regime), position sizing

#### [NEW] [adx.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/indicators/adx.hpp)
Average Directional Index — measures trend strength (0-100). Adapted for tick data using directional movement of consecutive prices.
- ADX > 25 = trending, ADX < 20 = ranging
- Used by: RL state features, multi-confluence (only trade mean-reversion when ADX is low)

---

### 2. New Strategies (`strategies/`)

#### [NEW] [strategy_base.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/strategy_base.hpp)
Abstract base class so all strategies share a uniform interface:
```cpp
class StrategyBase {
public:
    virtual ~StrategyBase() = default;
    virtual Signal onPrice(int64_t time, double price) = 0;
    virtual std::string name() const = 0;
    virtual std::string describeParams() const = 0;
};
```

#### [NEW] [ema_crossover.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/ema_crossover.hpp)
Fires Rise when fast EMA crosses above slow EMA, Fall when it crosses below.
- Parameters: `fastPeriod` (5-20), `slowPeriod` (20-100)
- Grid search ranges: fast ∈ {5,8,10,13}, slow ∈ {20,30,50}

#### [NEW] [macd_strategy.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/macd_strategy.hpp)
Fires on MACD histogram zero-crossing (momentum shift).
- Parameters: `fastEma` (8-15), `slowEma` (20-30), `signalPeriod` (5-12)
- Grid search ranges: fast ∈ {8,12}, slow ∈ {21,26}, signal ∈ {5,9}

#### [NEW] [bollinger_strategy.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/bollinger_strategy.hpp)
Mean-reversion: Rise when price touches lower band + RSI oversold, Fall when price touches upper band + RSI overbought.
- Parameters: `bbPeriod` (15-25), `bbStdDev` (1.5-2.5), RSI thresholds
- Grid search: period ∈ {15,20,25}, stddev ∈ {1.5,2.0,2.5}

#### [NEW] [stochastic_strategy.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/stochastic_strategy.hpp)
Fires when %K crosses %D in overbought (>80) or oversold (<20) territory.
- Parameters: `kPeriod` (5-21), `dPeriod` (3-5), thresholds
- Grid search: kPeriod ∈ {5,9,14,21}, dPeriod ∈ {3,5}

#### [NEW] [multi_confluence.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/multi_confluence.hpp)
The powerhouse strategy — requires N-of-M indicators to agree before firing:
- Configurable: which indicators to include, minimum agreement count
- Example: "Fire Rise only if RSI oversold AND Stochastic oversold AND price at Bollinger lower band AND ADX < 25 (ranging market = mean reversion works)"
- Grid search iterates over different indicator combinations and agreement thresholds

#### [NEW] [strategy_registry.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/strategies/strategy_registry.hpp)
Central registry that:
1. Knows every strategy class and its parameter grid
2. Can instantiate any strategy by name + param set
3. Generates all parameter combinations for grid search
4. Estimated total combinations: ~200-500 per symbol (keeps training tractable)

---

### 3. Reinforcement Learning (`rl/`)

#### [NEW] [state_features.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/rl/state_features.hpp)
Extracts discretized market regime features from recent price history:
- **Trend**: EMA(10) vs EMA(50) → {uptrend, downtrend, flat}
- **Volatility**: ATR percentile → {low, medium, high}
- **Momentum**: RSI zone → {oversold, neutral, overbought}
- **Regime**: ADX level → {trending, ranging}
- Total state space: 3 × 3 × 3 × 2 = 54 states (small enough for tabular Q-learning)

#### [NEW] [q_learning.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/rl/q_learning.hpp)
Tabular Q-learning agent:
- **State**: discretized market regime (54 states)
- **Actions**: which strategy to deploy (one per registered strategy config)
- **Reward**: P&L of the trade taken by the selected strategy
- **Learning**: Q(s,a) ← Q(s,a) + α[r + γ·max_a' Q(s',a') − Q(s,a)]
- Hyperparameters: α=0.1, γ=0.95, ε-greedy exploration (ε decays over episodes)
- Q-table saved/loaded from `qtable.json` for persistence across sessions

#### [NEW] [rl_trainer.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/rl/rl_trainer.hpp)
Wraps the Q-learning loop:
1. Split tick data into episodes (rolling windows)
2. For each tick: extract state → pick action (strategy) → simulate trade → get reward → update Q
3. After N episodes, report which strategies the agent learned to prefer in each regime
4. Supports train/test split for out-of-sample validation

---

### 4. Training Engine (`train/`)

#### [NEW] [grid_search.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/train/grid_search.hpp)
Parameter optimization engine:
1. Receives tick data + list of strategy configurations
2. For each config, runs the existing `runBacktest()` function
3. Collects results: win rate, net P&L, max drawdown, Sharpe-like ratio, trade count
4. Ranks by composite score: `score = netPnl * min(1, trades/20) * (1 - maxDD/netPnl_abs)`
   - Penalizes too-few-trades (overfitting on 3 lucky trades)
   - Penalizes huge drawdowns relative to gains
5. Returns top-K strategies

#### [NEW] [train_engine.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/train/train_engine.hpp)
Full training orchestrator — this is what `--train --autoadjust` calls:

**Phase 1 — Data Collection:**
- Connect to Deriv, resolve symbols (`--all-symbols` or `--symbols jump10,v75,gold`)
- Fetch max available tick history for each symbol
- Split 80/20 into train/test sets

**Phase 2 — Grid Search (per symbol):**
- Generate all strategy × parameter combinations from the registry
- Run backtest on train set for each combo
- Rank and keep top-20 per symbol

**Phase 3 — RL Meta-Learner:**
- Train Q-learning agent on the train set using top strategies as actions
- Agent learns which strategy works best in which market regime
- Validate on test set

**Phase 4 — Continuous Learning Loop (if --autoadjust):**
- Every N minutes, re-fetch latest ticks
- Re-run grid search on expanded dataset
- Update Q-table
- Print progress report
- Loop until Ctrl+C or `--train-hours` limit

**Phase 5 — Report Generation:**
- Create `results/` folder with timestamp
- Per-symbol subfolder with:
  - `summary.txt` — ranked strategies with metrics
  - `best_strategy_1.csv`, `best_strategy_2.csv`, ... — trade ledgers
  - `qtable.json` — learned Q-table
- Console summary of overall best strategies across all symbols

#### [NEW] [report.hpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/train/report.hpp)
Report generation utilities:
- Pretty-print ranked strategy tables to console
- Write CSV trade ledgers for each top strategy
- Write JSON summary with all metrics
- Generate `results/<timestamp>/` folder structure

---

### 5. Main File Changes

#### [MODIFY] [derivbot.cpp](file:///d:/More_Projects/Projects/Demos/CPP/derivbot/derivbot.cpp)

**New CLI flags:**
```
--train                    Enter training/optimization mode
--autoadjust               Enable continuous learning loop (re-fetches, re-trains)
--all-symbols              Train across all available Deriv symbols
--symbols <list>           Comma-separated symbol list (e.g. jump10,v75,gold,eurusd)
--train-hours <n>          Auto-stop training after N hours (default: unlimited)
--top-k <n>                Keep top N strategies per symbol (default: 10)
--train-test-split <pct>   Train/test split ratio (default: 0.8)
--results-dir <path>       Output directory for results (default: results/)
```

**Changes:**
- Add `#include` for all new headers
- Parse new CLI flags in `main()`
- Add `mode == "train"` branch that instantiates `TrainEngine` and runs it
- Existing `backtest` and `paper` modes remain unchanged — this is purely additive
- Add new strategy names to `--strategy` validation (ema_cross, macd, bollinger, stochastic, multi_confluence)
- Wire new strategies into existing backtest/paper paths so they work individually too

---

## Verification Plan

### Automated Tests
```bash
# Build
g++ derivbot.cpp -o derivbot.exe -std=c++17 -lssl -lcrypto -lws2_32 -lwsock32

# Test individual new strategies work in backtest mode
./derivbot.exe --mode backtest --symbol jump10 --duration 15 --count 50000 --strategy ema_cross
./derivbot.exe --mode backtest --symbol jump10 --duration 15 --count 50000 --strategy macd
./derivbot.exe --mode backtest --symbol jump10 --duration 15 --count 50000 --strategy bollinger
./derivbot.exe --mode backtest --symbol jump10 --duration 15 --count 50000 --strategy stochastic
./derivbot.exe --mode backtest --symbol jump10 --duration 15 --count 50000 --strategy multi_confluence

# Test training mode on single symbol
./derivbot.exe --train --symbols jump10 --count 50000 --top-k 5

# Test training with autoadjust (short run)
./derivbot.exe --train --autoadjust --symbols jump10 --count 50000 --train-hours 0.1

# Test multi-symbol training
./derivbot.exe --train --symbols jump10,v75,jump25 --count 50000
```

### Manual Verification
- Inspect `results/` folder for correct structure and content
- Verify CSV trade ledgers contain valid data
- Check that RL Q-table file is written and loadable
- Confirm existing `--mode backtest` and `--mode paper` still work identically
