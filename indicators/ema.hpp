#pragma once
#include <optional>

// Incremental Exponential Moving Average.
// Feed one price at a time via update(). The first `period` prices are used
// to seed with an SMA, after which the standard EMA formula kicks in:
//   EMA_t = price * k + EMA_{t-1} * (1 - k),  where k = 2 / (period + 1)
class EMA {
public:
    explicit EMA(int period)
        : period_(period), k_(2.0 / (period + 1)) {}

    std::optional<double> update(double price) {
        if (!seeded_) {
            sum_ += price;
            count_++;
            if (count_ == period_) {
                value_ = sum_ / period_;
                seeded_ = true;
                return value_;
            }
            return std::nullopt;
        }

        value_ = price * k_ + value_ * (1.0 - k_);
        return value_;
    }

    bool   isReady() const { return seeded_; }
    double value()   const { return value_; }

private:
    int    period_;
    double k_;
    bool   seeded_ = false;
    double sum_    = 0.0;
    int    count_  = 0;
    double value_  = 0.0;
};
