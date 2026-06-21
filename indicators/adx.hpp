#pragma once
#include "ema.hpp"
#include <optional>
#include <cmath>
#include <deque>
#include <algorithm>

// Average Directional Index (ADX) — adapted for tick data.
//
// In OHLC: +DI and -DI measure directional movement, ADX smooths their
// relationship into a 0-100 trend-strength score.
//
// For tick data we approximate: every `resolution` ticks we compute a
// "bar" (high/low/close over that window). Directional movement is
// computed from consecutive bars. ADX is then the smoothed average of
// the directional index (DX).
//
//   ADX > 25  → trending market
//   ADX < 20  → ranging/choppy market
class ADX {
public:
    ADX(int period = 14, int resolution = 20)
        : period_(period), resolution_(resolution) {}

    std::optional<double> update(double price) {
        tickBuf_.push_back(price);
        tickCount_++;

        if (tickCount_ % resolution_ != 0) {
            if (seeded_) return value_;
            return std::nullopt;
        }

        // Compute bar from tick buffer
        double barHigh  = *std::max_element(tickBuf_.begin(), tickBuf_.end());
        double barLow   = *std::min_element(tickBuf_.begin(), tickBuf_.end());
        double barClose = tickBuf_.back();
        tickBuf_.clear();

        if (!hasPrevBar_) {
            prevHigh_ = barHigh;
            prevLow_  = barLow;
            prevClose_ = barClose;
            hasPrevBar_ = true;
            return std::nullopt;
        }

        // Directional movement
        double upMove   = barHigh - prevHigh_;
        double downMove = prevLow_ - barLow;
        double plusDM  = (upMove > downMove && upMove > 0) ? upMove : 0.0;
        double minusDM = (downMove > upMove && downMove > 0) ? downMove : 0.0;

        // True range
        double tr = std::max({barHigh - barLow,
                              std::abs(barHigh - prevClose_),
                              std::abs(barLow - prevClose_)});

        prevHigh_ = barHigh;
        prevLow_  = barLow;
        prevClose_ = barClose;

        if (!smoothSeeded_) {
            trSum_      += tr;
            plusDMSum_   += plusDM;
            minusDMSum_ += minusDM;
            barCount_++;

            if (barCount_ == period_) {
                smoothTR_      = trSum_;
                smoothPlusDM_  = plusDMSum_;
                smoothMinusDM_ = minusDMSum_;
                smoothSeeded_  = true;
                // Compute first DX
                double plusDI  = (smoothTR_ > 0) ? 100.0 * smoothPlusDM_ / smoothTR_ : 0.0;
                double minusDI = (smoothTR_ > 0) ? 100.0 * smoothMinusDM_ / smoothTR_ : 0.0;
                double diSum   = plusDI + minusDI;
                double dx = (diSum > 0) ? 100.0 * std::abs(plusDI - minusDI) / diSum : 0.0;
                dxSum_ += dx;
                dxCount_++;
            }
            return std::nullopt;
        }

        // Wilder's smoothing for TR, +DM, -DM
        smoothTR_      = smoothTR_      - (smoothTR_ / period_) + tr;
        smoothPlusDM_  = smoothPlusDM_  - (smoothPlusDM_ / period_) + plusDM;
        smoothMinusDM_ = smoothMinusDM_ - (smoothMinusDM_ / period_) + minusDM;

        double plusDI  = (smoothTR_ > 0) ? 100.0 * smoothPlusDM_ / smoothTR_ : 0.0;
        double minusDI = (smoothTR_ > 0) ? 100.0 * smoothMinusDM_ / smoothTR_ : 0.0;
        double diSum   = plusDI + minusDI;
        double dx = (diSum > 0) ? 100.0 * std::abs(plusDI - minusDI) / diSum : 0.0;

        if (!seeded_) {
            dxSum_ += dx;
            dxCount_++;
            if (dxCount_ == period_) {
                value_ = dxSum_ / period_;
                seeded_ = true;
                return value_;
            }
            return std::nullopt;
        }

        // Smooth ADX
        value_ = (value_ * (period_ - 1) + dx) / period_;
        return value_;
    }

    bool   isReady() const { return seeded_; }
    double value()   const { return value_; }

private:
    int period_;
    int resolution_;
    std::deque<double> tickBuf_;
    int tickCount_ = 0;

    bool   hasPrevBar_ = false;
    double prevHigh_   = 0.0;
    double prevLow_    = 0.0;
    double prevClose_  = 0.0;

    // First-pass accumulation
    int    barCount_    = 0;
    double trSum_       = 0.0;
    double plusDMSum_   = 0.0;
    double minusDMSum_  = 0.0;

    // Smoothed values
    bool   smoothSeeded_  = false;
    double smoothTR_      = 0.0;
    double smoothPlusDM_  = 0.0;
    double smoothMinusDM_ = 0.0;

    // DX -> ADX
    double dxSum_   = 0.0;
    int    dxCount_ = 0;
    bool   seeded_  = false;
    double value_   = 0.0;
};
