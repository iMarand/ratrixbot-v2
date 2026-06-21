# Ratrix Bot v2 (R-bot 2) 📈🤖

An advanced, self-learning, multi-indicator trading engine for Deriv synthetic indices. Built entirely in high-performance C++ using Reinforcement Learning (Q-Learning) and massive grid-search optimization.

> **Disclaimer:** Ratrix Bot v2 is built for paper trading and historical backtesting. Synthetic indices are RNG-generated. Always test out-of-sample before risking capital. There are no real-money execution capabilities currently built in.

---

## ✨ Features

- **Reinforcement Learning Meta-Agent**: Uses Q-Learning to adapt to shifting market regimes (Trend, Volatility, Momentum).
- **Automated Grid Search**: Automatically iterates through thousands of parameter combinations across 6 different indicator strategies to find the perfect setup.
- **Continuous Learning Loop**: Can run indefinitely on a VPS, fetching new data, re-optimizing, and retraining its AI "brain" every 5 minutes.
- **Multi-Indicator Strategies**: Supports RSI, MACD, Bollinger Bands, Stochastic, EMA Crossovers, and a "Multi-Confluence" strategy that blends them all.
- **Results Manager**: Automatically exports the best strategies to JSON, allowing you to instantly load the `#1` ranked strategy into a live paper trading feed without manually typing parameters.
- **Fully Asynchronous**: Uses `Boost.Beast` for highly efficient WebSocket data streaming from the Deriv API.

---

## 🛠️ Installation & Compilation

Ratrix Bot is a high-performance C++ application. It is designed to be compiled easily with a single command. 

### Windows (MSYS2 MinGW64)
1. Install [MSYS2](https://www.msys2.org/).
2. Open the MSYS2 UCRT64 or MinGW64 terminal and install the required dependencies:
   ```bash
   pacman -S mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-nlohmann-json
   ```
3. Compile the bot:
   ```bash
   g++ derivbot.cpp -o derivbot.exe -std=c++17 -lssl -lcrypto -lws2_32 -lwsock32
   ```

### Linux / macOS
*Ensure you have `g++`, `libboost-all-dev`, `libssl-dev`, and `nlohmann-json3-dev` installed.*
```bash
g++ derivbot.cpp -o derivbot -std=c++17 -lssl -lcrypto -lboost_system
```

---

## 🚀 Quick Start & Best Workflows

### 1. The "Golden Loop" (Continuous Training)
The most powerful way to use Ratrix Bot v2 is to let it optimize itself. This command runs the training engine in an infinite loop, continuously adapting to the market.

```bash
./derivbot.exe --train --symbols jump10,jump25,jump50,v75,v100 --autoadjust
```

### 2. View Results
After letting it train, see what the AI accomplished:
```bash
./derivbot.exe --list-runs
```

### 3. Deploy to Live Paper Trading
Take a timestamped run ID from the step above, and deploy its `#1` ranked strategy directly into the live paper trading feed.
```bash
./derivbot.exe --mode paper --symbol jump10 --load-run run_20260621_234305 --rank 1
```

---

## 📚 Full Documentation

For a comprehensive list of all CLI flags, available strategies, parameter tweaking, and detailed workflows, please open the included **`doc.html`** file in your web browser. 

It contains a beautiful, fully-searchable UI with everything you need to know to master the bot.

---

## 🏗️ Project Structure

- `derivbot.cpp` - The core application entry point, CLI parser, and WebSocket manager.
- `strategies/` - Implementations of all technical indicators and trading logic.
- `train/` - The Grid Search and Results Management engine.
- `rl/` - The Reinforcement Learning (Q-learning) system and State Feature Extraction.
- `doc.html` - The official documentation website.
