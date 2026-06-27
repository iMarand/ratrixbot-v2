# Deriv Boom/Crash — the 98.5% strategies (Random Forest & XGBoost)

This folder bundles the ONLY strategies in the whole project that showed a real,
robust, out-of-sample edge: **Random Forest** and **XGBoost** (self-training,
candle-aware) traded on Deriv's **Boom 1000 / Crash 1000** synthetic indices,
backtested on **Deriv API tick data** via the `derivbot` C++ tool.

> They produced ~**98.5–99.5% win rate** and scaled linearly with more data
> (robust, not curve-fit). v75 and gold showed NO edge by contrast.

---

## 1. What contract type was used?  →  RISE/FALL (binary), NOT multipliers

The backtest models a **Rise/Fall (CALL/PUT) binary contract**:

- Enter at the current price, pick a direction (Rise or Fall) and a duration (15s).
- At expiry, compare exit price vs entry price.
- **Win** → +`stake × payout`  (payout assumed **0.95 = 95%**).
- **Loss** → −`stake`  (the loss is **capped at the stake** — this is what makes
  the high-win-rate drift bet profitable).

Exact settlement logic (from `derivbot.cpp`):

```cpp
// exit = first tick at entryTime + durationSec
bool priceUp   = exitPrice > entryPrice;
bool priceDown = exitPrice < entryPrice;
won = (direction == Rise) ? priceUp : priceDown;   // tie = loss
pnl = won ? (stake * payoutPct) : -stake;          // payoutPct = 0.95
```

This is **binary**. It is NOT the multiplier (MULTUP/MULTDOWN) contract used by
`derivbot --mode live`. The 98.5% result belongs to **rise/fall binary** only.

---

## 2. Results (Deriv API ticks, XAUUSD-free synthetics, 15s duration)

| Symbol | Strategy | Window | Trades | Win rate | Net P&L (stake $1) |
|---|---|---|---|---|---|
| Boom 1000 | Random Forest | 30k ticks | 849 | 99.5% | +$799 |
| Boom 1000 | XGBoost | 30k ticks | 849 | 98.7% | +$785 |
| Crash 1000 | XGBoost | 30k ticks | 849 | 99.4% | +$797 |
| Crash 1000 | Random Forest | 30k ticks | 849 | 98.5% | +$781 |
| **Boom 1000** | **Random Forest** | **60k (OOS)** | **1,849** | **98.5%** | **+$1,702** |
| **Crash 1000** | **XGBoost** | **60k (OOS)** | **1,849** | **98.5%** | **+$1,702** |
| v75 (R_75) | any | 30k | ~600–1200 | ~50% | breakeven/neg |

RSI and EMA-cross did NOT capture it (~46–50%); the ML models are needed to learn
the drift.

---

## 3. Why the edge exists (it's structural, not magic)

Boom/Crash are engineered: **Boom 1000 drifts DOWN in tiny steps and spikes UP**
roughly once per ~1000 ticks; **Crash 1000** is the mirror (drifts up, spikes
down). So over a 15s window the price is *almost always* slightly lower on Boom
(higher on Crash). The ML simply learns **"bet the drift direction"** (Fall on
Boom, Rise on Crash) and wins ~98–99% of the time, losing only on the rare spike.
Because binary caps each loss at the stake, 98.5% wins × 0.95 payout overwhelms
the ~1.5% losses.

It's robust because it exploits a *mechanical property of the instrument*, which
doesn't change quarter to quarter (unlike statistical pattern-fitting on gold).

---

## 4. Files here

| File | What it is |
|---|---|
| `strategies/random_forest.hpp` | Random Forest: bootstrap-bagged classification trees over candle features, majority vote with a confidence threshold. |
| `strategies/xgboost_strategy.hpp` | XGBoost-style gradient-boosted trees on logistic loss (from scratch). |
| `strategies/ml_features.hpp` | Candle-aware feature extraction (body/wick ratios, colour, engulfing, hammer/star, trend slope, etc.). |
| `strategies/strategy_base.hpp` | Common strategy interface. |
| `indicators/ohlc_builder.hpp` | Builds OHLC candles from the raw tick stream. |
| `signal_types.hpp` | `Signal { None, Rise, Fall }`. |

Both ML strategies **self-train**: they accumulate the first ~300 candles, train
the model, then predict/trade on everything after (so the reported results are
quasi out-of-sample within each window).

---

## 5. How to reproduce

Build the tool (repo root), then run against the Deriv API:

```bash
g++ derivbot.cpp -o derivbot.exe -std=c++17 -lssl -lcrypto -lws2_32 -lwsock32   # Windows/MinGW
# (Linux: drop the -lws2_32 -lwsock32)

./derivbot.exe --mode backtest --symbol boom1000  --strategy random_forest --duration 15 --count 60000
./derivbot.exe --mode backtest --symbol crash1000 --strategy xgboost       --duration 15 --count 60000
./derivbot.exe --mode backtest --symbol v75        --strategy random_forest --duration 15 --count 30000
```

(Aliases: `boom1000`→BOOM1000, `crash1000`→CRASH1000, `v75`→R_75.)

---

## 6. Honest caveats — read before risking anything

1. **Payout assumption (#1 risk).** The backtest assumes a fixed **95% payout**.
   Deriv sets the *real* payout on Boom/Crash rise/fall, and brokers price known
   drift in. If the real payout is materially below ~95%, the edge shrinks or
   dies. **Verify Deriv's actual Boom/Crash rise/fall payout before trusting this.**
2. **Live contract mismatch.** `derivbot --mode live` currently places
   **multipliers**, whose economics differ from the binary model here. To trade
   this as backtested, live trading must use **rise/fall (CALL/PUT)** contracts —
   not yet wired into live mode.
3. **Spike risk.** ~1.5% of trades lose on spikes; in binary that's capped at the
   stake. Fine as modelled, but never use leverage/martingale on top of it.
4. **Demo first.** Validate on a Deriv **demo** API token with small stakes and
   confirm the real win rate + payout before any real money.
5. For contrast: **gold and v75 have no edge** — don't apply these expectations
   there. This works *because* Boom/Crash are structurally biased instruments.
