# Top gold strategies (XAUUSD, $10k deposit, 1% risk)

The best configs found, over the 3-month backtest window (2026-03-26 → 2026-06-25).
Two cleared >$2K on generated ticks; RSI+PA is the one that held up on REAL ticks.

Each strategy = **one `.mq5` (the program) + one `.set` (its settings)**. Both
files for each are in this folder:

| Strategy | EA program (.mq5) | Settings (.set) | Net P&L (gen ticks, 3mo) |
|---|---|---|---|
| Breakout M1 | `StructureScalper_Ratrix.mq5` | `StructureScalper_Breakout_M1.set` | +$6,275 |
| Confluence | `GoldScalper_Ratrix.mq5` | `GoldScalper_Confluence_score4_SL180.set` | +$2,157 |
| RSI + price-action (M5) | `ClassicStrats_Ratrix.mq5` | `ClassicStrats_RSI_PriceAction_M5.set` | +$1,354 |

### Real-tick reality — full out-of-sample record (REAL ticks, per quarter)
| Strategy | Mar–Jun 26 | Dec–Mar 26 | Sep–Dec 25 | **9-mo net** |
|---|---|---|---|---|
| Breakout M1 | −$725 | (n/t) | (n/t) | negative |
| Confluence (score4 SL180) | +$1,197 | −$2,308 | −$1,872 | **−$2,983** ❌ |
| RSI + price-action (M5) | +$1,354 | +$36 | −$989 | **+$401** ⚠️ |

(Generated-tick 3-mo figures were +$6,275 / +$2,157 / +$1,354 respectively — all
inflated; trust the real-tick columns only.)

**Verdict:** every config that *looked* great on Mar–Jun fell apart on the other
quarters — classic single-window mirage. **RSI + price-action is the only one
net-positive over 9 months on real ticks, and only barely (+$401), carried by one
quarter.** None has a robust, durable edge. Treat all as demo experiments.

Note: tightening the trailing stop ("lock profit early") was tested and made it
WORSE (it shrank the winners more than it saved the losers). The `.set` here uses
the best (looser) trailing: break-even 250 / trail 250.

## Can the `.set` work without the `.mq5`? No.
A `.set` is only a list of input values — it has no trading logic. It **requires
the EA** (`.mq5`, which MT5 compiles to `.ex5`) to do anything. To use a
strategy you need BOTH: install the `.mq5`, compile it, then Load the `.set`.

## How to load in MT5
0. **Install the EA first:** copy the `.mq5` into `<MT5 data folder>/MQL5/Experts/`
   (File → Open Data Folder), open it in MetaEditor, press **Compile** (F7).
   (Both are already installed & compiled in your terminal.)
1. Open an **XAUUSD** chart (M1 for Breakout/Confluence, **M5 for RSI+PA**).
2. Drag the matching EA onto it (StructureScalper_Ratrix, GoldScalper_Ratrix, or ClassicStrats_Ratrix).
3. In the EA's **Inputs** tab → **Load** → pick the matching `.set` file.
   (Copies of these also live in `<MT5 data folder>/MQL5/Presets/` so the Load
   button finds them automatically.)
4. Enable **Algo Trading**. Keep it on a **DEMO** account.

## Honest note
These numbers are from MT5's *generated*-tick model over one 3-month window.
On **real ticks** and longer out-of-sample periods both drop to ~breakeven or
worse — the apparent edge did not persist in validation. Kept here because they
were the >$2K configs requested; treat as demo experiments, not a proven edge.
