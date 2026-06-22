#pragma once
#include "../strategies/strategy_registry.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <cmath>

// ============================================================================
// Grid Search Optimization
//
// Evaluates multiple strategy configurations against historical data and ranks
// them by a safety-first composite score that heavily penalizes drawdown and
// consecutive loss streaks.
// ============================================================================

struct GridSearchResult {
    std::string strategyName;
    ParamSet params;
    
    int    totalTrades    = 0;
    int    wins           = 0;
    double winRatePct     = 0.0;
    double netPnl         = 0.0;
    double maxDrawdown    = 0.0;
    int    maxLossStreak  = 0;
    
    double score          = 0.0;
    
    std::string describe() const {
        return strategyName + " (" + paramString() + ")";
    }
    
    std::string paramString() const {
        std::string s;
        bool first = true;
        for (const auto& kv : params) {
            if (!first) s += ", ";
            s += kv.first + "=" + std::to_string(kv.second);
            first = false;
        }
        return s;
    }
};

// ============================================================================
// Auto-detect pip size and $ per pip for forex/gold symbols
// ============================================================================

inline bool isForexOrCommodity(const std::string& symbol) {
    return symbol.rfind("frx", 0) == 0; // starts with "frx"
}

inline double getPipSize(const std::string& symbol) {
    // Gold (XAU) and Silver (XAG) use 0.01 per pip
    if (symbol.find("XAU") != std::string::npos ||
        symbol.find("XAG") != std::string::npos) {
        return 0.01;
    }
    // JPY pairs use 0.01 per pip
    if (symbol.find("JPY") != std::string::npos) {
        return 0.01;
    }
    // Standard forex pairs use 0.0001 per pip
    return 0.0001;
}

class GridSearchEngine {
public:
    struct Config {
        int    durationSec  = 15;
        double stake        = 1.0;
        double payoutPct    = 0.95;
        int    minTrades    = 10;
        double lotSize      = 0.5;     // default 0.5 lot = $5/pip for forex
        std::string symbol;            // needed for forex/commodity detection
    };
    
    GridSearchEngine(const Config& cfg) : cfg_(cfg) {}
    
    // Evaluate a single strategy configuration
    GridSearchResult evaluate(const StrategyGridEntry& entry,
                              const ParamSet& params,
                              const std::vector<int64_t>& times,
                              const std::vector<double>& prices) const {
        GridSearchResult res;
        res.strategyName = entry.name;
        res.params = params;
        
        auto strat = entry.factory(params);
        size_t n = prices.size();
        
        double equity = 0.0, peak = 0.0, maxDD = 0.0;
        int currentStreak = 0, worstStreak = 0;

        bool useTPSL = isForexOrCommodity(cfg_.symbol) && 
                       params.count("tp_pips") && params.count("sl_pips");
        
        double pipSize = getPipSize(cfg_.symbol);
        double dollarPerPip = cfg_.lotSize * 10.0; // 0.5 lot = $5/pip standard
        
        for (size_t i = 0; i < n; i++) {
            Signal sig = strat->onPrice(times[i], prices[i]);
            if (sig == Signal::None) continue;

            double pnl = 0.0;
            bool won = false;

            if (useTPSL) {
                // ---- TP/SL exit for Forex & Gold ----
                double tpPips = params.at("tp_pips");
                double slPips = params.at("sl_pips");
                double tpDist = tpPips * pipSize;
                double slDist = slPips * pipSize;

                bool resolved = false;
                for (size_t j = i + 1; j < n; j++) {
                    double move = prices[j] - prices[i];
                    if (sig == Signal::Fall) move = -move; // invert for sell

                    if (move >= tpDist) {
                        // TP hit
                        won = true;
                        pnl = tpPips * dollarPerPip;
                        resolved = true;
                        break;
                    }
                    if (move <= -slDist) {
                        // SL hit
                        won = false;
                        pnl = -(slPips * dollarPerPip);
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) continue; // trade never resolved, skip
            } else {
                // ---- Duration-based exit for Synthetics ----
                int duration = cfg_.durationSec;
                if (params.count("trade_duration")) {
                    duration = (int)params.at("trade_duration");
                }
                int64_t targetExit = times[i] + duration;
                size_t j = i + 1;
                while (j < n && times[j] < targetExit) j++;
                if (j >= n) continue;
                
                bool priceUp   = prices[j] > prices[i];
                bool priceDown = prices[j] < prices[i];
                won = (sig == Signal::Rise) ? priceUp : priceDown;
                pnl = won ? (cfg_.stake * cfg_.payoutPct) : -cfg_.stake;
            }

            res.totalTrades++;
            if (won) {
                res.wins++;
                currentStreak = 0;
            } else {
                currentStreak++;
                worstStreak = std::max(worstStreak, currentStreak);
            }
            
            equity += pnl;
            peak = std::max(peak, equity);
            maxDD = std::min(maxDD, equity - peak);
        }
        
        res.netPnl = equity;
        res.maxDrawdown = maxDD;
        res.maxLossStreak = worstStreak;
        res.winRatePct = res.totalTrades > 0 ? (100.0 * res.wins / res.totalTrades) : 0.0;
        
        // ================================================================
        // Safety-First Scoring Formula
        //
        // score = calmarRatio × winRateBonus × streakPenalty
        //
        // - calmarRatio  = P&L / |maxDrawdown|  (profit per unit of risk)
        // - winRateBonus  = boost if WR > 55% (breakeven for 95% payout)
        // - streakPenalty = exponentially punish loss streaks > 4
        // ================================================================
        if (res.totalTrades < cfg_.minTrades || res.netPnl <= 0) {
            res.score = res.netPnl;
        } else {
            double absDD = std::abs(res.maxDrawdown);
            double calmar = (absDD > 0.01) ? (res.netPnl / absDD) : res.netPnl;
            
            // Win rate bonus: 1.0 at 55%, scales up linearly
            double wrBonus = std::max(0.5, std::min(2.0, res.winRatePct / 55.0));
            
            // Streak penalty: no penalty up to 4, then exponential decay
            double streakPenalty = 1.0;
            if (res.maxLossStreak > 4) {
                streakPenalty = 1.0 / (1.0 + 0.3 * (res.maxLossStreak - 4));
            }
            
            res.score = calmar * wrBonus * streakPenalty;
        }
        
        return res;
    }
    
    // Run full grid search over all strategies and return top K
    std::vector<GridSearchResult> run(const std::vector<StrategyGridEntry>& grid,
                                      const std::vector<int64_t>& times,
                                      const std::vector<double>& prices,
                                      int topK = 10) const {
        std::vector<GridSearchResult> results;
        int totalCombos = 0;
        for (auto& entry : grid) totalCombos += entry.paramCombinations.size();
        
        std::cout << "  Running grid search over " << totalCombos << " combinations...\n";
        
        int done = 0;
        for (auto& entry : grid) {
            for (auto& params : entry.paramCombinations) {
                results.push_back(evaluate(entry, params, times, prices));
                done++;
                if (done % 50 == 0 || done == totalCombos) {
                    std::cout << "    progress: " << done << "/" << totalCombos << "\r" << std::flush;
                }
            }
        }
        std::cout << "\n";
        
        // Sort descending by score
        std::sort(results.begin(), results.end(), [](const GridSearchResult& a, const GridSearchResult& b) {
            return a.score > b.score;
        });
        
        if (results.size() > (size_t)topK) {
            results.resize(topK);
        }
        
        return results;
    }

private:
    Config cfg_;
};
