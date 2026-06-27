//+------------------------------------------------------------------+
//|                                              GoldScalper_ML.mq5   |
//|   Gold scalping EA with a self-training gradient-boosted-trees    |
//|   (XGBoost-style) model over candle-aware features.              |
//|                                                                  |
//|   On init it builds a training set from history (candle shape +  |
//|   EMA/RSI context), trains a gradient-boosting classifier on     |
//|   "did price rise over the next K bars", then trades when the    |
//|   model is confident -- with SL/TP, risk sizing, trailing and    |
//|   the same discipline guards as GoldScalper_Ratrix.             |
//|                                                                  |
//|   DEMO ONLY. Validate out-of-sample before trusting it.          |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.00"
#property strict

#include <Trade/Trade.mqh>

#define NFEAT 12   // number of candle-aware features

//--- Strategy / ML inputs ------------------------------------------
input group "Model"
input ENUM_TIMEFRAMES InpTimeframe = PERIOD_M5;   // Working timeframe (M5 scalping)
input int    InpRSIPeriod   = 14;     // RSI period
input int    InpFastMA      = 21;     // Fast EMA
input int    InpSlowMA      = 50;     // Slow EMA
input int    InpLabelAhead  = 3;      // Label: price up after this many bars
input int    InpTrainBars   = 4000;   // History bars used to train the model
input int    InpRounds      = 40;     // Boosting rounds (trees)
input int    InpMaxDepth    = 3;      // Tree depth
input double InpLearnRate   = 0.10;   // Learning rate
input double InpProbMargin  = 0.10;   // SELECTIVITY: trade only if |prob-0.5| >= this (raise it to trade rarely, only best patterns)
input bool   InpRequireCandle = true; // Also require a confluent candle in the trade direction
input bool   InpVolFilter   = true;   // Skip entries during volatility/news spikes
input double InpMaxATRmult  = 1.8;    // News proxy: skip if ATR > this x its recent average

//--- Risk ----------------------------------------------------------
input group "Risk"
input bool   InpUseRiskPercent = true;   // Size lot by % risk
input double InpRiskPercent    = 1.0;    // Risk per trade (% of balance)
input double InpLots           = 0.01;   // Fixed lot (if risk sizing off)
input int    InpStopLossPoints = 400;    // Stop loss (points) -- wider for M5
input int    InpTakeProfitPoints = 600;  // Take profit (points)
input bool   InpUseTrailing    = true;   // Trail stop after break-even
input int    InpBreakEvenPoints = 250;   // Start trailing after this profit (points)
input int    InpTrailPoints     = 250;   // Trailing distance (points)

//--- Discipline ----------------------------------------------------
input group "Discipline"
input int    InpMaxSpreadPoints = 200;   // Skip if spread above (points; 0=off)
input int    InpCooldownBars    = 1;     // Min bars between trades
input int    InpMaxConsecLosses = 4;     // Pause after N losses in a row (0=off)
input double InpMaxDailyLoss     = 0.0;  // Stop after $ loss for the day (0=off)

//--- Execution -----------------------------------------------------
input group "Execution"
input ulong  InpMagic   = 26062027;
input ulong  InpSlippagePoints = 20;

//+------------------------------------------------------------------+
//| Gradient-boosted regression tree (one boosting round)            |
//+------------------------------------------------------------------+
struct GBNode
{
   bool   isLeaf;
   double leaf;
   int    feature;
   double threshold;
   int    left;
   int    right;
};

class CGBTree
{
public:
   GBNode m_nodes[];
   int    m_count;

   CGBTree() { m_count = 0; ArrayResize(m_nodes, 64); }

   int NewNode()
   {
      if(m_count >= ArraySize(m_nodes)) ArrayResize(m_nodes, ArraySize(m_nodes) * 2);
      int id = m_count++;
      m_nodes[id].isLeaf = false; m_nodes[id].leaf = 0.0;
      m_nodes[id].feature = -1; m_nodes[id].threshold = 0.0;
      m_nodes[id].left = -1; m_nodes[id].right = -1;
      return id;
   }

   double Predict(const double &x[])
   {
      if(m_count == 0) return 0.0;
      int id = 0;
      while(!m_nodes[id].isLeaf)
      {
         if(x[m_nodes[id].feature] <= m_nodes[id].threshold) id = m_nodes[id].left;
         else                                                id = m_nodes[id].right;
         if(id < 0) return 0.0;
      }
      return m_nodes[id].leaf;
   }
};

//--- Training data (module globals) --------------------------------
double   g_X[];          // flattened [sample*NFEAT + f]
double   g_y[];          // labels 0/1
double   g_raw[];        // running raw score per sample
double   g_grad[];       // gradient
double   g_hess[];       // hessian
int      g_nSamples = 0;
double   g_baseScore = 0.0;

CGBTree *g_trees[];      // the boosted ensemble
int      g_nTrees = 0;
bool     g_ready  = false;

//--- Indicator handles / state -------------------------------------
CTrade   trade;
int      rsiHandle, fastHandle, slowHandle, atrHandle;
datetime lastBarTime = 0, lastTradeBarTime = 0;
double   g_lastProb = 0.5;

//+------------------------------------------------------------------+
int OnInit()
{
   trade.SetExpertMagicNumber(InpMagic);
   trade.SetDeviationInPoints(InpSlippagePoints);
   trade.SetTypeFillingBySymbol(_Symbol);

   rsiHandle  = iRSI(_Symbol, InpTimeframe, InpRSIPeriod, PRICE_CLOSE);
   fastHandle = iMA(_Symbol, InpTimeframe, InpFastMA, 0, MODE_EMA, PRICE_CLOSE);
   slowHandle = iMA(_Symbol, InpTimeframe, InpSlowMA, 0, MODE_EMA, PRICE_CLOSE);
   atrHandle  = iATR(_Symbol, InpTimeframe, 14);
   if(rsiHandle==INVALID_HANDLE || fastHandle==INVALID_HANDLE ||
      slowHandle==INVALID_HANDLE || atrHandle==INVALID_HANDLE)
   { Print("indicator handle failed"); return(INIT_FAILED); }

   PrintFormat("GoldScalper_ML on %s %s. Training model... (DEMO ONLY)",
               _Symbol, EnumToString(InpTimeframe));
   return(INIT_SUCCEEDED);
}

void OnDeinit(const int reason)
{
   for(int i=0;i<g_nTrees;i++) if(CheckPointer(g_trees[i])==POINTER_DYNAMIC) delete g_trees[i];
   ArrayResize(g_trees,0); g_nTrees=0;
   if(rsiHandle!=INVALID_HANDLE) IndicatorRelease(rsiHandle);
   if(fastHandle!=INVALID_HANDLE) IndicatorRelease(fastHandle);
   if(slowHandle!=INVALID_HANDLE) IndicatorRelease(slowHandle);
   if(atrHandle!=INVALID_HANDLE) IndicatorRelease(atrHandle);
   Comment("");
}

//+------------------------------------------------------------------+
//| Build the feature vector for the bar at `shift`.                 |
//| Needs indicator buffers; returns false if data not ready.        |
//+------------------------------------------------------------------+
bool BuildFeatures(int shift, double &x[])
{
   MqlRates r[];
   if(CopyRates(_Symbol, InpTimeframe, shift, 2, r) < 2) return(false); // r[0]=older,r[1]=this
   double rsi[], fast[], slow[], atr[];
   if(CopyBuffer(rsiHandle,  0, shift, 2, rsi)  < 2) return(false);
   if(CopyBuffer(fastHandle, 0, shift, 2, fast) < 2) return(false);
   if(CopyBuffer(slowHandle, 0, shift, 1, slow) < 1) return(false);
   if(CopyBuffer(atrHandle,  0, shift, 1, atr)  < 1) return(false);

   MqlRates prev = r[0], cur = r[1];
   double rng = MathMax(cur.high - cur.low, _Point);
   double body = MathAbs(cur.close - cur.open);
   double lw = MathMin(cur.open,cur.close) - cur.low;
   double uw = cur.high - MathMax(cur.open,cur.close);
   bool isGreen = cur.close > cur.open;

   bool bullEng = (prev.close<prev.open && isGreen && cur.open<=prev.close && cur.close>=prev.open);
   bool bearEng = (prev.close>prev.open && !isGreen && cur.open>=prev.close && cur.close<=prev.open);
   bool hammer  = (lw>=0.5*rng && uw<=0.15*rng && body<=0.4*rng);
   bool star    = (uw>=0.5*rng && lw<=0.15*rng && body<=0.4*rng);

   double atrv = MathMax(atr[0], _Point);

   ArrayResize(x, NFEAT);
   x[0]  = body/rng;                       // body ratio
   x[1]  = uw/rng;                         // upper wick
   x[2]  = lw/rng;                         // lower wick
   x[3]  = isGreen ? 1.0 : 0.0;            // colour
   x[4]  = (bullEng?1.0:0.0)-(bearEng?1.0:0.0); // engulfing signed
   x[5]  = (hammer?1.0:0.0)-(star?1.0:0.0);     // hammer/star signed
   x[6]  = rsi[1];                         // RSI
   x[7]  = rsi[1]-rsi[0];                  // RSI slope
   x[8]  = (fast[1]-slow[0])/atrv;         // trend (EMA gap in ATRs)
   x[9]  = (fast[1]-fast[0])/atrv;         // EMA momentum
   x[10] = (cur.close-fast[1])/atrv;       // price vs fast EMA
   x[11] = rng/atrv;                       // range vs ATR (volatility)
   return(true);
}

//+------------------------------------------------------------------+
//| Train the gradient-boosting model from history.                 |
//+------------------------------------------------------------------+
double Sigmoid(double z){ return 1.0/(1.0+MathExp(-z)); }

void TrainModel()
{
   int bars = Bars(_Symbol, InpTimeframe);
   int usable = MathMin(InpTrainBars, bars - InpLabelAhead - InpSlowMA - 5);
   if(usable < 300) { Print("not enough history to train"); return; }

   ArrayResize(g_X, usable*NFEAT);
   ArrayResize(g_y, usable);
   g_nSamples = 0;

   // Build samples from shift (usable+InpLabelAhead) ... down to InpLabelAhead+1
   for(int s = usable + InpLabelAhead; s > InpLabelAhead; s--)
   {
      double x[];
      if(!BuildFeatures(s, x)) continue;
      double cClose = iClose(_Symbol, InpTimeframe, s);
      double fClose = iClose(_Symbol, InpTimeframe, s - InpLabelAhead);
      if(cClose==0.0 || fClose==0.0) continue;
      int idx = g_nSamples;
      for(int f=0; f<NFEAT; f++) g_X[idx*NFEAT+f] = x[f];
      g_y[idx] = (fClose > cClose) ? 1.0 : 0.0;
      g_nSamples++;
   }
   if(g_nSamples < 200) { Print("too few samples: ", g_nSamples); return; }

   // base score = log-odds of positive class
   double pos=0; for(int i=0;i<g_nSamples;i++) pos+=g_y[i];
   double rate=MathMax(0.001,MathMin(0.999,pos/g_nSamples));
   g_baseScore=MathLog(rate/(1.0-rate));

   ArrayResize(g_raw,g_nSamples); ArrayResize(g_grad,g_nSamples); ArrayResize(g_hess,g_nSamples);
   for(int i=0;i<g_nSamples;i++) g_raw[i]=g_baseScore;

   ArrayResize(g_trees, InpRounds); g_nTrees=0;
   for(int rnd=0; rnd<InpRounds; rnd++)
   {
      for(int i=0;i<g_nSamples;i++)
      {
         double p=Sigmoid(g_raw[i]);
         g_grad[i]=p-g_y[i];
         g_hess[i]=MathMax(1e-6,p*(1.0-p));
      }
      CGBTree *tree=new CGBTree();
      int idx[]; ArrayResize(idx,g_nSamples);
      for(int i=0;i<g_nSamples;i++) idx[i]=i;
      int root=tree.NewNode();
      BuildNode(tree, root, idx, g_nSamples, 0);
      // update raw scores
      for(int i=0;i<g_nSamples;i++)
      {
         double xrow[]; ArrayResize(xrow,NFEAT);
         for(int f=0;f<NFEAT;f++) xrow[f]=g_X[i*NFEAT+f];
         g_raw[i]+=InpLearnRate*tree.Predict(xrow);
      }
      g_trees[g_nTrees++]=tree;
   }
   g_ready=true;
   PrintFormat("Model trained: %d samples, %d trees, pos-rate=%.3f", g_nSamples, g_nTrees, rate);
}

//--- Recursive node builder (XGBoost gain + leaf weight) -----------
void BuildNode(CGBTree *tree, int nodeId, int &idx[], int n, int depth)
{
   const double lambda=1.0, gamma=0.0, minChild=1.0;
   double G=0,H=0;
   for(int i=0;i<n;i++){ G+=g_grad[idx[i]]; H+=g_hess[idx[i]]; }

   if(depth>=InpMaxDepth || n<2)
   { tree.m_nodes[nodeId].isLeaf=true; tree.m_nodes[nodeId].leaf=-G/(H+lambda); return; }

   double bestGain=0; int bestF=-1; double bestThr=0;
   double rootScore=(G*G)/(H+lambda);

   for(int f=0; f<NFEAT; f++)
   {
      // sort idx by feature f (simple insertion on a copy)
      int ord[]; ArrayResize(ord,n); for(int i=0;i<n;i++) ord[i]=idx[i];
      SortByFeature(ord, n, f);
      double GL=0,HL=0;
      for(int s=0;s<n-1;s++)
      {
         GL+=g_grad[ord[s]]; HL+=g_hess[ord[s]];
         double v1=g_X[ord[s]*NFEAT+f], v2=g_X[ord[s+1]*NFEAT+f];
         if(v1==v2) continue;
         double GR=G-GL, HR=H-HL;
         if(HL<minChild || HR<minChild) continue;
         double gain=0.5*((GL*GL)/(HL+lambda)+(GR*GR)/(HR+lambda)-rootScore)-gamma;
         if(gain>bestGain){ bestGain=gain; bestF=f; bestThr=(v1+v2)/2.0; }
      }
   }

   if(bestF<0 || bestGain<=0.0)
   { tree.m_nodes[nodeId].isLeaf=true; tree.m_nodes[nodeId].leaf=-G/(H+lambda); return; }

   int leftIdx[], rightIdx[]; int nl=0,nr=0;
   ArrayResize(leftIdx,n); ArrayResize(rightIdx,n);
   for(int i=0;i<n;i++)
   {
      if(g_X[idx[i]*NFEAT+bestF] <= bestThr) leftIdx[nl++]=idx[i];
      else rightIdx[nr++]=idx[i];
   }
   ArrayResize(leftIdx,nl); ArrayResize(rightIdx,nr);

   tree.m_nodes[nodeId].isLeaf=false;
   tree.m_nodes[nodeId].feature=bestF;
   tree.m_nodes[nodeId].threshold=bestThr;
   int l=tree.NewNode(); int rr=tree.NewNode();
   tree.m_nodes[nodeId].left=l; tree.m_nodes[nodeId].right=rr;
   BuildNode(tree, l,  leftIdx,  nl, depth+1);
   BuildNode(tree, rr, rightIdx, nr, depth+1);
}

void SortByFeature(int &arr[], int n, int f)
{
   for(int i=1;i<n;i++)
   {
      int key=arr[i]; double kv=g_X[key*NFEAT+f]; int j=i-1;
      while(j>=0 && g_X[arr[j]*NFEAT+f]>kv){ arr[j+1]=arr[j]; j--; }
      arr[j+1]=key;
   }
}

double PredictProb(const double &x[])
{
   double raw=g_baseScore;
   for(int i=0;i<g_nTrees;i++) raw+=InpLearnRate*g_trees[i].Predict(x);
   return Sigmoid(raw);
}

//+------------------------------------------------------------------+
//| Tick / bar handling                                              |
//+------------------------------------------------------------------+
void OnTick()
{
   if(!g_ready) { TrainModel(); if(!g_ready) return; }

   ManageOpenPosition();
   UpdateStatus();

   datetime bt = iTime(_Symbol, InpTimeframe, 0);
   if(bt == lastBarTime) return;
   lastBarTime = bt;
   OnNewBar();
}

void OnNewBar()
{
   if(HasOpenPosition()) return;
   if(!SpreadOK()) return;
   if(InpCooldownBars>0 && BarsSinceLastTrade()<InpCooldownBars) return;
   if(InpMaxConsecLosses>0 && CountConsecutiveLosses()>=InpMaxConsecLosses) return;
   if(InpMaxDailyLoss>0.0 && DailyProfit()<=-InpMaxDailyLoss) return;
   if(!VolatilityOK()) return;          // skip volatility/news spikes (news proxy)

   double x[];
   if(!BuildFeatures(1, x)) return;     // features of the last closed bar
   double p = PredictProb(x);
   g_lastProb = p;

   bool wantBuy  = (p >= 0.5 + InpProbMargin);
   bool wantSell = (p <= 0.5 - InpProbMargin);
   if(!wantBuy && !wantSell) return;

   if(InpRequireCandle)
   {
      if(wantBuy  && !BullishCandle()) return;
      if(wantSell && !BearishCandle()) return;
   }

   if(wantBuy)  OpenTrade(ORDER_TYPE_BUY);
   else         OpenTrade(ORDER_TYPE_SELL);
}

//+------------------------------------------------------------------+
//| Candle confirmation                                              |
//+------------------------------------------------------------------+
bool BullishCandle()
{
   MqlRates r[]; if(CopyRates(_Symbol,InpTimeframe,1,2,r)<2) return(false);
   MqlRates prev=r[0],cur=r[1];
   double rng=MathMax(cur.high-cur.low,_Point), body=MathAbs(cur.close-cur.open);
   double lw=MathMin(cur.open,cur.close)-cur.low, uw=cur.high-MathMax(cur.open,cur.close);
   bool g=cur.close>cur.open;
   bool ham=(lw>=0.5*rng && uw<=0.15*rng && body<=0.4*rng);
   bool eng=(prev.close<prev.open && g && cur.open<=prev.close && cur.close>=prev.open);
   return(g || ham || eng);
}
bool BearishCandle()
{
   MqlRates r[]; if(CopyRates(_Symbol,InpTimeframe,1,2,r)<2) return(false);
   MqlRates prev=r[0],cur=r[1];
   double rng=MathMax(cur.high-cur.low,_Point), body=MathAbs(cur.close-cur.open);
   double lw=MathMin(cur.open,cur.close)-cur.low, uw=cur.high-MathMax(cur.open,cur.close);
   bool rd=cur.close<cur.open;
   bool star=(uw>=0.5*rng && lw<=0.15*rng && body<=0.4*rng);
   bool eng=(prev.close>prev.open && rd && cur.open>=prev.close && cur.close<=prev.open);
   return(rd || star || eng);
}

//+------------------------------------------------------------------+
//| Order placement + management (same approach as GoldScalper)      |
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
   bool ok = (type==ORDER_TYPE_BUY)
             ? trade.Buy(lots,_Symbol,0.0,sl,tp,"Ratrix ML")
             : trade.Sell(lots,_Symbol,0.0,sl,tp,"Ratrix ML");
   if(ok){ lastTradeBarTime=iTime(_Symbol,InpTimeframe,0);
           PrintFormat("OPEN %s %.2f lots p=%.3f sl=%.2f tp=%.2f",
                       (type==ORDER_TYPE_BUY?"BUY":"SELL"), lots, g_lastProb, sl, tp); }
   else PrintFormat("Order failed: %d %s", trade.ResultRetcode(), trade.ResultRetcodeDescription());
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
   double bal=AccountInfoDouble(ACCOUNT_BALANCE);
   double risk=bal*InpRiskPercent/100.0;
   double tv=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_VALUE);
   double ts=SymbolInfoDouble(_Symbol,SYMBOL_TRADE_TICK_SIZE);
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
   {
      if((bid-open)/pt >= InpBreakEvenPoints)
      { double n=NormalizeDouble(bid-InpTrailPoints*pt,_Digits); if(n>sl && n>=open) trade.PositionModify(_Symbol,n,tp); }
   }
   else if(type==POSITION_TYPE_SELL)
   {
      if((open-ask)/pt >= InpBreakEvenPoints)
      { double n=NormalizeDouble(ask+InpTrailPoints*pt,_Digits); if((n<sl||sl==0.0) && n<=open) trade.PositionModify(_Symbol,n,tp); }
   }
}

//+------------------------------------------------------------------+
//| Helpers                                                          |
//+------------------------------------------------------------------+
bool PositionSelectByMagic()
{
   for(int i=PositionsTotal()-1;i>=0;i--)
   {
      ulong t=PositionGetTicket(i); if(t==0) continue;
      if(PositionGetString(POSITION_SYMBOL)==_Symbol && (ulong)PositionGetInteger(POSITION_MAGIC)==InpMagic) return(true);
   }
   return(false);
}
bool HasOpenPosition(){ return PositionSelectByMagic(); }
bool SpreadOK(){ long s=SymbolInfoInteger(_Symbol,SYMBOL_SPREAD); return(InpMaxSpreadPoints<=0 || s<=InpMaxSpreadPoints); }

// News/volatility proxy: skip when the latest ATR is far above its recent average.
bool VolatilityOK()
{
   if(!InpVolFilter) return(true);
   double atr[];
   if(CopyBuffer(atrHandle,0,1,50,atr)<50) return(true);  // atr[49]=newest, atr[0]=oldest
   double sum=0; for(int i=0;i<50;i++) sum+=atr[i];
   double avg=sum/50.0;
   if(avg<=0.0) return(true);
   return(atr[49] <= InpMaxATRmult*avg);
}
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
   string txt=StringFormat("GoldScalper_ML [DEMO]\nSymbol: %s  TF: %s\nModel: %s (%d trees)\nLast prob: %.3f (margin %.2f)\nPosition: %s\nConsec losses: %d / %d",
      _Symbol, EnumToString(InpTimeframe), (g_ready?"ready":"training"), g_nTrees, g_lastProb, InpProbMargin, pos,
      (InpMaxConsecLosses>0?CountConsecutiveLosses():0), InpMaxConsecLosses);
   Comment(txt);
}
//+------------------------------------------------------------------+
