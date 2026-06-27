//+------------------------------------------------------------------+
//|                                         ClassicStrats_Ratrix.mq5  |
//|   Two classic strategies for testing on gold, selectable:        |
//|     STRAT_EMA_CROSS : fast EMA crossing slow EMA (trend follow). |
//|     STRAT_RSI_PA    : RSI extreme + price-action candle confirm. |
//|   Hard SL/TP, % risk sizing, trailing, discipline. DEMO ONLY.    |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.00"
#property strict
#include <Trade/Trade.mqh>

enum ENUM_STRAT { STRAT_EMA_CROSS=0, STRAT_RSI_PA=1 };

input group "Strategy"
input ENUM_STRAT      InpStrategy   = STRAT_EMA_CROSS; // which strategy
input ENUM_TIMEFRAMES InpTimeframe  = PERIOD_M5;
input int    InpFastEMA  = 9;     // EMA cross: fast
input int    InpSlowEMA  = 21;    // EMA cross: slow
input int    InpRSIPeriod= 14;    // RSI
input double InpRSIOS    = 30.0;  // RSI oversold (RSI+PA)
input double InpRSIOB    = 70.0;  // RSI overbought (RSI+PA)
input bool   InpUseTrendAlign = true; // RSI+PA: buy dips only above the long-term trend EMA / sell below
input int    InpTrendEMA      = 200;  // long-term trend reference for the filter
input bool   InpUseADX        = false;// RSI+PA: also require a ranging market (ADX below max)
input int    InpADXPeriod     = 14;   // ADX period (regime filter)
input double InpADXMax        = 30.0; // skip RSI+PA when ADX above this (too trendy)

input group "Risk"
input bool   InpUseRiskPercent = true;
input double InpRiskPercent    = 1.0;
input double InpLots           = 0.01;
input int    InpStopLossPoints = 400;
input int    InpTakeProfitPoints = 600;
input bool   InpUseTrailing    = true;
input int    InpBreakEvenPoints = 250;
input int    InpTrailPoints     = 250;

input group "Discipline"
input int    InpMaxSpreadPoints = 200;
input int    InpCooldownBars    = 1;
input int    InpMaxConsecLosses = 4;
input double InpMaxDailyLoss     = 0.0;

input group "Execution"
input ulong  InpMagic = 27062029;
input ulong  InpSlippagePoints = 20;

CTrade trade;
int rsiHandle, fastHandle, slowHandle, adxHandle, trendHandle;
datetime lastBarTime=0, lastTradeBarTime=0;

int OnInit()
{
   trade.SetExpertMagicNumber(InpMagic);
   trade.SetDeviationInPoints(InpSlippagePoints);
   trade.SetTypeFillingBySymbol(_Symbol);
   rsiHandle = iRSI(_Symbol,InpTimeframe,InpRSIPeriod,PRICE_CLOSE);
   fastHandle= iMA(_Symbol,InpTimeframe,InpFastEMA,0,MODE_EMA,PRICE_CLOSE);
   slowHandle= iMA(_Symbol,InpTimeframe,InpSlowEMA,0,MODE_EMA,PRICE_CLOSE);
   adxHandle = iADX(_Symbol,InpTimeframe,InpADXPeriod);
   trendHandle = iMA(_Symbol,InpTimeframe,InpTrendEMA,0,MODE_EMA,PRICE_CLOSE);
   if(rsiHandle==INVALID_HANDLE||fastHandle==INVALID_HANDLE||slowHandle==INVALID_HANDLE||adxHandle==INVALID_HANDLE||trendHandle==INVALID_HANDLE)
   { Print("handle failed"); return(INIT_FAILED); }
   PrintFormat("ClassicStrats on %s %s | strat=%s (DEMO)",
      _Symbol, EnumToString(InpTimeframe), (InpStrategy==STRAT_EMA_CROSS?"EMA_CROSS":"RSI_PA"));
   return(INIT_SUCCEEDED);
}
void OnDeinit(const int reason)
{
   if(rsiHandle!=INVALID_HANDLE) IndicatorRelease(rsiHandle);
   if(fastHandle!=INVALID_HANDLE) IndicatorRelease(fastHandle);
   if(slowHandle!=INVALID_HANDLE) IndicatorRelease(slowHandle);
   if(adxHandle!=INVALID_HANDLE) IndicatorRelease(adxHandle);
   if(trendHandle!=INVALID_HANDLE) IndicatorRelease(trendHandle);
   Comment("");
}
void OnTick()
{
   ManageOpenPosition();
   datetime bt=iTime(_Symbol,InpTimeframe,0);
   if(bt==lastBarTime) return;
   lastBarTime=bt;
   OnNewBar();
}
void OnNewBar()
{
   if(HasOpenPosition()) return;
   if(InpCooldownBars>0 && BarsSinceLastTrade()<InpCooldownBars) return;
   if(InpMaxConsecLosses>0 && CountConsecutiveLosses()>=InpMaxConsecLosses) return;
   if(InpMaxDailyLoss>0.0 && DailyProfit()<=-InpMaxDailyLoss) return;
   if(InpMaxSpreadPoints>0 && SymbolInfoInteger(_Symbol,SYMBOL_SPREAD)>InpMaxSpreadPoints) return;

   int dir = (InpStrategy==STRAT_EMA_CROSS) ? SignalEmaCross() : SignalRsiPa();
   if(dir>0)      OpenTrade(ORDER_TYPE_BUY);
   else if(dir<0) OpenTrade(ORDER_TYPE_SELL);
}

//--- Strategy 1: two-EMA crossover ---------------------------------
int SignalEmaCross()
{
   double f[],s[];
   if(CopyBuffer(fastHandle,0,1,2,f)<2) return(0); // f[0]=shift2, f[1]=shift1
   if(CopyBuffer(slowHandle,0,1,2,s)<2) return(0);
   bool crossUp   = (f[0]<=s[0] && f[1]>s[1]);
   bool crossDown = (f[0]>=s[0] && f[1]<s[1]);
   if(crossUp)   return(1);
   if(crossDown) return(-1);
   return(0);
}

//--- Strategy 2: RSI extreme + price-action confirmation -----------
int SignalRsiPa()
{
   double r[];
   if(CopyBuffer(rsiHandle,0,1,1,r)<1) return(0);
   double c=iClose(_Symbol,InpTimeframe,1), o=iOpen(_Symbol,InpTimeframe,1);
   bool bull = c>o, bear = c<o;
   int dir=0;
   if(r[0]<=InpRSIOS && bull) dir=1;       // oversold + bullish candle
   else if(r[0]>=InpRSIOB && bear) dir=-1; // overbought + bearish candle
   if(dir==0) return(0);

   // --- "When to trade" regime filters ---
   // 1) Trend alignment: don't buy dips below the slow EMA (falling knife) and
   //    don't sell rallies above it. Take pullbacks WITH the bigger trend.
   if(InpUseTrendAlign)
   {
      double s[]; if(CopyBuffer(trendHandle,0,1,1,s)<1) return(0); // long-term trend EMA
      if(dir>0 && c < s[0]) return(0);  // don't buy dips below the long-term trend
      if(dir<0 && c > s[0]) return(0);  // don't sell rallies above it
   }
   // 2) ADX regime: mean-reversion works in ranges, not strong trends.
   if(InpUseADX)
   {
      double a[]; if(CopyBuffer(adxHandle,0,1,1,a)<1) return(0); // buffer 0 = ADX main line
      if(a[0] > InpADXMax) return(0);
   }
   return(dir);
}

//--- Orders / management / helpers (shared) ------------------------
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
   double lots=InpUseRiskPercent?CalcLotByRisk(slPts):NormalizeLot(InpLots);
   bool ok=(type==ORDER_TYPE_BUY)?trade.Buy(lots,_Symbol,0.0,sl,tp,"Ratrix classic")
                                 :trade.Sell(lots,_Symbol,0.0,sl,tp,"Ratrix classic");
   if(ok) lastTradeBarTime=iTime(_Symbol,InpTimeframe,0);
   else PrintFormat("Order failed: %d %s",trade.ResultRetcode(),trade.ResultRetcodeDescription());
}
double NormalizeLot(double lot)
{
   double step=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_STEP);
   double mn=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MIN), mx=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MAX);
   if(step<=0) step=0.01; lot=MathFloor(lot/step)*step; if(lot<mn) lot=mn; if(lot>mx) lot=mx; return lot;
}
double CalcLotByRisk(double slPoints)
{
   double bal=AccountInfoDouble(ACCOUNT_BALANCE), risk=bal*InpRiskPercent/100.0;
   double tv=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_VALUE), ts=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_SIZE);
   double pt=SymbolInfoDouble(_Symbol,SYMBOL_POINT);
   if(tv<=0||ts<=0) return NormalizeLot(InpLots);
   double lpl=(slPoints*pt/ts)*tv; if(lpl<=0) return NormalizeLot(InpLots);
   return NormalizeLot(risk/lpl);
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
//+------------------------------------------------------------------+
