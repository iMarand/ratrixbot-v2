# Live Gold Trading on a Linux VPS (Deriv API, no MT5)

`--mode live` makes `derivbot` place **real orders on your Deriv demo account**
using **multiplier contracts** — natively on Linux, no MT5, no Wine, no GUI.
It streams ticks, runs your chosen strategy, and on a signal buys a
MULTUP/MULTDOWN contract with stop-loss / take-profit plus a bot-side trailing
stop. One position at a time, with full risk discipline.

> ⚠️ Use a **DEMO (virtual)** API token. The bot prints `[VIRTUAL/DEMO]` or
> `[REAL MONEY]` on startup and warns you if the token is real-money. It will
> place actual trades on whatever account the token belongs to.

## 1. Get a Deriv API token (demo)

1. Log into Deriv and **switch to your demo/virtual account** (top-right
   account switcher).
2. Go to **app.deriv.com → Account Settings → API token**.
3. Create a token with the **Read** and **Trade** scopes.
4. Copy it. (It is tied to the account that was active when you created it — make
   sure that was the demo account.)

## 2. Build on the Linux VPS

```bash
sudo apt install g++ libssl-dev nlohmann-json3-dev libboost-all-dev
g++ derivbot.cpp -o derivbot -std=c++17 -lssl -lcrypto
# (drop -lws2_32 -lwsock32 -- those are Windows-only)
```

## 3. Run the gold scalper

```bash
./derivbot --mode live \
  --symbol gold \
  --token YOUR_DEMO_TOKEN \
  --strategy rsi \
  --stake 1 \
  --multiplier 100 \
  --sl-amount 5 \
  --tp-amount 10 \
  --be-amount 5 \
  --trail-amount 3 \
  --cooldown 60 \
  --max-consec-losses 3 \
  --daily-loss 50
```

What the money flags mean (all in account currency):
- `--stake` — money put on each contract.
- `--sl-amount` / `--tp-amount` — close the trade at this **loss / profit** (calculated risk).
- `--be-amount` — once profit reaches this, the **trailing stop arms**.
- `--trail-amount` — after arming, if profit falls this far from its peak, the bot **closes to lock the gain**.
- `--daily-loss` — stop opening new trades once the session is down this much.

## 4. Keep it running on the VPS

```bash
# simplest: detached with a log
nohup ./derivbot --mode live --symbol gold --token YOUR_DEMO_TOKEN \
   --strategy rsi --stake 1 --multiplier 100 --sl-amount 5 --tp-amount 10 \
   --be-amount 5 --trail-amount 3 --cooldown 60 --max-consec-losses 3 \
   > live.log 2>&1 &

tail -f live.log     # watch it
```
For a long-lived setup, wrap it in a `systemd` service or a `tmux`/`screen`
session so it survives disconnects and restarts on reboot.

## Important notes / honest caveats

- **Gold is a real market**: it is **closed on weekends**, so the bot idles then.
- **Strategy warm-up:** `--strategy rsi` starts trading quickly. The ML
  strategies (`random_forest`, `xgboost`) **self-train on the first ~300 live
  candles** before they trade — at 15s candles that's ~75 minutes of warm-up,
  and they train on a small window. For live use, start with `rsi`; use the ML
  models for *research/backtesting* until we add saved-model loading.
- **Multipliers ≠ MT5 lots.** Risk is expressed as money amounts
  (`--sl-amount`), not pips/lots. A multiplier contract can lose at most your
  stake (Deriv auto-closes at a full stake loss), so set `--sl-amount` below
  `--stake`.
- **Test small first:** keep `--stake 1` and watch `live.log` for a full session
  before scaling. Past results never guarantee future ones.
