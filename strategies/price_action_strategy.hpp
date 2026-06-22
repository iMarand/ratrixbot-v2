#pragma once
#include "strategy_base.hpp"
#include "../indicators/ohlc_builder.hpp"
#include "../indicators/candle_patterns.hpp"
#include <sstream>

// ============================================================================
// Price Action Strategy
// Converts ticks to OHLC candles and trades classic reversal patterns.
// ============================================================================

class PriceActionStrategy : public StrategyBase {
public:
    PriceActionStrategy(int candlePeriodSec) 
        : ohlc_(candlePeriodSec), period_(candlePeriodSec) {}

    Signal onPrice(int64_t time, double price) override {
        auto candleOpt = ohlc_.update(time, price);
        
        if (candleOpt.has_value()) {
            CandlePattern pattern = detector_.analyze(*candleOpt);

            if (pattern == CandlePattern::BullishEngulfing || 
                pattern == CandlePattern::BullishHarami ||
                pattern == CandlePattern::Hammer || 
                pattern == CandlePattern::PiercingLine) {
                return Signal::Rise;
            }

            if (pattern == CandlePattern::BearishEngulfing || 
                pattern == CandlePattern::BearishHarami ||
                pattern == CandlePattern::ShootingStar || 
                pattern == CandlePattern::DarkCloudCover) {
                return Signal::Fall;
            }
        }

        return Signal::None;
    }

    std::string name() const override { return "price_action"; }
    
    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "candle_period=" << period_;
        return ss.str();
    }

private:
    OHLCBuilder ohlc_;
    CandlePatternDetector detector_;
    int period_;
};
