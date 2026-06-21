#pragma once
#include "strategy_base.hpp"
#include "../indicators/stochastic.hpp"
#include <sstream>

// Stochastic Crossover Strategy
//
// Rise: %K crosses above %D in oversold territory (< oversoldThresh)
// Fall: %K crosses below %D in overbought territory (> overboughtThresh)
//
// Edge-triggered: fires once per crossover event, re-arms when leaving the zone.

class StochasticStrategy : public StrategyBase {
public:
    StochasticStrategy(int kPeriod = 14, int dPeriod = 3,
                       double oversold = 20.0, double overbought = 80.0)
        : stoch_(kPeriod, dPeriod),
          kPeriod_(kPeriod), dPeriod_(dPeriod),
          oversold_(oversold), overbought_(overbought) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        auto out = stoch_.update(price);
        if (!out.has_value()) return Signal::None;

        double k = out->k;
        double d = out->d;
        Signal sig = Signal::None;

        if (hasPrev_) {
            // %K crosses above %D in oversold zone → Rise
            if (prevK_ <= prevD_ && k > d && k < oversold_) {
                if (!riseArmed_) {
                    sig = Signal::Rise;
                    riseArmed_ = true;
                }
            } else if (k > oversold_) {
                riseArmed_ = false;
            }

            // %K crosses below %D in overbought zone → Fall
            if (prevK_ >= prevD_ && k < d && k > overbought_) {
                if (!fallArmed_) {
                    sig = Signal::Fall;
                    fallArmed_ = true;
                }
            } else if (k < overbought_) {
                fallArmed_ = false;
            }
        }

        prevK_   = k;
        prevD_   = d;
        hasPrev_ = true;
        return sig;
    }

    std::string name() const override { return "stochastic"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "k=" << kPeriod_ << " d=" << dPeriod_
           << " os=" << oversold_ << " ob=" << overbought_;
        return ss.str();
    }

private:
    Stochastic stoch_;
    int    kPeriod_, dPeriod_;
    double oversold_, overbought_;
    double prevK_     = 50.0;
    double prevD_     = 50.0;
    bool   hasPrev_   = false;
    bool   riseArmed_ = false;
    bool   fallArmed_ = false;
};
