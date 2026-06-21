#pragma once
#include <optional>
#include <deque>
#include <cmath>
#include <algorithm>

// Average True Range (ATR) — adapted for tick data.
//
// In OHLC markets, true range = max(H-L, |H-prevC|, |L-prevC|).
// With tick-only data we approximate: we maintain a rolling window of ticks
// and compute the range (max - min) over each sub-window of `resolution`
// ticks, then smooth those ranges with Wilder's method over `period` bars.
//
// This gives a reasonable volatility measure even without candles.
class ATR {
public:
    ATR(int period = 14, int resolution = 20)
        : period_(period), resolution_(resolution) {}

    std::optional<double> update(double price) {
        tickWindow_.push_back(price);
        if ((int)tickWindow_.size() > resolution_) tickWindow_.pop_front();
        if ((int)tickWindow_.size() < resolution_) return std::nullopt;

        double high = *std::max_element(tickWindow_.begin(), tickWindow_.end());
        double low  = *std::min_element(tickWindow_.begin(), tickWindow_.end());
        double tr   = high - low;

        tickCount_++;
        if (tickCount_ % resolution_ != 0) {
            // Only update ATR once per "bar" (every `resolution` ticks)
            if (seeded_) return value_;
            return std::nullopt;
        }

        if (!seeded_) {
            trSum_ += tr;
            barCount_++;
            if (barCount_ == period_) {
                value_ = trSum_ / period_;
                seeded_ = true;
                return value_;
            }
            return std::nullopt;
        }

        // Wilder's smoothing
        value_ = (value_ * (period_ - 1) + tr) / period_;
        return value_;
    }

    bool   isReady() const { return seeded_; }
    double value()   const { return value_; }

private:
    int period_;
    int resolution_;
    std::deque<double> tickWindow_;
    int    tickCount_ = 0;
    int    barCount_  = 0;
    double trSum_     = 0.0;
    bool   seeded_    = false;
    double value_     = 0.0;
};
