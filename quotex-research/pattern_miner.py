"""
Pattern miner for Quotex OTC candles (next-candle binary direction).

Tests a battery of recurring setups and validates each across K independent
time folds. Reports only patterns that are (a) frequent enough, (b) consistent
in direction across ALL folds, and (c) clear the break-even win rate with margin.

Break-even at payout P: p* = 1/(1+P).  At 92% payout -> 0.521.  We require a
tradeable margin above that.

Usage:  python pattern_miner.py <csv> [payout=0.92] [folds=4]
"""
import sys, csv, math

CSV = sys.argv[1] if len(sys.argv) > 1 else r"d:/More_Projects/Projects/Demos/CPP/derivbot/quotex-data/SOLUSD_otc_60s.csv"
PAYOUT = float(sys.argv[2]) if len(sys.argv) > 2 else 0.92
FOLDS = int(sys.argv[3]) if len(sys.argv) > 3 else 4
BREAKEVEN = 1.0 / (1.0 + PAYOUT)
MARGIN = 0.025            # require winrate >= breakeven + margin
MIN_N_PER_FOLD = 25       # need enough samples in each fold

def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f); next(r, None)
        for x in r:
            try: rows.append((int(x[0]), float(x[1]), float(x[2]), float(x[3]), float(x[4])))
            except Exception: pass
    rows.sort(key=lambda z: z[0])
    return rows

def rsi(closes, n=14):
    out = [None]*len(closes)
    if len(closes) <= n: return out
    gains=losses=0.0
    for i in range(1, n+1):
        d = closes[i]-closes[i-1]
        gains += max(d,0); losses += max(-d,0)
    ag=gains/n; al=losses/n
    out[n] = 100 - 100/(1+(ag/al if al else 999))
    for i in range(n+1, len(closes)):
        d = closes[i]-closes[i-1]
        ag = (ag*(n-1)+max(d,0))/n
        al = (al*(n-1)+max(-d,0))/n
        out[i] = 100 - 100/(1+(ag/al if al else 999))
    return out

def sma(closes, n):
    out=[None]*len(closes); s=0.0
    for i,c in enumerate(closes):
        s+=c
        if i>=n: s-=closes[i-n]
        if i>=n-1: out[i]=s/n
    return out

def main():
    rows = load(CSV)
    n = len(rows)
    closes = [r[4] for r in rows]; opens=[r[1] for r in rows]
    highs=[r[2] for r in rows]; lows=[r[3] for r in rows]
    R = rsi(closes,14); S20 = sma(closes,20)
    direction = [0]*n
    for i in range(1,n):
        direction[i] = 1 if closes[i]>closes[i-1] else (-1 if closes[i]<closes[i-1] else 0)

    print(f"Data: {n} candles, {(rows[-1][0]-rows[0][0])/86400:.1f} days | payout {PAYOUT:.0%} -> breakeven {BREAKEVEN:.3f}, need >= {BREAKEVEN+MARGIN:.3f}")
    print(f"Baseline P(up) over all: {sum(1 for d in direction if d>0)/sum(1 for d in direction if d!=0):.4f}\n")

    # ---- candidate signal generators: each maps index i -> label string (or None) ----
    def streak_len(i):
        if i<5: return 0
        k=0; s=direction[i]
        if s==0: return 0
        j=i
        while j>0 and direction[j]==s: k+=1; j-=1
        return k*s   # signed streak length

    signals = {}   # name -> list of (fold-independent) (i, predicted_up_bool... we store outcome separately)
    def add(name, i):
        signals.setdefault(name, []).append(i)

    for i in range(25, n-1):
        # streaks
        sl = streak_len(i)
        if abs(sl)>=2: add(f"streak{sl:+d}", i)
        # rsi bands
        if R[i] is not None:
            if R[i]<25: add("rsi<25", i)
            elif R[i]<30: add("rsi<30", i)
            elif R[i]>75: add("rsi>75", i)
            elif R[i]>70: add("rsi>70", i)
        # hour of day (UTC)
        hr = (rows[i][0]//3600)%24
        add(f"hour={hr:02d}", i)
        # candle patterns
        o,c,h,l = opens[i],closes[i],highs[i],lows[i]
        po,pc = opens[i-1],closes[i-1]
        rng = h-l if h>l else 1e-9
        body = abs(c-o)
        upwick = h-max(o,c); dnwick = min(o,c)-l
        if c>o and pc<po and c>=po and o<=pc: add("bull_engulf", i)
        if c<o and pc>po and c<=po and o>=pc: add("bear_engulf", i)
        if body/rng<0.5 and dnwick>2*body and c>o: add("hammer", i)
        if body/rng<0.5 and upwick>2*body and c<o: add("shooting_star", i)
        if body/rng<0.12: add("doji", i)
        # distance from sma20 (mean reversion)
        if S20[i]:
            dev=(c-S20[i])/S20[i]
            if dev>0.012: add("far_above_sma", i)
            elif dev<-0.012: add("far_below_sma", i)

    # ---- evaluate each signal across folds ----
    fold_bounds=[int(n*k/FOLDS) for k in range(FOLDS+1)]
    def fold_of(i):
        for k in range(FOLDS):
            if fold_bounds[k]<=i<fold_bounds[k+1]: return k
        return FOLDS-1

    def outcome_up(i):  # did NEXT candle go up?
        return direction[i+1]>0

    results=[]
    for name, idxs in signals.items():
        # per-fold up-rate (ignoring flat next)
        fold_stats=[]
        tot_up=tot=0
        for k in range(FOLDS):
            up=t=0
            for i in idxs:
                if fold_of(i)!=k: continue
                if direction[i+1]==0: continue
                t+=1; up+= 1 if outcome_up(i) else 0
            fold_stats.append((up,t)); tot_up+=up; tot+=t
        if tot<60: continue
        p_up = tot_up/tot
        edge_side = "CALL" if p_up>=0.5 else "PUT"
        # winrate of betting the dominant side each fold
        fold_wr=[]
        ok_folds=0; enough=0
        for up,t in fold_stats:
            if t<MIN_N_PER_FOLD:
                fold_wr.append(None); continue
            enough+=1
            wr = (up/t) if edge_side=="CALL" else (1-up/t)
            fold_wr.append(wr)
            if wr>=BREAKEVEN+MARGIN: ok_folds+=1
        overall_wr = p_up if edge_side=="CALL" else 1-p_up
        # binomial two-sided p-value vs 0.5
        mu=tot*0.5; sd=math.sqrt(tot*0.25)
        z=(tot_up-mu)/sd if sd else 0
        results.append(dict(name=name, n=tot, wr=overall_wr, side=edge_side,
                            z=abs(z), folds=fold_wr, enough=enough, ok=ok_folds))

    results.sort(key=lambda r:r['wr'], reverse=True)
    print(f"{'PATTERN':16}{'side':>5}{'N':>7}{'winrate':>9}{'|z|':>6}  per-fold winrate (need all >= %.3f)"%(BREAKEVEN+MARGIN))
    print("-"*92)
    survivors=[]
    for r in results:
        fw=" ".join(f"{w:.2f}" if w is not None else " .  " for w in r['folds'])
        robust = (r['enough']==FOLDS and r['ok']==FOLDS and r['z']>2.0)
        flag = "  <== ROBUST" if robust else ""
        if robust: survivors.append(r)
        # only print interesting rows to keep it readable
        if r['wr']>=0.52 or r['z']>2.0 or robust:
            print(f"{r['name']:16}{r['side']:>5}{r['n']:>7}{r['wr']:>9.4f}{r['z']:>6.2f}  {fw}{flag}")

    print("\n=== VERDICT ===")
    if survivors:
        print(f"{len(survivors)} pattern(s) clear breakeven+margin in ALL folds with |z|>2:")
        for r in survivors:
            print(f"  {r['name']} ({r['side']}): {r['wr']:.3f} winrate, n={r['n']}")
    else:
        print("No pattern clears the breakeven+margin bar consistently across all folds.")
        print("(Patterns that look good in one fold but fail others are noise / overfitting.)")

main()
