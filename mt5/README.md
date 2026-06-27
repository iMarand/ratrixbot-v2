# GoldScalper_Ratrix — MT5 Gold Scalping EA (DEMO)

A MetaTrader 5 Expert Advisor that scalps **Gold (XAUUSD)** using the same logic
the `derivbot` research tool refined:

- **Entry — EMA + RSI + candle confluence *score*.** Instead of one rigid rule,
  each bar earns points for: EMA(21)/EMA(50) trend, fast-EMA slope (momentum),
  RSI direction, RSI pullback turning up/down, candlestick reading (engulfing,
  hammer/shooting-star, piercing/dark-cloud from `candles.md`), a pullback to
  the fast EMA with a confirming candle, and an RSI-extreme bonus. It trades
  when one side's score reaches **`InpMinScore`** and beats the other side.
- **`InpMinScore` is your frequency dial:** **lower = more trades** (looser),
  higher = stricter/fewer. Default 3. This is the knob to fix "trades too
  little" — drop it to 2; if it overtrades, raise to 4.
- **`InpTradeWithTrendOnly`** (default off): set true to only take trades in the
  EMA trend direction.
- **Discipline (anti-overtrading):** spread guard, cooldown bars between trades,
  one position at a time, max trades/day, max daily loss, and an automatic pause
  after N consecutive losses.
- **Trade management:** fixed SL/TP in points, optional break-even + trailing.

> ⚠️ **DEMO ONLY.** The EA places real orders on whatever account the terminal
> is logged into. Keep it on a **demo** account until you have validated it
> yourself over many trades. Past backtest behaviour does not guarantee results.

> 🖥️ **Requires MT5 *desktop*.** The MT5 **web terminal**
> (`mt5-demo-web.deriv.com`) **cannot run Expert Advisors** — no EAs, no
> MetaEditor, no Python API. Download the **MT5 desktop app** and log in with the
> *same* Deriv-MT5 demo credentials (e.g. login `41139617` + your password +
> the Deriv demo server). Same account, same balance — just the desktop client.
> For gold/forex you need a Deriv MT5 **Financial** demo (the *Derived* account
> type only has synthetics, no XAUUSD).

## Risk sizing ("calculated risk")

By default the EA sizes each trade by **risk percent**, not a fixed lot:

- `InpUseRiskPercent = true`, `InpRiskPercent = 1.0` → each trade is sized so
  that hitting the stop loss costs ~1% of the account balance. On a $10k demo
  that's ~$100 risk per trade, and the lot auto-scales with your SL distance.
- Set `InpUseRiskPercent = false` to use the fixed `InpLots` instead.

The EA also **shifts the stop loss in your favour** once a trade moves into
profit: at `InpBreakEvenPoints` of profit it starts trailing the stop
`InpTrailPoints` behind price, locking in gains (never moving against you).

## Install

1. In MT5: **File → Open Data Folder**.
2. Copy `GoldScalper_Ratrix.mq5` into `MQL5/Experts/`.
3. In MetaEditor (F4) open the file and press **Compile** (F7). It should
   compile with 0 errors.
4. Back in MT5, open an **XAUUSD** chart, set it to **M1**, and drag the EA
   onto the chart from the Navigator.
5. Enable **Algo Trading** (the toolbar button) and tick *Allow Algo Trading* in
   the EA dialog's *Common* tab.

## Recommended first settings (gold, demo)

Gold spreads and point size vary by broker — **check your symbol's point value
and typical spread first**, then adjust:

| Input | Suggested start | Notes |
|---|---|---|
| `InpTimeframe` | M1 | scalping |
| `InpMinScore` | 3 | **frequency dial** — drop to 2 for more trades, raise to 4 for fewer/stricter |
| `InpStopLossPoints` | 250 | tune to gold volatility/ATR |
| `InpTakeProfitPoints` | 350 | aim for TP > SL |
| `InpMaxSpreadPoints` | 60 | **most important guard for gold** — skip when spread is wide |
| `InpCooldownBars` | 2 | spacing → fewer, better trades |
| `InpMaxConsecLosses` | 3 | auto-pause on a losing streak |
| `InpMaxTradesPerDay` | 10 | "trade only when necessary" |
| `InpUseRiskPercent` | true | size by % risk (recommended) |
| `InpRiskPercent` | 1.0 | ~$100 risk/trade on a $10k demo; lower to 0.5 to be safer |
| `InpLots` | 0.01 | only used if risk sizing is off |

## Validate before trusting it

1. **Strategy Tester** (Ctrl+R): pick `GoldScalper_Ratrix`, symbol XAUUSD,
   period M1, "Every tick based on real ticks", a few months of history.
2. Optimize `InpStopLossPoints` / `InpTakeProfitPoints` / `InpMaxSpreadPoints`
   for your broker's gold feed.
3. Only then run it forward on the live **demo** chart.

## How this maps to the research side

The `derivbot` C++ tool (Random Forest / XGBoost grid search) is for *finding
which candlestick/RSI configuration tends to win* on historical ticks. Once a
configuration grades well there, mirror its character here: e.g. if longer
holding times scored better in the multi-horizon analysis, widen
`InpTakeProfitPoints` and SL; if a tighter conviction reduced loss streaks,
keep `InpRequireCandleConfirm = true` and the trend filter on.
