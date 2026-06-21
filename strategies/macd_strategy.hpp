#pragma once
#include "strategy_base.hpp"
#include "../indicators/macd.hpp"
#include <sstream>

// MACD Histogram Reversal Strategy
// Fires Rise when MACD histogram crosses from negative to positive (momentum shifting bullish).
// Fires Fall when MACD histogram crosses from positive to negative (momentum shifting bearish).

class MacdStrategy : public StrategyBase {
public:
    MacdStrategy(int fastPeriod = 12, int slowPeriod = 26, int signalPeriod = 9)
        : macd_(fastPeriod, slowPeriod, signalPeriod),
          fastP_(fastPeriod), slowP_(slowPeriod), sigP_(signalPeriod) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        auto out = macd_.update(price);
        if (!out.has_value()) return Signal::None;

        double hist = out->histogram;
        Signal sig = Signal::None;

        if (hasPrev_) {
            if (prevHist_ <= 0.0 && hist > 0.0) {
                sig = Signal::Rise;  // histogram crossed above zero
            } else if (prevHist_ >= 0.0 && hist < 0.0) {
                sig = Signal::Fall;  // histogram crossed below zero
            }
        }

        prevHist_ = hist;
        hasPrev_  = true;
        return sig;
    }

    std::string name() const override { return "macd"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "fast=" << fastP_ << " slow=" << slowP_ << " signal=" << sigP_;
        return ss.str();
    }

private:
    MACD   macd_;
    int    fastP_, slowP_, sigP_;
    double prevHist_ = 0.0;
    bool   hasPrev_  = false;
};
