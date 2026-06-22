# DerivBot Command Cheat Sheet

Here is a quick reference guide to the most useful commands for running and optimizing your self-learning trading bot.

## 1. Compilation
Always run this command after making any changes to the C++ code to rebuild the executable.
```bash
g++ derivbot.cpp -o derivbot.exe -std=c++17 -lssl -lcrypto -lws2_32 -lwsock32
```

---

## 2. Training & Optimization (The "Brain")
These commands use historical data to grid-search parameters and train the Q-learning AI.

**⭐ Best overall command for continuous learning:**
This will download history, find the best parameters, train the AI, save the results, wait 5 minutes, and repeat forever (or until you hit `Ctrl+C`). It covers 3 popular volatile symbols.
```bash
./derivbot.exe --train --symbols jump10,jump25,v75 --autoadjust
```

**Train across all available Deriv symbols:**
Use this to do a massive sweep of the entire Deriv catalog to see which markets currently have the most predictable patterns.
```bash
./derivbot.exe --train --all-symbols --top-k 5
```

**Train for a specific duration:**
Limit the training loop to exactly 4 hours before it automatically shuts down.
```bash
./derivbot.exe --train --symbols jump10,v75,gold --autoadjust --train-hours 4
```

---

## 3. Backtesting Specific Strategies
Once you check the `results/` folder and find a set of parameters that work incredibly well, you can run a standalone backtest to verify it.

**Backtest the Multi-Confluence (God) Strategy:**
```bash
./derivbot.exe --mode backtest --symbol jump10 --strategy multi_confluence --duration 15
```

**Backtest the Bollinger Band Strategy:**
```bash
./derivbot.exe --mode backtest --symbol v75 --strategy bollinger --duration 15
```

**Backtest with Custom Thresholds (e.g., RSI):**
```bash
./derivbot.exe --mode backtest --symbol jump25 --strategy rsi --oversold 20 --overbought 80 --rsi-period 14
```

---

## 4. Paper Trading (Live Market)
Once you trust a strategy, you can let it run on the live data feed. It will track virtual P&L in memory and save trades to a CSV file.

**Paper Trade the Multi-Confluence Strategy:**
```bash
./derivbot.exe --mode paper --symbol jump10 --strategy multi_confluence --duration 15 --csv live_trades.csv
```

**Paper Trade with a Cooldown:**
Adding `--cooldown 60` prevents the bot from opening another trade for 60 seconds after a signal fires, avoiding a string of losses on the same sudden price spike.
```bash
./derivbot.exe --mode paper --symbol v75 --strategy macd --cooldown 60
```

---

## 5. Results Management & Strategy Loader
Manage your historical training runs and instantly load the best strategies from them.

**List all completed training runs:**
See a list of every folder inside `results/` along with the symbols trained in that run.
```bash
./derivbot.exe --list-runs
```

**Load a saved strategy into a Backtest:**
Instead of typing out `--strategy`, `--oversold`, `--ema-fast`, etc., just load the perfect parameters directly from the JSON summary of a training run! `--rank 1` loads the top performing strategy.
```bash
./derivbot.exe --mode backtest --symbol jump10 --load-run run_20260621_234305 --rank 1
```

**Load a saved strategy into Paper Trading:**
Same as above, but runs it live!
```bash
./derivbot.exe --mode paper --symbol jump10 --load-run run_20260621_234305 --rank 1
```

**Delete a specific training run:**
```bash
./derivbot.exe --delete-run run_20260621_234305
```

**Clear all training runs:**
```bash
./derivbot.exe --delete-all-runs
```

---

## 6. Utilities
**List all available market symbols:**
```bash
./derivbot.exe --list-symbols
```

**View the Help Menu:**
```bash
./derivbot.exe --help
```
