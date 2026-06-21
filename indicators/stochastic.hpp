#pragma once
#include <optional>
#include <deque>
#include <algorithm>

// Stochastic Oscillator
//   %K = (price − lowestLow) / (highestHigh − lowestLow) × 100
//   %D = SMA(%K, dPeriod)
//
// For tick data we treat each tick as a "close" and track the rolling
// highest/lowest over the kPeriod window.
struct StochasticOutput {
    double k = 50.0;   // fast stochastic (0-100)
    double d = 50.0;   // slow stochastic (smoothed %K)
};

class Stochastic {
public:
    Stochastic(int kPeriod = 14, int dPeriod = 3)
        : kPeriod_(kPeriod), dPeriod_(dPeriod) {}

    std::optional<StochasticOutput> update(double price) {
        priceWindow_.push_back(price);
        if ((int)priceWindow_.size() > kPeriod_) priceWindow_.pop_front();
        if ((int)priceWindow_.size() < kPeriod_) return std::nullopt;

        double highest = *std::max_element(priceWindow_.begin(), priceWindow_.end());
        double lowest  = *std::min_element(priceWindow_.begin(), priceWindow_.end());

        double range = highest - lowest;
        double kVal = (range > 0.0) ? ((price - lowest) / range) * 100.0 : 50.0;

        // Smooth %K into %D via simple moving average
        kWindow_.push_back(kVal);
        if ((int)kWindow_.size() > dPeriod_) kWindow_.pop_front();

        double dVal = 0.0;
        for (double v : kWindow_) dVal += v;
        dVal /= (double)kWindow_.size();

        if ((int)kWindow_.size() < dPeriod_) return std::nullopt;

        StochasticOutput out;
        out.k = kVal;
        out.d = dVal;
        lastOutput_ = out;
        return out;
    }

    bool isReady() const { return lastOutput_.has_value(); }
    StochasticOutput lastOutput() const { return lastOutput_.value_or(StochasticOutput{}); }

private:
    int kPeriod_;
    int dPeriod_;
    std::deque<double> priceWindow_;
    std::deque<double> kWindow_;
    std::optional<StochasticOutput> lastOutput_;
};
