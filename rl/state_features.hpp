#pragma once
#include "../indicators/ema.hpp"
#include "../indicators/atr.hpp"
#include "../indicators/adx.hpp"
#include "../indicators/ohlc_builder.hpp"
#include "../indicators/candle_patterns.hpp"
#include "../rsi.hpp"
#include <vector>
#include <cstdint>
#include <optional>

// ============================================================================
// StateFeatures — extracts a discretized market regime from recent price data.
//
// Five dimensions:
//   Trend:        EMA(10) vs EMA(50) → {downtrend=0, flat=1, uptrend=2}
//   Volatility:   ATR percentile vs running average → {low=0, medium=1, high=2}
//   Momentum:     RSI zone → {oversold=0, neutral=1, overbought=2}
//   Regime:       ADX → {ranging=0, trending=1}
//   CandleSignal: Last candle pattern → {bearish=0, neutral=1, bullish=2}
//
// Total state space: 3 × 3 × 3 × 2 × 3 = 162 states
// ============================================================================

struct MarketState {
    int trend        = 1;  // 0=down, 1=flat, 2=up
    int volatility   = 1;  // 0=low, 1=medium, 2=high
    int momentum     = 1;  // 0=oversold, 1=neutral, 2=overbought
    int regime       = 0;  // 0=ranging, 1=trending
    int candleSignal = 1;  // 0=bearish, 1=neutral, 2=bullish

    // Flatten to a single integer index for Q-table lookup
    int toIndex() const {
        // trend(3) × volatility(3) × momentum(3) × regime(2) × candle(3)
        return ((trend * 3 + volatility) * 3 + momentum) * 2 * 3
               + regime * 3 + candleSignal;
    }

    static constexpr int NUM_STATES = 162;  // 3 * 3 * 3 * 2 * 3
};

class StateFeatureExtractor {
public:
    StateFeatureExtractor()
        : emaFast_(10), emaSlow_(50), atr_(14, 20), adx_(14, 20), rsi_(14),
          ohlc_(15) {}

    // Feed one tick. Returns a discretized state once all indicators are ready.
    std::optional<MarketState> update(double price, int64_t time = 0) {
        auto ef   = emaFast_.update(price);
        auto es   = emaSlow_.update(price);
        auto aVal = atr_.update(price);
        auto dVal = adx_.update(price);
        auto rVal = rsi_.update(price);

        // Update candle builder
        if (time > 0) {
            auto candleOpt = ohlc_.update(time, price);
            if (candleOpt.has_value()) {
                CandlePattern pattern = candleDetector_.analyze(*candleOpt);
                switch (pattern) {
                    case CandlePattern::BullishEngulfing:
                    case CandlePattern::BullishHarami:
                    case CandlePattern::Hammer:
                    case CandlePattern::PiercingLine:
                        lastCandleSignal_ = 2;  // bullish
                        break;
                    case CandlePattern::BearishEngulfing:
                    case CandlePattern::BearishHarami:
                    case CandlePattern::ShootingStar:
                    case CandlePattern::DarkCloudCover:
                        lastCandleSignal_ = 0;  // bearish
                        break;
                    default:
                        lastCandleSignal_ = 1;  // neutral
                        break;
                }
            }
        }

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

        // Candle signal
        state.candleSignal = lastCandleSignal_;

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

    // Candle pattern analysis
    OHLCBuilder ohlc_;
    CandlePatternDetector candleDetector_;
    int lastCandleSignal_ = 1;  // neutral by default
};
