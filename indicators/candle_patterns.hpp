#pragma once
#include "ohlc_builder.hpp"

// ============================================================================
// Candle Pattern Detector
// Analyzes recent finalized candles to detect classic price action patterns.
// ============================================================================

enum class CandlePattern {
    None,
    BullishEngulfing,
    BearishEngulfing,
    BullishHarami,
    BearishHarami,
    Hammer,
    ShootingStar,
    PiercingLine,
    DarkCloudCover
};

class CandlePatternDetector {
public:
    // Call this every time the OHLCBuilder returns a finalized candle
    CandlePattern analyze(const Candle& current) {
        CandlePattern detected = CandlePattern::None;

        if (hasPrev_) {
            // 2-candle patterns
            if (isBullishEngulfing(prev_, current)) detected = CandlePattern::BullishEngulfing;
            else if (isBearishEngulfing(prev_, current)) detected = CandlePattern::BearishEngulfing;
            else if (isBullishHarami(prev_, current)) detected = CandlePattern::BullishHarami;
            else if (isBearishHarami(prev_, current)) detected = CandlePattern::BearishHarami;
            else if (isPiercingLine(prev_, current)) detected = CandlePattern::PiercingLine;
            else if (isDarkCloudCover(prev_, current)) detected = CandlePattern::DarkCloudCover;
        }

        // 1-candle patterns (fallback if no 2-candle pattern found)
        if (detected == CandlePattern::None) {
            if (isHammer(current)) detected = CandlePattern::Hammer;
            else if (isShootingStar(current)) detected = CandlePattern::ShootingStar;
        }

        prev_ = current;
        hasPrev_ = true;

        return detected;
    }

private:
    bool hasPrev_ = false;
    Candle prev_;

    // Pattern Logic

    bool isBullishEngulfing(const Candle& c1, const Candle& c2) const {
        if (!c1.isRed() || !c2.isGreen()) return false;
        // c2 body completely engulfs c1 body
        return c2.open <= c1.close && c2.close >= c1.open;
    }

    bool isBearishEngulfing(const Candle& c1, const Candle& c2) const {
        if (!c1.isGreen() || !c2.isRed()) return false;
        // c2 body completely engulfs c1 body
        return c2.open >= c1.close && c2.close <= c1.open;
    }

    bool isBullishHarami(const Candle& c1, const Candle& c2) const {
        if (!c1.isRed() || !c2.isGreen()) return false;
        // c2 body completely inside c1 body
        return c2.open >= c1.close && c2.close <= c1.open;
    }

    bool isBearishHarami(const Candle& c1, const Candle& c2) const {
        if (!c1.isGreen() || !c2.isRed()) return false;
        // c2 body completely inside c1 body
        return c2.open <= c1.close && c2.close >= c1.open;
    }

    bool isPiercingLine(const Candle& c1, const Candle& c2) const {
        if (!c1.isRed() || !c2.isGreen()) return false;
        double midPoint = c1.close + (c1.bodySize() / 2.0);
        return c2.open < c1.low && c2.close > midPoint && c2.close < c1.open;
    }

    bool isDarkCloudCover(const Candle& c1, const Candle& c2) const {
        if (!c1.isGreen() || !c2.isRed()) return false;
        double midPoint = c1.open + (c1.bodySize() / 2.0);
        return c2.open > c1.high && c2.close < midPoint && c2.close > c1.open;
    }

    bool isHammer(const Candle& c) const {
        // Small body, long lower wick (>= 2x body), tiny or no upper wick (<= 0.5x body)
        if (c.bodySize() == 0) return false;
        return c.lowerWick() >= (2.0 * c.bodySize()) && 
               c.upperWick() <= (0.5 * c.bodySize());
    }

    bool isShootingStar(const Candle& c) const {
        // Small body, long upper wick (>= 2x body), tiny or no lower wick (<= 0.5x body)
        if (c.bodySize() == 0) return false;
        return c.upperWick() >= (2.0 * c.bodySize()) && 
               c.lowerWick() <= (0.5 * c.bodySize());
    }
};
