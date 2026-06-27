//+------------------------------------------------------------------+
//|                                       StructureScalper_Ratrix.mq5 |
//|   Gold scalper based on market STRUCTURE + SUPPORT/RESISTANCE     |
//|   (from candles.md), not lagging indicators.                     |
//|                                                                  |
//|     * Swings: fractal swing highs (resistance) & lows (support). |
//|     * Structure: uptrend = higher highs + higher lows;           |
//|                  downtrend = lower highs + lower lows.            |
//|     * BREAKOUT mode: enter when a candle CLOSES beyond the level  |
//|                  (confirmation filter), in the trend direction.   |
//|     * BOUNCE mode  : enter on a rejection candle at support/      |
//|                  resistance, with the trend.                      |
//|                                                                  |
//|   Hard SL/TP, % risk sizing, trailing, discipline. DEMO ONLY.    |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.00"
#property strict

#include <Trade/Trade.mqh>

enum ENUM_SS_MODE { MODE_BREAKOUT=0, MODE_BOUNCE=1 };

input group "Strategy"
input ENUM_SS_MODE    InpMode        = MODE_BREAKOUT; // BREAKOUT or BOUNCE
input ENUM_TIMEFRAMES InpTimeframe   = PERIOD_M1;     // Working timeframe (M1 = the +$6K config)
input int    InpFractalN             = 3;             // Bars each side defining a swing
input bool   InpUseTrendFilter       = true;          // Require HH/HL or LH/LL alignment
input int    InpBreakBufferPoints    = 30;            // Breakout: close this far beyond level
input int    InpBounceTolPoints      = 60;            // Bounce: how close to the level counts as a touch

input group "Risk"
input bool   InpUseRiskPercent       = true;
input double InpRiskPercent          = 1.0;
input double InpLots                 = 0.01;
input int    InpStopLossPoints       = 200;
input int    InpTakeProfitPoints     = 300;
input bool   InpUseTrailing          = true;
input int    InpBreakEvenPoints      = 120;
input int    InpTrailPoints          = 120;

input group "Discipline"
input int    InpMaxSpreadPoints      = 200;
input int    InpCooldownBars         = 1;
input int    InpMaxConsecLosses      = 4;
input double InpMaxDailyLoss         = 0.0;

input group "Execution"
input ulong  InpMagic                = 27062028;
input ulong  InpSlippagePoints       = 20;

CTrade   trade;
datetime lastBarTime=0, lastTradeBarTime=0;
double   g_lastSH=0, g_prevSH=0, g_lastSL=0, g_prevSL=0;
int      g_trend=0; // +1 up, -1 down, 0 range

//+------------------------------------------------------------------+
int OnInit()
{
   trade.SetExpertMagicNumber(InpMagic);
   trade.SetDeviationInPoints(InpSlippagePoints);
   trade.SetTypeFillingBySymbol(_Symbol);
   PrintFormat("StructureScalper on %s %s | mode=%s (DEMO ONLY)",
               _Symbol, EnumToString(InpTimeframe),
               (InpMode==MODE_BREAKOUT?"BREAKOUT":"BOUNCE"));
   return(INIT_SUCCEEDED);
}
void OnDeinit(const int reason){ Comment(""); }

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
//| Find the two most recent swing highs and lows; set trend.        |
//+------------------------------------------------------------------+
bool UpdateStructure()
{
   int N=InpFractalN;
   int count=250;
   MqlRates r[];
   ArraySetAsSeries(r,true);
   if(CopyRates(_Symbol,InpTimeframe,0,count,r)<2*N+5) return(false);

   double sh[2]; double sl[2]; int nSH=0,nSL=0;
   for(int i=N+1; i<=count-N-1 && (nSH<2 || nSL<2); i++)
   {
      bool isHigh=true, isLow=true;
      for(int j=1;j<=N;j++)
      {
         if(r[i].high <= r[i-j].high || r[i].high <= r[i+j].high) isHigh=false;
         if(r[i].low  >= r[i-j].low  || r[i].low  >= r[i+j].low ) isLow=false;
      }
      if(isHigh && nSH<2){ sh[nSH++]=r[i].high; }
      if(isLow  && nSL<2){ sl[nSL++]=r[i].low;  }
   }
   if(nSH<2 || nSL<2) return(false);

   g_lastSH=sh[0]; g_prevSH=sh[1];
   g_lastSL=sl[0]; g_prevSL=sl[1];

   bool hh=g_lastSH>g_prevSH, hl=g_lastSL>g_prevSL;
   bool lh=g_lastSH<g_prevSH, ll=g_lastSL<g_prevSL;
   if(hh && hl)      g_trend=1;
   else if(lh && ll) g_trend=-1;
   else              g_trend=0;
   return(true);
}

//+------------------------------------------------------------------+
void OnNewBar()
{
   if(HasOpenPosition()) return;
   if(InpCooldownBars>0 && BarsSinceLastTrade()<InpCooldownBars) return;
   if(InpMaxConsecLosses>0 && CountConsecutiveLosses()>=InpMaxConsecLosses) return;
   if(InpMaxDailyLoss>0.0 && DailyProfit()<=-InpMaxDailyLoss) return;
   if(InpMaxSpreadPoints>0 && SymbolInfoInteger(_Symbol,SYMBOL_SPREAD)>InpMaxSpreadPoints) return;

   if(!UpdateStructure()) return;

   // Last closed bar (shift 1)
   double c=iClose(_Symbol,InpTimeframe,1), o=iOpen(_Symbol,InpTimeframe,1);
   double hi=iHigh(_Symbol,InpTimeframe,1), lo=iLow(_Symbol,InpTimeframe,1);
   double pt=_Point;
   double buf=InpBreakBufferPoints*pt, tol=InpBounceTolPoints*pt;

   int dir=0;
   if(InpMode==MODE_BREAKOUT)
   {
      bool buyBreak  = (c > g_lastSH + buf);
      bool sellBreak = (c < g_lastSL - buf);
      if(InpUseTrendFilter){ if(g_trend<0) buyBreak=false; if(g_trend>0) sellBreak=false; }
      if(buyBreak)  dir=1; else if(sellBreak) dir=-1;
   }
   else // BOUNCE
   {
      bool buyBounce  = (lo <= g_lastSL + tol && c > o);  // touched support, bullish reject
      bool sellBounce = (hi >= g_lastSH - tol && c < o);  // touched resistance, bearish reject
      if(InpUseTrendFilter){ if(g_trend<0) buyBounce=false; if(g_trend>0) sellBounce=false; }
      if(buyBounce)  dir=1; else if(sellBounce) dir=-1;
   }

   if(dir>0)      OpenTrade(ORDER_TYPE_BUY);
   else if(dir<0) OpenTrade(ORDER_TYPE_SELL);
}

//+------------------------------------------------------------------+
//| Orders + management                                              |
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
   double lots=InpUseRiskPercent?CalcLotByRisk(slPts):NormalizeLot(InpLots);
   bool ok=(type==ORDER_TYPE_BUY)
           ? trade.Buy(lots,_Symbol,0.0,sl,tp,"Ratrix struct")
           : trade.Sell(lots,_Symbol,0.0,sl,tp,"Ratrix struct");
   if(ok){ lastTradeBarTime=iTime(_Symbol,InpTimeframe,0);
           PrintFormat("OPEN %s %.2f lots sl=%.2f tp=%.2f trend=%d",(type==ORDER_TYPE_BUY?"BUY":"SELL"),lots,sl,tp,g_trend); }
   else PrintFormat("Order failed: %d %s",trade.ResultRetcode(),trade.ResultRetcodeDescription());
}
double NormalizeLot(double lot)
{
   double step=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_STEP);
   double mn=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MIN), mx=SymbolInfoDouble(_Symbol,SYMBOL_VOLUME_MAX);
   if(step<=0) step=0.01;
   lot=MathFloor(lot/step)*step; if(lot<mn) lot=mn; if(lot>mx) lot=mx; return lot;
}
double CalcLotByRisk(double slPoints)
{
   double bal=AccountInfoDouble(ACCOUNT_BALANCE), risk=bal*InpRiskPercent/100.0;
   double tv=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_VALUE), ts=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_SIZE);
   double pt=SymbolInfoDouble(_Symbol,SYMBOL_POINT);
   if(tv<=0||ts<=0) return NormalizeLot(InpLots);
   double lossPerLot=(slPoints*pt/ts)*tv; if(lossPerLot<=0) return NormalizeLot(InpLots);
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
   string tr=(g_trend>0?"UP (HH/HL)":(g_trend<0?"DOWN (LH/LL)":"range"));
   string txt=StringFormat("StructureScalper [DEMO]\nSymbol: %s  TF: %s  mode: %s\nTrend: %s\nResistance: %.2f  Support: %.2f\nPosition: %s",
      _Symbol, EnumToString(InpTimeframe), (InpMode==MODE_BREAKOUT?"BREAKOUT":"BOUNCE"),
      tr, g_lastSH, g_lastSL, (HasOpenPosition()?"IN TRADE":"flat"));
   Comment(txt);
}
//+------------------------------------------------------------------+
