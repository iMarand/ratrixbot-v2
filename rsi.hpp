#pragma once
#include <optional>

// Incremental RSI using Wilder's smoothing method.
// Feed it one price at a time via update(); once at least `period + 1`
// prices have been seen it starts returning real RSI values (0-100).
class RSI {
public:
    explicit RSI(int period) : period_(period) {}

    std::optional<double> update(double price) {
        std::optional<double> result;

        if (hasPrev_) {
            double change = price - prevPrice_;
            double gain = change > 0 ? change : 0.0;
            double loss = change < 0 ? -change : 0.0;

            if (count_ < period_) {
                gainSum_ += gain;
                lossSum_ += loss;
                count_++;
                if (count_ == period_) {
                    avgGain_ = gainSum_ / period_;
                    avgLoss_ = lossSum_ / period_;
                    seeded_ = true;
                    result = computeRsi();
                }
            } else {
                avgGain_ = (avgGain_ * (period_ - 1) + gain) / period_;
                avgLoss_ = (avgLoss_ * (period_ - 1) + loss) / period_;
                result = computeRsi();
            }
        }

        prevPrice_ = price;
        hasPrev_ = true;
        return result;
    }

    bool isReady() const { return seeded_; }

private:
    double computeRsi() const {
        if (avgLoss_ == 0.0) return 100.0;
        double rs = avgGain_ / avgLoss_;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    int period_;
    bool hasPrev_ = false;
    double prevPrice_ = 0.0;
    int count_ = 0;
    double gainSum_ = 0.0;
    double lossSum_ = 0.0;
    bool seeded_ = false;
    double avgGain_ = 0.0;
    double avgLoss_ = 0.0;
};