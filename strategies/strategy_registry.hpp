#pragma once
#include "strategy_base.hpp"
#include "ema_crossover.hpp"
#include "macd_strategy.hpp"
#include "bollinger_strategy.hpp"
#include "stochastic_strategy.hpp"
#include "multi_confluence.hpp"
#include "price_action_strategy.hpp"
#include "candle_decision_tree.hpp"
#include "../rsi.hpp"
#include <vector>
#include <functional>
#include <memory>
#include <string>

// ============================================================================
// StrategyRegistry
//
// Central catalog of every strategy + its parameter grid for automated
// grid search. Call getAllGridEntries() to get every strategy with every
// parameter combination ready to instantiate and backtest.
// ============================================================================

// Adapter: wraps the existing RsiThresholdStrategy into StrategyBase interface
// so it can participate in the registry alongside the new strategies.
#include <sstream>
class RsiThresholdStrategyAdapter : public StrategyBase {
public:
    RsiThresholdStrategyAdapter(int period, double oversold, double overbought)
        : rsi_(period), period_(period), oversold_(oversold), overbought_(overbought) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        auto value = rsi_.update(price);
        if (!value.has_value()) return Signal::None;
        double r = *value;
        Signal sig = Signal::None;

        if (r <= oversold_ && !osArm_) { sig = Signal::Rise; osArm_ = true; }
        else if (r > oversold_) osArm_ = false;

        if (r >= overbought_ && !obArm_) {
            if (sig == Signal::None) sig = Signal::Fall;
            obArm_ = true;
        } else if (r < overbought_) obArm_ = false;

        lastRsi_ = r;
        return sig;
    }

    std::string name() const override { return "rsi"; }
    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "period=" << period_ << " os=" << oversold_ << " ob=" << overbought_;
        return ss.str();
    }
    double lastRsi() const override { return lastRsi_; }

private:
    RSI    rsi_;
    int    period_;
    double oversold_, overbought_;
    bool   osArm_ = false, obArm_ = false;
    double lastRsi_ = 50.0;
};

// Helper: generate cartesian product of parameter ranges
namespace detail {
    inline std::vector<ParamSet> cartesian(
        const std::vector<std::pair<std::string, std::vector<double>>>& paramRanges)
    {
        std::vector<ParamSet> results;
        results.push_back({}); // start with one empty set

        for (auto& [paramName, values] : paramRanges) {
            std::vector<ParamSet> newResults;
            for (auto& existing : results) {
                for (double v : values) {
                    ParamSet ps = existing;
                    ps[paramName] = v;
                    newResults.push_back(ps);
                }
            }
            results = std::move(newResults);
        }
        return results;
    }
}

class StrategyRegistry {
public:
    static std::vector<StrategyGridEntry> getAllGridEntries(
        const std::vector<std::string>& allowedStrategies = {},
        const std::vector<double>& tradeDurations = {},
        const std::vector<double>& candlePeriods = {}
    ) {
        std::vector<StrategyGridEntry> entries;
        
        auto isAllowed = [&](const std::string& name) {
            if (allowedStrategies.empty()) return true;
            for (const auto& s : allowedStrategies) {
                if (s == name) return true;
            }
            return false;
        };

        if (isAllowed("rsi")) {
            StrategyGridEntry e;
            e.name = "rsi";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<RsiThresholdStrategyAdapter>(
                    (int)p.at("period"), p.at("oversold"), p.at("overbought"));
            };
            e.paramCombinations = detail::cartesian({
                {"period",     {10, 14, 21}},
                {"oversold",   {25, 30, 35}},
                {"overbought", {65, 70, 75}}
            });
            entries.push_back(std::move(e));
        }

        // 2. EMA Crossover
        if (isAllowed("ema_cross")) {
            StrategyGridEntry e;
            e.name = "ema_cross";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<EmaCrossoverStrategy>(
                    (int)p.at("fast"), (int)p.at("slow"));
            };
            e.paramCombinations = detail::cartesian({
                {"fast", {5, 8, 10, 13}},
                {"slow", {20, 30, 50}}
            });
            entries.push_back(std::move(e));
        }

        // 3. MACD
        if (isAllowed("macd")) {
            StrategyGridEntry e;
            e.name = "macd";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<MacdStrategy>(
                    (int)p.at("fast"), (int)p.at("slow"), (int)p.at("signal"));
            };
            e.paramCombinations = detail::cartesian({
                {"fast",   {8, 12}},
                {"slow",   {21, 26}},
                {"signal", {5, 9}}
            });
            entries.push_back(std::move(e));
        }

        // 4. Bollinger
        if (isAllowed("bollinger")) {
            StrategyGridEntry e;
            e.name = "bollinger";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<BollingerStrategy>(
                    (int)p.at("bb_period"), p.at("bb_std"),
                    (int)p.at("rsi_period"), p.at("oversold"), p.at("overbought"));
            };
            e.paramCombinations = detail::cartesian({
                {"bb_period",  {15, 20, 25}},
                {"bb_std",     {1.5, 2.0, 2.5}},
                {"rsi_period", {14}},
                {"oversold",   {30}},
                {"overbought", {70}}
            });
            entries.push_back(std::move(e));
        }

        // 5. Stochastic
        if (isAllowed("stochastic")) {
            StrategyGridEntry e;
            e.name = "stochastic";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<StochasticStrategy>(
                    (int)p.at("k_period"), (int)p.at("d_period"),
                    p.at("oversold"), p.at("overbought"));
            };
            e.paramCombinations = detail::cartesian({
                {"k_period",   {5, 9, 14, 21}},
                {"d_period",   {3, 5}},
                {"oversold",   {20}},
                {"overbought", {80}}
            });
            entries.push_back(std::move(e));
        }

        // 6. Multi-Confluence
        if (isAllowed("multi_confluence")) {
            StrategyGridEntry e;
            e.name = "multi_confluence";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                MultiConfluenceStrategy::Config cfg;
                cfg.enabledMask    = (uint32_t)p.at("mask");
                cfg.minAgreement   = (int)p.at("min_agree");
                cfg.rsiPeriod      = 14;
                cfg.rsiOversold    = 30.0;
                cfg.rsiOverbought  = 70.0;
                cfg.emaFast        = (int)p.at("ema_fast");
                cfg.emaSlow        = (int)p.at("ema_slow");
                cfg.macdFast       = 12;
                cfg.macdSlow       = 26;
                cfg.macdSignal     = 9;
                cfg.bbPeriod       = 20;
                cfg.bbStdDev       = 2.0;
                cfg.stochK         = 14;
                cfg.stochD         = 3;
                cfg.adxFilterEnabled = (p.at("adx_filter") > 0.5);
                cfg.adxThreshold   = 25.0;
                return std::make_unique<MultiConfluenceStrategy>(cfg);
            };

            std::vector<double> masks = {
                0x07,  // RSI + EMA + MACD
                0x09,  // RSI + BB
                0x11,  // RSI + Stoch
                0x0B,  // RSI + EMA + BB
                0x15,  // RSI + MACD + Stoch
                0x19,  // RSI + BB + Stoch
                0x1F,  // All 5
                0x0F,  // RSI + EMA + MACD + BB
                0x1B,  // RSI + EMA + BB + Stoch
            };

            e.paramCombinations = detail::cartesian({
                {"mask",       masks},
                {"min_agree",  {2, 3}},
                {"ema_fast",   {8, 13}},
                {"ema_slow",   {30}},
                {"adx_filter", {0, 1}}
            });
            entries.push_back(std::move(e));
        }

        // 7. Price Action
        if (isAllowed("price_action")) {
            StrategyGridEntry e;
            e.name = "price_action";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                return std::make_unique<PriceActionStrategy>((int)p.at("candle_period"));
            };
            
            std::vector<double> activeCandles = candlePeriods.empty() ? std::vector<double>{5, 10, 15, 30, 60} : candlePeriods;
            std::vector<double> activeDurations = tradeDurations.empty() ? std::vector<double>{15, 30, 60} : tradeDurations;
            
            e.paramCombinations = detail::cartesian({
                {"candle_period", activeCandles},
                {"trade_duration", activeDurations}
            });
            entries.push_back(std::move(e));
        }

        // 8. Candle Decision Tree
        if (isAllowed("candle_tree")) {
            StrategyGridEntry e;
            e.name = "candle_tree";
            e.factory = [](const ParamSet& p) -> std::unique_ptr<StrategyBase> {
                int cp = (int)p.at("candle_period");
                int md = (int)p.at("max_depth");
                int ms = (int)p.at("min_samples");
                int td = p.count("trade_duration") ? (int)p.at("trade_duration") : 15;
                return std::make_unique<CandleDecisionTreeStrategy>(cp, md, ms, td);
            };
            
            std::vector<double> activeCandles = candlePeriods.empty() ? std::vector<double>{5, 10, 15, 30} : candlePeriods;
            std::vector<double> activeDurations = tradeDurations.empty() ? std::vector<double>{15, 30, 60} : tradeDurations;
            
            e.paramCombinations = detail::cartesian({
                {"candle_period", activeCandles},
                {"max_depth", {2, 3, 4}},
                {"min_samples", {5, 10}},
                {"trade_duration", activeDurations}
            });
            entries.push_back(std::move(e));
        }

        return entries;
    }

    // Get grid entries with TP/SL params injected for Forex & Gold
    static std::vector<StrategyGridEntry> getForexGridEntries(
        const std::vector<std::string>& allowedStrategies = {},
        const std::vector<double>& tradeDurations = {},
        const std::vector<double>& candlePeriods = {}
    ) {
        auto entries = getAllGridEntries(allowedStrategies, tradeDurations, candlePeriods);
        
        // For each strategy, multiply its param combos by TP/SL ranges
        std::vector<double> tpValues = {3, 5, 10, 15, 20};
        std::vector<double> slValues = {2, 3, 5, 10};
        
        for (auto& entry : entries) {
            std::vector<ParamSet> expanded;
            for (auto& baseParams : entry.paramCombinations) {
                for (double tp : tpValues) {
                    for (double sl : slValues) {
                        if (tp <= sl) continue; // TP must be > SL for positive expectancy
                        ParamSet p = baseParams;
                        p["tp_pips"] = tp;
                        p["sl_pips"] = sl;
                        expanded.push_back(p);
                    }
                }
            }
            entry.paramCombinations = std::move(expanded);
        }
        
        return entries;
    }
    
    // Count total parameter combinations across all strategies
    static int totalCombinations(
        const std::vector<std::string>& allowedStrategies = {},
        const std::vector<double>& tradeDurations = {},
        const std::vector<double>& candlePeriods = {}
    ) {
        int total = 0;
        for (auto& e : getAllGridEntries(allowedStrategies, tradeDurations, candlePeriods)) {
            total += (int)e.paramCombinations.size();
        }
        return total;
    }
};
