#pragma once
#include "strategy_base.hpp"
#include "../indicators/bollinger.hpp"
#include "../rsi.hpp"
#include <sstream>

// Bollinger Band Bounce Strategy (Mean-Reversion)
//
// Rise signal: price touches/dips below lower Bollinger band AND RSI is oversold
//   → expect bounce back toward the mean
// Fall signal: price touches/rises above upper Bollinger band AND RSI is overbought
//   → expect rejection back toward the mean
//
// This is a mean-reversion strategy: it works best in ranging/choppy markets
// (low ADX) and gets destroyed in strong trends.

class BollingerStrategy : public StrategyBase {
public:
    BollingerStrategy(int bbPeriod = 20, double bbStdDev = 2.0,
                      int rsiPeriod = 14, double oversold = 30.0, double overbought = 70.0)
        : bb_(bbPeriod, bbStdDev), rsi_(rsiPeriod),
          bbPeriod_(bbPeriod), bbStdDev_(bbStdDev),
          rsiPeriod_(rsiPeriod), oversold_(oversold), overbought_(overbought) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        auto bands  = bb_.update(price);
        auto rsiVal = rsi_.update(price);

        if (!bands.has_value() || !rsiVal.has_value()) return Signal::None;
        lastRsi_ = *rsiVal;

        // Price at or below lower band + RSI oversold → bounce up
        if (price <= bands->lower && *rsiVal <= oversold_) {
            if (!lowerArmed_) {
                lowerArmed_ = true;
                return Signal::Rise;
            }
        } else {
            lowerArmed_ = false;
        }

        // Price at or above upper band + RSI overbought → reject down
        if (price >= bands->upper && *rsiVal >= overbought_) {
            if (!upperArmed_) {
                upperArmed_ = true;
                return Signal::Fall;
            }
        } else {
            upperArmed_ = false;
        }

        return Signal::None;
    }

    std::string name() const override { return "bollinger"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "bb_period=" << bbPeriod_ << " bb_std=" << bbStdDev_
           << " rsi=" << rsiPeriod_ << " os=" << oversold_ << " ob=" << overbought_;
        return ss.str();
    }

    double lastRsi() const override { return lastRsi_; }

private:
    BollingerBands bb_;
    RSI            rsi_;
    int    bbPeriod_;
    double bbStdDev_;
    int    rsiPeriod_;
    double oversold_;
    double overbought_;
    double lastRsi_     = 50.0;
    bool   lowerArmed_  = false;
    bool   upperArmed_  = false;
};
