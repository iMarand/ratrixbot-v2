//+------------------------------------------------------------------+
//|                                          GoldScalper_Ratrix.mq5   |
//|   Gold (XAUUSD) scalping Expert Advisor for MetaTrader 5         |
//|                                                                  |
//|   Ports the Ratrix/derivbot logic into a real MT5 EA you can     |
//|   drop on an XAUUSD chart in a DEMO account:                     |
//|                                                                  |
//|     * Entry  : RSI(14) "rebound" (exit-of-zone) -- arm when RSI  |
//|                pushes into overbought/oversold, fire when it     |
//|                rebounds back out (the reversal is confirming).   |
//|     * Filter : candlestick confirmation drawn from candles.md    |
//|                (engulfing, hammer/shooting-star, piercing/dark   |
//|                cloud, doji rejection) + optional trend filter.   |
//|     * Discipline: spread guard, cooldown between trades, one     |
//|                position at a time, max trades/day, max daily     |
//|                loss, pause after N consecutive losses.           |
//|                                                                  |
//|   SAFETY: intended for a DEMO account. It places real orders on  |
//|   whatever account the terminal is logged into -- keep it on     |
//|   demo until you have validated the behaviour yourself.          |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.00"
#property strict

#include <Trade/Trade.mqh>

//--- Entry / indicator inputs --------------------------------------
input group "Strategy (EMA + RSI + candle confluence)"
input ENUM_TIMEFRAMES InpTimeframe      = PERIOD_M1;   // Working timeframe (scalping: M1)
input int    InpRSIPeriod               = 14;          // RSI period
input double InpOverbought              = 70.0;        // RSI overbought (mean-reversion bonus)
input double InpOversold                = 30.0;        // RSI oversold (mean-reversion bonus)
input int    InpFastMA                  = 21;          // Fast EMA (trend + pullback)
input int    InpSlowMA                  = 50;          // Slow EMA (trend)
input int    InpMinScore                = 3;           // Confluence score needed to trade (LOWER = MORE trades)
input bool   InpTradeWithTrendOnly      = false;       // true = only trade in the EMA trend direction
input double InpPullbackPoints          = 0.0;         // "near fast EMA" distance in points (0 = auto from ATR)

//--- Risk / money management ---------------------------------------
input group "Risk"
input bool   InpUseRiskPercent          = true;        // Size lot by % risk (else use fixed InpLots)
input double InpRiskPercent             = 1.0;         // Risk per trade (% of balance) if above is true
input double InpLots                    = 0.01;        // Fixed lot size (used if risk sizing is off)
input int    InpStopLossPoints          = 250;         // Stop loss (points)  ~ for XAUUSD
input int    InpTakeProfitPoints        = 350;         // Take profit (points)
input bool   InpUseTrailing             = true;        // Trail stop after break-even
input int    InpBreakEvenPoints         = 150;         // Move SL to entry after this profit (points)
input int    InpTrailPoints             = 150;         // Trailing distance (points)

//--- Discipline (anti-overtrading) ---------------------------------
input group "Discipline"
input int    InpMaxSpreadPoints         = 200;         // Skip entry if spread above this (points; 0 = off)
input int    InpCooldownBars            = 1;           // Min bars between trades
input int    InpMaxTradesPerDay         = 40;          // Max new trades per day (0 = unlimited)
input int    InpMaxConsecLosses         = 4;           // Pause after this many losses in a row (0 = off)
input double InpMaxDailyLoss            = 0.0;         // Stop for the day after this $ loss (0 = off)

//--- Execution -----------------------------------------------------
input group "Execution"
input ulong  InpMagic                   = 26062026;    // Magic number (identifies this EA's trades)
input ulong  InpSlippagePoints          = 20;          // Max deviation (points)

//--- Globals -------------------------------------------------------
CTrade   trade;
int      rsiHandle  = INVALID_HANDLE;
int      fastHandle = INVALID_HANDLE;
int      slowHandle = INVALID_HANDLE;
int      atrHandle  = INVALID_HANDLE;
datetime lastBarTime = 0;
datetime lastTradeBarTime = 0;
int      lastScore = 0;   // for the status panel

//+------------------------------------------------------------------+
//| Initialisation                                                   |
//+------------------------------------------------------------------+
int OnInit()
{
   trade.SetExpertMagicNumber(InpMagic);
   trade.SetDeviationInPoints(InpSlippagePoints);
   trade.SetTypeFillingBySymbol(_Symbol);

   rsiHandle = iRSI(_Symbol, InpTimeframe, InpRSIPeriod, PRICE_CLOSE);
   if(rsiHandle == INVALID_HANDLE)
   {
      Print("Failed to create RSI handle");
      return(INIT_FAILED);
   }

   fastHandle = iMA(_Symbol, InpTimeframe, InpFastMA, 0, MODE_EMA, PRICE_CLOSE);
   slowHandle = iMA(_Symbol, InpTimeframe, InpSlowMA, 0, MODE_EMA, PRICE_CLOSE);
   atrHandle  = iATR(_Symbol, InpTimeframe, 14);
   if(fastHandle == INVALID_HANDLE || slowHandle == INVALID_HANDLE || atrHandle == INVALID_HANDLE)
   {
      Print("Failed to create EMA/ATR handles");
      return(INIT_FAILED);
   }

   long stopsLevel = SymbolInfoInteger(_Symbol, SYMBOL_TRADE_STOPS_LEVEL);
   PrintFormat("GoldScalper_Ratrix started on %s %s. DEMO USE ONLY.",
               _Symbol, EnumToString(InpTimeframe));
   PrintFormat("Symbol info: digits=%d point=%.5f min stops level=%d points. "
               "Your SL=%d TP=%d points.",
               _Digits, _Point, (int)stopsLevel, InpStopLossPoints, InpTakeProfitPoints);
   if(InpStopLossPoints <= stopsLevel || InpTakeProfitPoints <= stopsLevel)
      PrintFormat("WARNING: SL/TP below broker min stops level (%d). They will be "
                  "auto-widened to that minimum so orders aren't rejected.", (int)stopsLevel);
   return(INIT_SUCCEEDED);
}

//+------------------------------------------------------------------+
//| Cleanup                                                          |
//+------------------------------------------------------------------+
void OnDeinit(const int reason)
{
   if(rsiHandle  != INVALID_HANDLE) IndicatorRelease(rsiHandle);
   if(fastHandle != INVALID_HANDLE) IndicatorRelease(fastHandle);
   if(slowHandle != INVALID_HANDLE) IndicatorRelease(slowHandle);
   if(atrHandle  != INVALID_HANDLE) IndicatorRelease(atrHandle);
   Comment("");
}

//+------------------------------------------------------------------+
//| Main tick handler                                                |
//+------------------------------------------------------------------+
void OnTick()
{
   // Manage any open position on every tick (trailing/break-even).
   ManageOpenPosition();

   UpdateStatus();

   // Everything else is decided once per closed bar.
   datetime barTime = iTime(_Symbol, InpTimeframe, 0);
   if(barTime == lastBarTime) return;
   lastBarTime = barTime;

   OnNewBar();
}

//+------------------------------------------------------------------+
//| On-chart status panel                                            |
//+------------------------------------------------------------------+
void UpdateStatus()
{
   long   spread = SymbolInfoInteger(_Symbol, SYMBOL_SPREAD);
   int    consec = (InpMaxConsecLosses > 0) ? CountConsecutiveLosses() : 0;
   int    today  = TradesToday();
   string pos    = HasOpenPosition() ? "IN TRADE" : "flat";
   int    t      = TrendDirection();
   string trend  = (t > 0 ? "UP" : (t < 0 ? "DOWN" : "flat"));

   string txt = StringFormat(
      "GoldScalper_Ratrix  [DEMO]\n"
      "Symbol: %s   TF: %s\n"
      "Spread: %d pts (max %d)\n"
      "EMA trend: %s   last score: %d / %d\n"
      "Position: %s\n"
      "Trades today: %d / %d\n"
      "Consec losses: %d / %d\n"
      "Daily P/L: %.2f",
      _Symbol, EnumToString(InpTimeframe),
      (int)spread, InpMaxSpreadPoints,
      trend, lastScore, InpMinScore, pos,
      today, InpMaxTradesPerDay,
      consec, InpMaxConsecLosses,
      DailyProfit());
   Comment(txt);
}

//+------------------------------------------------------------------+
//| New-bar decision logic                                           |
//+------------------------------------------------------------------+
void OnNewBar()
{
   // One position at a time (classic scalper discipline).
   if(HasOpenPosition()) return;

   // --- Discipline gates -------------------------------------------
   if(!SpreadOK())                                   return;
   if(InpCooldownBars > 0 && BarsSinceLastTrade() < InpCooldownBars) return;
   if(InpMaxConsecLosses > 0 && CountConsecutiveLosses() >= InpMaxConsecLosses) return;
   if(InpMaxTradesPerDay > 0 && TradesToday() >= InpMaxTradesPerDay) return;
   if(InpMaxDailyLoss > 0.0 && DailyProfit() <= -InpMaxDailyLoss)    return;

   // --- EMA + RSI + candle confluence score ------------------------
   int dir = ComputeSignal();   // +1 = BUY, -1 = SELL, 0 = stand aside
   if(dir > 0)      OpenTrade(ORDER_TYPE_BUY);
   else if(dir < 0) OpenTrade(ORDER_TYPE_SELL);
}

//+------------------------------------------------------------------+
//| Confluence signal: combine EMA trend/pullback + RSI momentum +   |
//| candlestick reading into a score. Trade when one side's score    |
//| reaches InpMinScore (and beats the other side). Lower MinScore   |
//| => more trades; higher => stricter.                              |
//+------------------------------------------------------------------+
int ComputeSignal()
{
   double rsi[], fast[], slow[];
   if(CopyBuffer(rsiHandle, 0, 1, 2, rsi)   < 2) return(0); // rsi[0]=shift2, rsi[1]=shift1
   if(CopyBuffer(fastHandle,0, 1, 2, fast)  < 2) return(0); // fast[0]=shift2, fast[1]=shift1
   if(CopyBuffer(slowHandle,0, 1, 1, slow)  < 1) return(0);

   double rsiNow  = rsi[1],  rsiOld = rsi[0];
   double emaFast = fast[1], emaFastOld = fast[0];
   double emaSlow = slow[0];

   bool trendUp   = emaFast > emaSlow;
   bool trendDown = emaFast < emaSlow;
   bool fastRising = emaFast > emaFastOld;
   bool rsiRising  = rsiNow > rsiOld;

   bool bull = BullishCandleSignal();
   bool bear = BearishCandleSignal();

   // Price location vs the fast EMA on the last closed bar (pullback entry).
   double closeLast = iClose(_Symbol, InpTimeframe, 1);
   double tol = InpPullbackPoints * _Point;
   if(tol <= 0.0)
   {
      double atr[];
      if(CopyBuffer(atrHandle, 0, 1, 1, atr) == 1) tol = 0.5 * atr[0]; // auto: half an ATR
   }
   bool nearFastEMA = MathAbs(closeLast - emaFast) <= tol;

   int buy = 0, sell = 0;

   // 1) Trend (EMA fast vs slow)
   if(trendUp)   buy++;  if(trendDown) sell++;
   // 2) Momentum (fast EMA slope)
   if(fastRising) buy++; else sell++;
   // 3) RSI momentum, not yet stretched
   if(rsiRising && rsiNow < InpOverbought) buy++;
   if(!rsiRising && rsiNow > InpOversold)  sell++;
   // 4) Pullback turning: dipped then turning up / popped then turning down
   if(rsiNow < 45.0 && rsiRising)  buy++;
   if(rsiNow > 55.0 && !rsiRising) sell++;
   // 5) Candlestick reading (candles.md)
   if(bull) buy++;
   if(bear) sell++;
   // 6) Pullback to fast EMA with a confirming candle (best scalp entries)
   if(nearFastEMA && trendUp   && bull) buy++;
   if(nearFastEMA && trendDown && bear) sell++;
   // 7) Mean-reversion bonus at RSI extremes
   if(rsiNow <= InpOversold)   buy++;
   if(rsiNow >= InpOverbought) sell++;

   // Optional hard trend gate.
   if(InpTradeWithTrendOnly)
   {
      if(trendDown) buy = 0;
      if(trendUp)   sell = 0;
   }

   lastScore = MathMax(buy, sell);
   if(buy >= InpMinScore && buy > sell)  return(1);
   if(sell >= InpMinScore && sell > buy) return(-1);
   return(0);
}

//+------------------------------------------------------------------+
//| Open a trade with SL/TP in points                                |
//+------------------------------------------------------------------+
void OpenTrade(ENUM_ORDER_TYPE type)
{
   double point = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
   double ask   = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   double bid   = SymbolInfoDouble(_Symbol, SYMBOL_BID);

   // Respect the broker's minimum stop distance so orders aren't rejected.
   long stopsLevel = SymbolInfoInteger(_Symbol, SYMBOL_TRADE_STOPS_LEVEL);
   double slPts = MathMax((double)InpStopLossPoints,   (double)stopsLevel + 1);
   double tpPts = MathMax((double)InpTakeProfitPoints, (double)stopsLevel + 1);

   double price, sl, tp;
   if(type == ORDER_TYPE_BUY)
   {
      price = ask;
      sl = price - slPts * point;
      tp = price + tpPts * point;
   }
   else
   {
      price = bid;
      sl = price + slPts * point;
      tp = price - tpPts * point;
   }

   sl = NormalizeDouble(sl, _Digits);
   tp = NormalizeDouble(tp, _Digits);

   // Calculated risk: size the lot so a stop-out loses ~InpRiskPercent of
   // balance, instead of a fixed lot. Falls back to InpLots if disabled.
   double lots = InpUseRiskPercent ? CalcLotByRisk(slPts) : NormalizeLot(InpLots);

   bool ok = (type == ORDER_TYPE_BUY)
             ? trade.Buy(lots, _Symbol, 0.0, sl, tp, "Ratrix scalp")
             : trade.Sell(lots, _Symbol, 0.0, sl, tp, "Ratrix scalp");

   if(ok)
   {
      lastTradeBarTime = iTime(_Symbol, InpTimeframe, 0);
      PrintFormat("OPEN %s %.2f lots @ %.2f sl=%.2f tp=%.2f (risk %s)",
                  (type == ORDER_TYPE_BUY ? "BUY" : "SELL"), lots, price, sl, tp,
                  InpUseRiskPercent ? (DoubleToString(InpRiskPercent,2) + "%") : "fixed");
   }
   else
   {
      PrintFormat("Order failed: %d %s", trade.ResultRetcode(),
                  trade.ResultRetcodeDescription());
   }
}

//+------------------------------------------------------------------+
//| Position sizing                                                  |
//+------------------------------------------------------------------+
double NormalizeLot(double lot)
{
   double step   = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
   double minLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   double maxLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
   if(step <= 0) step = 0.01;
   lot = MathFloor(lot / step) * step;
   if(lot < minLot) lot = minLot;
   if(lot > maxLot) lot = maxLot;
   return lot;
}

// Lot size such that hitting the stop loses ~InpRiskPercent of balance.
double CalcLotByRisk(double slPoints)
{
   double balance   = AccountInfoDouble(ACCOUNT_BALANCE);
   double riskMoney = balance * InpRiskPercent / 100.0;
   double tickValue = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_VALUE);
   double tickSize  = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_SIZE);
   double point     = SymbolInfoDouble(_Symbol, SYMBOL_POINT);

   if(tickValue <= 0.0 || tickSize <= 0.0) return NormalizeLot(InpLots);

   double slPrice    = slPoints * point;                  // SL distance in price
   double lossPerLot = (slPrice / tickSize) * tickValue;  // $ lost per 1.0 lot at SL
   if(lossPerLot <= 0.0) return NormalizeLot(InpLots);

   double lot = riskMoney / lossPerLot;
   lot = NormalizeLot(lot);
   PrintFormat("Risk sizing: balance=%.2f risk=%.2f%% ($%.2f) slPts=%.0f -> %.2f lots",
               balance, InpRiskPercent, riskMoney, slPoints, lot);
   return lot;
}

//+------------------------------------------------------------------+
//| Break-even + trailing stop management                            |
//+------------------------------------------------------------------+
void ManageOpenPosition()
{
   if(!InpUseTrailing) return;
   if(!PositionSelectByMagic()) return;

   long   type  = PositionGetInteger(POSITION_TYPE);
   double open  = PositionGetDouble(POSITION_PRICE_OPEN);
   double sl    = PositionGetDouble(POSITION_SL);
   double tp    = PositionGetDouble(POSITION_TP);
   double point = SymbolInfoDouble(_Symbol, SYMBOL_POINT);
   double bid   = SymbolInfoDouble(_Symbol, SYMBOL_BID);
   double ask   = SymbolInfoDouble(_Symbol, SYMBOL_ASK);

   if(type == POSITION_TYPE_BUY)
   {
      double profitPts = (bid - open) / point;
      if(profitPts >= InpBreakEvenPoints)
      {
         double newSL = NormalizeDouble(bid - InpTrailPoints * point, _Digits);
         if(newSL > sl && newSL >= open) // never below entry once at break-even
            trade.PositionModify(_Symbol, newSL, tp);
      }
   }
   else if(type == POSITION_TYPE_SELL)
   {
      double profitPts = (open - ask) / point;
      if(profitPts >= InpBreakEvenPoints)
      {
         double newSL = NormalizeDouble(ask + InpTrailPoints * point, _Digits);
         if((newSL < sl || sl == 0.0) && newSL <= open)
            trade.PositionModify(_Symbol, newSL, tp);
      }
   }
}

//+------------------------------------------------------------------+
//| Candlestick pattern detection (candles.md)                       |
//| Uses the last two closed bars (shift 1 and shift 2).             |
//+------------------------------------------------------------------+
bool BullishCandleSignal()
{
   MqlRates r[];
   if(CopyRates(_Symbol, InpTimeframe, 1, 2, r) < 2) return(false);
   // r[1] = last closed (shift 1), r[0] = the one before (shift 2)
   MqlRates prev = r[0];
   MqlRates cur  = r[1];

   double rng  = MathMax(cur.high - cur.low, _Point);
   double body = MathAbs(cur.close - cur.open);
   double lowerWick = MathMin(cur.open, cur.close) - cur.low;
   double upperWick = cur.high - MathMax(cur.open, cur.close);

   bool isGreen  = cur.close > cur.open;
   bool hammer   = (lowerWick >= 0.5*rng && upperWick <= 0.15*rng && body <= 0.4*rng);
   bool bullEng  = (prev.close < prev.open && isGreen &&
                    cur.open <= prev.close && cur.close >= prev.open);
   double prevMid = (prev.open + prev.close) / 2.0;
   bool piercing = (prev.close < prev.open && isGreen &&
                    cur.open < prev.close && cur.close > prevMid && cur.close < prev.open);

   return(isGreen || hammer || bullEng || piercing);
}

bool BearishCandleSignal()
{
   MqlRates r[];
   if(CopyRates(_Symbol, InpTimeframe, 1, 2, r) < 2) return(false);
   MqlRates prev = r[0];
   MqlRates cur  = r[1];

   double rng  = MathMax(cur.high - cur.low, _Point);
   double body = MathAbs(cur.close - cur.open);
   double lowerWick = MathMin(cur.open, cur.close) - cur.low;
   double upperWick = cur.high - MathMax(cur.open, cur.close);

   bool isRed       = cur.close < cur.open;
   bool shootingStar= (upperWick >= 0.5*rng && lowerWick <= 0.15*rng && body <= 0.4*rng);
   bool bearEng     = (prev.close > prev.open && isRed &&
                       cur.open >= prev.close && cur.close <= prev.open);
   double prevMid   = (prev.open + prev.close) / 2.0;
   bool darkCloud   = (prev.close > prev.open && isRed &&
                       cur.open > prev.close && cur.close < prevMid && cur.close > prev.open);

   return(isRed || shootingStar || bearEng || darkCloud);
}

//+------------------------------------------------------------------+
//| Trend direction from EMA cross: +1 up, -1 down, 0 flat           |
//+------------------------------------------------------------------+
int TrendDirection()
{
   double fast[], slow[];
   if(CopyBuffer(fastHandle, 0, 1, 1, fast) < 1) return(0);
   if(CopyBuffer(slowHandle, 0, 1, 1, slow) < 1) return(0);
   if(fast[0] > slow[0]) return(1);
   if(fast[0] < slow[0]) return(-1);
   return(0);
}

//+------------------------------------------------------------------+
//| Helpers: position / spread / counters                            |
//+------------------------------------------------------------------+
bool PositionSelectByMagic()
{
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) == _Symbol &&
         (ulong)PositionGetInteger(POSITION_MAGIC) == InpMagic)
         return(true);
   }
   return(false);
}

bool HasOpenPosition()
{
   return PositionSelectByMagic();
}

bool SpreadOK()
{
   long spread = SymbolInfoInteger(_Symbol, SYMBOL_SPREAD);
   return(InpMaxSpreadPoints <= 0 || spread <= InpMaxSpreadPoints);
}

int BarsSinceLastTrade()
{
   if(lastTradeBarTime == 0) return(1000000);
   int bars = iBarShift(_Symbol, InpTimeframe, lastTradeBarTime, false);
   return(bars < 0 ? 1000000 : bars);
}

//--- Scan today's closed deals for this EA --------------------------
datetime StartOfToday()
{
   MqlDateTime dt;
   TimeToStruct(TimeCurrent(), dt);
   dt.hour = 0; dt.min = 0; dt.sec = 0;
   return StructToTime(dt);
}

int TradesToday()
{
   if(!HistorySelect(StartOfToday(), TimeCurrent())) return(0);
   int count = 0;
   int deals = HistoryDealsTotal();
   for(int i = 0; i < deals; i++)
   {
      ulong ticket = HistoryDealGetTicket(i);
      if((ulong)HistoryDealGetInteger(ticket, DEAL_MAGIC) != InpMagic) continue;
      if(HistoryDealGetString(ticket, DEAL_SYMBOL) != _Symbol)         continue;
      if(HistoryDealGetInteger(ticket, DEAL_ENTRY) == DEAL_ENTRY_IN)   count++;
   }
   return(count);
}

double DailyProfit()
{
   if(!HistorySelect(StartOfToday(), TimeCurrent())) return(0.0);
   double pnl = 0.0;
   int deals = HistoryDealsTotal();
   for(int i = 0; i < deals; i++)
   {
      ulong ticket = HistoryDealGetTicket(i);
      if((ulong)HistoryDealGetInteger(ticket, DEAL_MAGIC) != InpMagic) continue;
      if(HistoryDealGetString(ticket, DEAL_SYMBOL) != _Symbol)         continue;
      pnl += HistoryDealGetDouble(ticket, DEAL_PROFIT)
           + HistoryDealGetDouble(ticket, DEAL_SWAP)
           + HistoryDealGetDouble(ticket, DEAL_COMMISSION);
   }
   return(pnl);
}

// Count consecutive losing closed trades (most recent backwards).
int CountConsecutiveLosses()
{
   // Look back up to 14 days of history for this EA's closed deals.
   datetime from = TimeCurrent() - 14 * 24 * 60 * 60;
   if(!HistorySelect(from, TimeCurrent())) return(0);

   int deals = HistoryDealsTotal();
   int streak = 0;
   for(int i = deals - 1; i >= 0; i--)
   {
      ulong ticket = HistoryDealGetTicket(i);
      if((ulong)HistoryDealGetInteger(ticket, DEAL_MAGIC) != InpMagic) continue;
      if(HistoryDealGetString(ticket, DEAL_SYMBOL) != _Symbol)         continue;
      if(HistoryDealGetInteger(ticket, DEAL_ENTRY) != DEAL_ENTRY_OUT)  continue;

      double profit = HistoryDealGetDouble(ticket, DEAL_PROFIT)
                    + HistoryDealGetDouble(ticket, DEAL_SWAP)
                    + HistoryDealGetDouble(ticket, DEAL_COMMISSION);
      if(profit < 0.0) streak++;
      else break; // a win (or break-even) ends the losing streak
   }
   return(streak);
}
//+------------------------------------------------------------------+
