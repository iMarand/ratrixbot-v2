#pragma once
#include "../indicators/ema.hpp"
#include "../indicators/atr.hpp"
#include "../indicators/adx.hpp"
#include "../rsi.hpp"
#include <vector>
#include <cstdint>
#include <optional>

// ============================================================================
// StateFeatures — extracts a discretized market regime from recent price data.
//
// Four dimensions:
//   Trend:      EMA(10) vs EMA(50) → {downtrend=0, flat=1, uptrend=2}
//   Volatility: ATR percentile vs running average → {low=0, medium=1, high=2}
//   Momentum:   RSI zone → {oversold=0, neutral=1, overbought=2}
//   Regime:     ADX → {ranging=0, trending=1}
//
// Total state space: 3 × 3 × 3 × 2 = 54 states (perfect for tabular Q-learning)
// ============================================================================

struct MarketState {
    int trend      = 1;  // 0=down, 1=flat, 2=up
    int volatility = 1;  // 0=low, 1=medium, 2=high
    int momentum   = 1;  // 0=oversold, 1=neutral, 2=overbought
    int regime     = 0;  // 0=ranging, 1=trending

    // Flatten to a single integer index for Q-table lookup
    int toIndex() const {
        return trend * 18 + volatility * 6 + momentum * 2 + regime;
    }

    static constexpr int NUM_STATES = 54;  // 3 * 3 * 3 * 2
};

class StateFeatureExtractor {
public:
    StateFeatureExtractor()
        : emaFast_(10), emaSlow_(50), atr_(14, 20), adx_(14, 20), rsi_(14) {}

    // Feed one tick. Returns a discretized state once all indicators are ready.
    std::optional<MarketState> update(double price) {
        auto ef   = emaFast_.update(price);
        auto es   = emaSlow_.update(price);
        auto aVal = atr_.update(price);
        auto dVal = adx_.update(price);
        auto rVal = rsi_.update(price);

        if (!ef || !es || !rVal) return std::nullopt;

        MarketState state;

        // Trend: compare fast EMA to slow EMA
        double emaDiff = (*ef - *es) / *es;
        if (emaDiff > 0.0005)       state.trend = 2;  // uptrend
        else if (emaDiff < -0.0005) state.trend = 0;  // downtrend
        else                        state.trend = 1;  // flat

        // Volatility: based on ATR relative to price
        if (aVal.has_value()) {
            double atrPct = *aVal / price;
            // Track running average of ATR% to set thresholds dynamically
            atrAvg_ = atrAvg_ * 0.99 + atrPct * 0.01;
            atrCount_++;
            if (atrCount_ > 100) {
                if (atrPct < atrAvg_ * 0.7)      state.volatility = 0;  // low
                else if (atrPct > atrAvg_ * 1.3)  state.volatility = 2;  // high
                else                               state.volatility = 1;  // medium
            }
        }

        // Momentum: RSI zones
        double rsi = *rVal;
        if (rsi <= 30.0)      state.momentum = 0;  // oversold
        else if (rsi >= 70.0) state.momentum = 2;  // overbought
        else                  state.momentum = 1;  // neutral

        // Regime: ADX
        if (dVal.has_value()) {
            state.regime = (*dVal > 25.0) ? 1 : 0;
        }

        return state;
    }

private:
    EMA emaFast_;
    EMA emaSlow_;
    ATR atr_;
    ADX adx_;
    RSI rsi_;
    double atrAvg_  = 0.0;
    int    atrCount_ = 0;
};
