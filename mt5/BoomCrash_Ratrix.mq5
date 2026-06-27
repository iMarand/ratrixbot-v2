//+------------------------------------------------------------------+
//|                                            BoomCrash_Ratrix.mq5   |
//|   Boom/Crash index EA for Deriv MT5 (Derived account).          |
//|                                                                  |
//|   Boom indices drift DOWN in small steps and SPIKE UP rarely.    |
//|   Crash indices drift UP and SPIKE DOWN rarely. Two strategies:  |
//|                                                                  |
//|     SPIKE mode  : trade WITH the spike (Boom=BUY, Crash=SELL).   |
//|                   Small risk per trade, large TP / trailing to   |
//|                   ride an occasional big spike. Limited downside. |
//|     DRIFT mode  : trade WITH the drift (Boom=SELL, Crash=BUY).   |
//|                   Small TP captures the steady drift; a MANDATORY |
//|                   hard stop caps the rare spike against you.      |
//|                                                                  |
//|   Spike direction is auto-detected from the symbol name.         |
//|   DEMO ONLY. Must be validated out-of-sample before trusting.    |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.00"
#property strict

#include <Trade/Trade.mqh>

enum ENUM_BC_MODE { MODE_SPIKE=0, MODE_DRIFT=1 };

input group "Strategy"
input ENUM_BC_MODE   InpMode        = MODE_SPIKE;   // SPIKE=ride spikes, DRIFT=ride the drift
input ENUM_TIMEFRAMES InpTimeframe  = PERIOD_M1;    // Working timeframe
input int    InpStretchCandles      = 4;            // SPIKE: enter after N drift candles in a row
input int    InpRSIPeriod           = 14;           // RSI (entry filter)
input double InpDriftRSI            = 50.0;         // DRIFT: enter when RSI past this toward the drift
input int    InpSpikeUpOverride     = -1;           // -1 auto from name; 1=spikes up (Boom); 0=spikes down (Crash)

input group "Risk (hard stop is mandatory)"
input bool   InpUseRiskPercent      = true;         // size lot by % risk
input double InpRiskPercent         = 0.5;          // risk per trade (% of balance)
input double InpLots                = 0.20;         // fixed lot if risk sizing off (Boom/Crash min often 0.2)
input int    InpStopLossPoints      = 1500;         // HARD stop (points) -- caps the spike
input int    InpTakeProfitPoints    = 3000;         // take profit (points)
input bool   InpUseTrailing         = true;         // trail to lock a spike's profit
input int    InpBreakEvenPoints     = 1000;         // start trailing after this profit (points)
input int    InpTrailPoints         = 800;          // trailing distance (points)

input group "Discipline"
input int    InpCooldownBars        = 1;            // min bars between trades
input int    InpMaxConsecLosses     = 6;            // pause after N losses in a row (0=off)
input double InpMaxDailyLoss        = 0.0;          // stop after $ loss for the day (0=off)
input int    InpMaxSpreadPoints     = 0;            // 0=off (synthetics spread is usually fixed)

input group "Execution"
input ulong  InpMagic               = 27062026;
input ulong  InpSlippagePoints      = 50;

CTrade   trade;
int      rsiHandle;
datetime lastBarTime=0, lastTradeBarTime=0;
bool     g_spikeUp=true;   // resolved spike direction

//+------------------------------------------------------------------+
int OnInit()
{
   trade.SetExpertMagicNumber(InpMagic);
   trade.SetDeviationInPoints(InpSlippagePoints);
   trade.SetTypeFillingBySymbol(_Symbol);

   rsiHandle = iRSI(_Symbol, InpTimeframe, InpRSIPeriod, PRICE_CLOSE);
   if(rsiHandle==INVALID_HANDLE){ Print("RSI handle failed"); return(INIT_FAILED); }

   // Resolve spike direction.
   if(InpSpikeUpOverride==1)      g_spikeUp=true;
   else if(InpSpikeUpOverride==0) g_spikeUp=false;
   else
   {
      string s=_Symbol; StringToUpper(s);
      if(StringFind(s,"BOOM")>=0)  g_spikeUp=true;
      else if(StringFind(s,"CRASH")>=0) g_spikeUp=false;
      else { Print("Cannot auto-detect Boom/Crash from '",_Symbol,"'. Set InpSpikeUpOverride."); }
   }
   PrintFormat("BoomCrash_Ratrix on %s %s | mode=%s | spikes=%s (DEMO ONLY)",
               _Symbol, EnumToString(InpTimeframe),
               (InpMode==MODE_SPIKE?"SPIKE":"DRIFT"), (g_spikeUp?"UP(Boom)":"DOWN(Crash)"));
   return(INIT_SUCCEEDED);
}

void OnDeinit(const int reason)
{
   if(rsiHandle!=INVALID_HANDLE) IndicatorRelease(rsiHandle);
   Comment("");
}

void OnTick()
{
   ManageOpenPosition();
   UpdateStatus();
   datetime bt=iTime(_Symbol,InpTimeframe,0);
   if(bt==lastBarTime) return;
   lastBarTime=bt;
   OnNewBar();
}

//+------------------------------------------------------------------+
void OnNewBar()
{
   if(HasOpenPosition()) return;
   if(InpCooldownBars>0 && BarsSinceLastTrade()<InpCooldownBars) return;
   if(InpMaxConsecLosses>0 && CountConsecutiveLosses()>=InpMaxConsecLosses) return;
   if(InpMaxDailyLoss>0.0 && DailyProfit()<=-InpMaxDailyLoss) return;
   if(InpMaxSpreadPoints>0 && SymbolInfoInteger(_Symbol,SYMBOL_SPREAD)>InpMaxSpreadPoints) return;

   int dir = ComputeSignal();   // +1 buy, -1 sell, 0 none
   if(dir>0)      OpenTrade(ORDER_TYPE_BUY);
   else if(dir<0) OpenTrade(ORDER_TYPE_SELL);
}

//+------------------------------------------------------------------+
//| Signal logic for both modes.                                     |
//+------------------------------------------------------------------+
int ComputeSignal()
{
   double rsi[];
   if(CopyBuffer(rsiHandle,0,1,1,rsi)<1) return(0);
   double r = rsi[0];

   // Count consecutive drift candles on the last closed bars.
   // Boom drift = down (red); Crash drift = up (green).
   int driftRun = ConsecutiveDriftCandles();

   if(InpMode==MODE_SPIKE)
   {
      // Trade WITH the spike after price has stretched in the drift direction
      // (a spike/bounce becomes more likely). Boom -> BUY, Crash -> SELL.
      if(driftRun >= InpStretchCandles)
         return(g_spikeUp ? 1 : -1);
      return(0);
   }
   else // MODE_DRIFT
   {
      // Trade WITH the drift, using RSI as a light pullback filter so we enter
      // on a small counter-move. Boom -> SELL (drift down), Crash -> BUY.
      if(g_spikeUp) // Boom: sell the drift, prefer entering after a small up-tick
      { if(r >= InpDriftRSI) return(-1); }
      else          // Crash: buy the drift, after a small down-tick
      { if(r <= (100.0 - InpDriftRSI)) return(1); }
      return(0);
   }
}

// Consecutive candles in the DRIFT direction on the last closed bars.
int ConsecutiveDriftCandles()
{
   MqlRates r[];
   int need=10;
   if(CopyRates(_Symbol,InpTimeframe,1,need,r)<need) return(0);
   // r is non-series: r[need-1] = most recent closed (shift 1)
   int run=0;
   for(int i=need-1;i>=0;i--)
   {
      bool driftCandle = g_spikeUp ? (r[i].close<r[i].open)   // Boom drift = red
                                   : (r[i].close>r[i].open);  // Crash drift = green
      if(driftCandle) run++; else break;
   }
   return(run);
}

//+------------------------------------------------------------------+
//| Orders + management (hard stop always set)                       |
//+------------------------------------------------------------------+
void OpenTrade(ENUM_ORDER_TYPE type)
{
   double point=SymbolInfoDouble(_Symbol,SYMBOL_POINT);
   double ask=SymbolInfoDouble(_Symbol,SYMBOL_ASK), bid=SymbolInfoDouble(_Symbol,SYMBOL_BID);
   long stops=SymbolInfoInteger(_Symbol,SYMBOL_TRADE_STOPS_LEVEL);
   double slPts=MathMax((double)InpStopLossPoints,(double)stops+1);
   double tpPts=MathMax((double)InpTakeProfitPoints,(double)stops+1);

   double price,sl,tp;
   if(type==ORDER_TYPE_BUY){ price=ask; sl=price-slPts*point; tp=price+tpPts*point; }
   else                    { price=bid; sl=price+slPts*point; tp=price-tpPts*point; }
   sl=NormalizeDouble(sl,_Digits); tp=NormalizeDouble(tp,_Digits);

   double lots = InpUseRiskPercent ? CalcLotByRisk(slPts) : NormalizeLot(InpLots);
   bool ok=(type==ORDER_TYPE_BUY)
           ? trade.Buy(lots,_Symbol,0.0,sl,tp,"Ratrix BC")
           : trade.Sell(lots,_Symbol,0.0,sl,tp,"Ratrix BC");
   if(ok){ lastTradeBarTime=iTime(_Symbol,InpTimeframe,0);
           PrintFormat("OPEN %s %.2f lots sl=%.5f tp=%.5f",(type==ORDER_TYPE_BUY?"BUY":"SELL"),lots,sl,tp); }
   else PrintFormat("Order failed: %d %s",trade.ResultRetcode(),trade.ResultRetcodeDescription());
}

double NormalizeLot(double lot)
{
   double step=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_STEP);
   double mn=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MIN), mx=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MAX);
   if(step<=0) step=0.01;
   lot=MathFloor(lot/step)*step;
   if(lot<mn) lot=mn; if(lot>mx) lot=mx; return lot;
}
double CalcLotByRisk(double slPoints)
{
   double bal=AccountInfoDouble(ACCOUNT_BALANCE), risk=bal*InpRiskPercent/100.0;
   double tv=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_VALUE), ts=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_SIZE);
   double pt=SymbolInfoDouble(_Symbol,SYMBOL_POINT);
   if(tv<=0||ts<=0) return NormalizeLot(InpLots);
   double lossPerLot=(slPoints*pt/ts)*tv;
   if(lossPerLot<=0) return NormalizeLot(InpLots);
   return NormalizeLot(risk/lossPerLot);
}

void ManageOpenPosition()
{
   if(!InpUseTrailing) return;
   if(!PositionSelectByMagic()) return;
   long type=PositionGetInteger(POSITION_TYPE);
   double open=PositionGetDouble(POSITION_PRICE_OPEN), sl=PositionGetDouble(POSITION_SL), tp=PositionGetDouble(POSITION_TP);
   double pt=SymbolInfoDouble(_Symbol,SYMBOL_POINT);
   double bid=SymbolInfoDouble(_Symbol,SYMBOL_BID), ask=SymbolInfoDouble(_Symbol,SYMBOL_ASK);
   if(type==POSITION_TYPE_BUY)
   { if((bid-open)/pt>=InpBreakEvenPoints){ double n=NormalizeDouble(bid-InpTrailPoints*pt,_Digits); if(n>sl&&n>=open) trade.PositionModify(_Symbol,n,tp);} }
   else if(type==POSITION_TYPE_SELL)
   { if((open-ask)/pt>=InpBreakEvenPoints){ double n=NormalizeDouble(ask+InpTrailPoints*pt,_Digits); if((n<sl||sl==0.0)&&n<=open) trade.PositionModify(_Symbol,n,tp);} }
}

//+------------------------------------------------------------------+
//| Helpers                                                          |
//+------------------------------------------------------------------+
bool PositionSelectByMagic()
{
   for(int i=PositionsTotal()-1;i>=0;i--){ ulong t=PositionGetTicket(i); if(t==0) continue;
      if(PositionGetString(POSITION_SYMBOL)==_Symbol && (ulong)PositionGetInteger(POSITION_MAGIC)==InpMagic) return(true); }
   return(false);
}
bool HasOpenPosition(){ return PositionSelectByMagic(); }
int  BarsSinceLastTrade(){ if(lastTradeBarTime==0) return(1000000); int b=iBarShift(_Symbol,InpTimeframe,lastTradeBarTime,false); return(b<0?1000000:b); }
datetime StartOfToday(){ MqlDateTime dt; TimeToStruct(TimeCurrent(),dt); dt.hour=0;dt.min=0;dt.sec=0; return StructToTime(dt); }
double DailyProfit()
{
   if(!HistorySelect(StartOfToday(),TimeCurrent())) return(0.0);
   double pnl=0; int d=HistoryDealsTotal();
   for(int i=0;i<d;i++){ ulong t=HistoryDealGetTicket(i);
      if((ulong)HistoryDealGetInteger(t,DEAL_MAGIC)!=InpMagic) continue;
      if(HistoryDealGetString(t,DEAL_SYMBOL)!=_Symbol) continue;
      pnl+=HistoryDealGetDouble(t,DEAL_PROFIT)+HistoryDealGetDouble(t,DEAL_SWAP)+HistoryDealGetDouble(t,DEAL_COMMISSION); }
   return(pnl);
}
int CountConsecutiveLosses()
{
   datetime from=TimeCurrent()-14*24*60*60;
   if(!HistorySelect(from,TimeCurrent())) return(0);
   int d=HistoryDealsTotal(), streak=0;
   for(int i=d-1;i>=0;i--){ ulong t=HistoryDealGetTicket(i);
      if((ulong)HistoryDealGetInteger(t,DEAL_MAGIC)!=InpMagic) continue;
      if(HistoryDealGetString(t,DEAL_SYMBOL)!=_Symbol) continue;
      if(HistoryDealGetInteger(t,DEAL_ENTRY)!=DEAL_ENTRY_OUT) continue;
      double pf=HistoryDealGetDouble(t,DEAL_PROFIT)+HistoryDealGetDouble(t,DEAL_SWAP)+HistoryDealGetDouble(t,DEAL_COMMISSION);
      if(pf<0) streak++; else break; }
   return(streak);
}
void UpdateStatus()
{
   string pos=HasOpenPosition()?"IN TRADE":"flat";
   string txt=StringFormat("BoomCrash_Ratrix [DEMO]\nSymbol: %s  TF: %s\nMode: %s   Spikes: %s\nDrift run: %d / %d\nPosition: %s\nConsec losses: %d / %d",
      _Symbol, EnumToString(InpTimeframe),
      (InpMode==MODE_SPIKE?"SPIKE":"DRIFT"), (g_spikeUp?"UP":"DOWN"),
      ConsecutiveDriftCandles(), InpStretchCandles, pos,
      (InpMaxConsecLosses>0?CountConsecutiveLosses():0), InpMaxConsecLosses);
   Comment(txt);
}
//+------------------------------------------------------------------+
