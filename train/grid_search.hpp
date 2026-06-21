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
// them by a composite score.
// ============================================================================

struct GridSearchResult {
    std::string strategyName;
    ParamSet params;
    
    int    totalTrades = 0;
    int    wins        = 0;
    double winRatePct  = 0.0;
    double netPnl      = 0.0;
    double maxDrawdown = 0.0;
    
    double score       = 0.0;
    
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

class GridSearchEngine {
public:
    struct Config {
        int    durationSec = 15;
        double stake       = 1.0;
        double payoutPct   = 0.95;
        int    minTrades   = 10;
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
        
        for (size_t i = 0; i < n; i++) {
            Signal sig = strat->onPrice(times[i], prices[i]);
            if (sig == Signal::None) continue;
            
            int64_t targetExit = times[i] + cfg_.durationSec;
            size_t j = i + 1;
            while (j < n && times[j] < targetExit) j++;
            if (j >= n) continue;
            
            bool priceUp   = prices[j] > prices[i];
            bool priceDown = prices[j] < prices[i];
            bool won = (sig == Signal::Rise) ? priceUp : priceDown;
            double pnl = won ? (cfg_.stake * cfg_.payoutPct) : -cfg_.stake;
            
            res.totalTrades++;
            if (won) res.wins++;
            
            equity += pnl;
            peak = std::max(peak, equity);
            maxDD = std::min(maxDD, equity - peak);
        }
        
        res.netPnl = equity;
        res.maxDrawdown = maxDD;
        res.winRatePct = res.totalTrades > 0 ? (100.0 * res.wins / res.totalTrades) : 0.0;
        
        // Composite score calculation:
        // Score = P&L * min(1, trades/minTrades) * (1 - penalty for drawdown)
        if (res.totalTrades < cfg_.minTrades || res.netPnl <= 0) {
            res.score = res.netPnl; // keep negative scores as is, but don't boost them
        } else {
            double ddPenalty = std::min(1.0, std::abs(res.maxDrawdown) / std::abs(res.netPnl));
            double tradeFactor = std::min(1.0, (double)res.totalTrades / cfg_.minTrades);
            res.score = res.netPnl * tradeFactor * (1.0 - 0.5 * ddPenalty); 
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
