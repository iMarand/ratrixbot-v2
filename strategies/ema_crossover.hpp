#pragma once
#include "strategy_base.hpp"
#include "../indicators/ema.hpp"
#include <sstream>

// EMA Crossover Strategy
// Fires Rise when fast EMA crosses above slow EMA (golden cross).
// Fires Fall when fast EMA crosses below slow EMA (death cross).
// Classic trend-following strategy.

class EmaCrossoverStrategy : public StrategyBase {
public:
    EmaCrossoverStrategy(int fastPeriod = 10, int slowPeriod = 30)
        : fastEma_(fastPeriod), slowEma_(slowPeriod),
          fastPeriod_(fastPeriod), slowPeriod_(slowPeriod) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        auto f = fastEma_.update(price);
        auto s = slowEma_.update(price);

        if (!f.has_value() || !s.has_value()) return Signal::None;

        double fast = *f;
        double slow = *s;
        Signal sig = Signal::None;

        if (prevFast_ <= prevSlow_ && fast > slow && hasPrev_) {
            sig = Signal::Rise;  // fast crossed above slow
        } else if (prevFast_ >= prevSlow_ && fast < slow && hasPrev_) {
            sig = Signal::Fall;  // fast crossed below slow
        }

        prevFast_ = fast;
        prevSlow_ = slow;
        hasPrev_  = true;
        return sig;
    }

    std::string name() const override { return "ema_cross"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "fast=" << fastPeriod_ << " slow=" << slowPeriod_;
        return ss.str();
    }

private:
    EMA    fastEma_;
    EMA    slowEma_;
    int    fastPeriod_;
    int    slowPeriod_;
    double prevFast_ = 0.0;
    double prevSlow_ = 0.0;
    bool   hasPrev_  = false;
};
