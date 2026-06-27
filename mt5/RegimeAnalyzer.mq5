//+------------------------------------------------------------------+
//|                                              RegimeAnalyzer.mq5   |
//|   Diagnostic: auto-buckets ALL available D1 history into calendar |
//|   quarters and reports regime metrics per quarter so we can test  |
//|   (with many quarters) whether trend-strength predicts when the   |
//|   RSI mean-reversion strategy works.                              |
//|     Efficiency Ratio (0=chop,1=trend), avg ADX, net move, range.  |
//|   Writes Common/Files/regime.csv and prints the data date range.  |
//+------------------------------------------------------------------+
#property copyright "Ratrix"
#property version   "1.10"
#property strict

int adxHandle;

int OnInit()
{
   adxHandle = iADX(_Symbol, PERIOD_D1, 14);
   if(adxHandle==INVALID_HANDLE) { Print("ADX handle failed"); return(INIT_FAILED); }
   return(INIT_SUCCEEDED);
}

void OnDeinit(const int reason)
{
   MqlRates r[]; ArraySetAsSeries(r,true);
   int n=CopyRates(_Symbol, PERIOD_D1, 0, 3000, r);
   double adx[]; ArraySetAsSeries(adx,true);
   int na=CopyBuffer(adxHandle,0,0,3000,adx);
   if(n<30){ Print("not enough D1 data: n=",n); return; }

   PrintFormat("Data range: %s  ->  %s   (%d D1 bars)",
               TimeToString(r[n-1].time,TIME_DATE), TimeToString(r[0].time,TIME_DATE), n);

   // Parallel arrays keyed by year*10+quarter
   int    key[];   ArrayResize(key,64);
   double first[]; ArrayResize(first,64);
   double last[];  ArrayResize(last,64);
   double path[];  ArrayResize(path,64);
   double rng[];   ArrayResize(rng,64);
   double sadx[];  ArrayResize(sadx,64);
   int    cnt[];   ArrayResize(cnt,64);
   double prevC[]; ArrayResize(prevC,64);
   bool   hasF[];  ArrayResize(hasF,64);
   bool   hasP[];  ArrayResize(hasP,64);
   int    nq=0;

   for(int i=n-1;i>=0;i--) // oldest -> newest
   {
      MqlDateTime dt; TimeToStruct(r[i].time,dt);
      int qk = dt.year*10 + ((dt.mon-1)/3 + 1);
      int idx=-1;
      for(int j=0;j<nq;j++) if(key[j]==qk){ idx=j; break; }
      if(idx<0){ idx=nq; key[idx]=qk; first[idx]=0;last[idx]=0;path[idx]=0;rng[idx]=0;sadx[idx]=0;cnt[idx]=0;hasF[idx]=false;hasP[idx]=false; nq++; }
      if(!hasF[idx]){ first[idx]=r[i].close; hasF[idx]=true; }
      last[idx]=r[i].close;
      if(hasP[idx]) path[idx]+=MathAbs(r[i].close-prevC[idx]);
      prevC[idx]=r[i].close; hasP[idx]=true;
      rng[idx]+=(r[i].high-r[i].low);
      if(i<na) sadx[idx]+=adx[i];
      cnt[idx]++;
   }

   int fh=FileOpen("regime.csv",FILE_WRITE|FILE_CSV|FILE_ANSI|FILE_COMMON,',');
   if(fh!=INVALID_HANDLE) FileWrite(fh,"quarter","days","net_$","dir","efficiency_ratio","avg_ADX","avg_range_$");
   Print("===== REGIME PER QUARTER (XAUUSD D1) =====");
   for(int j=0;j<nq;j++)
   {
      if(cnt[j]<5) continue;
      double net=last[j]-first[j];
      double er=(path[j]>0)?MathAbs(net)/path[j]:0;
      double aadx=sadx[j]/cnt[j];
      double arng=rng[j]/cnt[j];
      string dir=(net>0?"UP":(net<0?"DOWN":"flat"));
      int yr=key[j]/10, qq=key[j]%10;
      string qn=StringFormat("%d-Q%d",yr,qq);
      PrintFormat("%s | days=%d net=$%.0f %s ER=%.2f ADX=%.1f range=$%.1f", qn,cnt[j],net,dir,er,aadx,arng);
      if(fh!=INVALID_HANDLE) FileWrite(fh,qn,cnt[j],DoubleToString(net,0),dir,DoubleToString(er,3),DoubleToString(aadx,1),DoubleToString(arng,1));
   }
   if(fh!=INVALID_HANDLE) FileClose(fh);
   if(adxHandle!=INVALID_HANDLE) IndicatorRelease(adxHandle);
}

void OnTick() { }
//+------------------------------------------------------------------+
