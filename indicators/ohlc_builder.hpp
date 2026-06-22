#pragma once
#include <cstdint>
#include <optional>
#include <cmath>

// ============================================================================
// OHLC Builder
// Aggregates raw tick stream into Time-based Candles.
// ============================================================================

struct Candle {
    int64_t time;       // Start time of the candle
    double  open;
    double  high;
    double  low;
    double  close;
    int     tickCount;

    double bodySize() const { return std::abs(open - close); }
    double upperWick() const { return high - std::max(open, close); }
    double lowerWick() const { return std::min(open, close) - low; }
    double range() const { return high - low; }
    
    bool isGreen() const { return close > open; }
    bool isRed() const { return close < open; }
    bool isDoji() const { return bodySize() <= (range() * 0.1); } // Body is <= 10% of total range
};

class OHLCBuilder {
public:
    OHLCBuilder(int periodSec) : periodSec_(periodSec) {}

    // Feeds a tick and returns a finalized candle ONLY if this tick pushed us into a new time period.
    std::optional<Candle> update(int64_t time, double price) {
        int64_t candleGridTime = (time / periodSec_) * periodSec_;

        if (!hasCurrent_) {
            // First tick ever
            startNewCandle(candleGridTime, price);
            return std::nullopt;
        }

        if (candleGridTime > current_.time) {
            // This tick belongs to a new time bucket.
            // Save the finalized candle to return it.
            Candle finalized = current_;
            
            // Start a new candle with this tick
            startNewCandle(candleGridTime, price);
            
            return finalized;
        }

        // Update current candle
        current_.high = std::max(current_.high, price);
        current_.low = std::min(current_.low, price);
        current_.close = price;
        current_.tickCount++;

        return std::nullopt;
    }

private:
    void startNewCandle(int64_t time, double price) {
        current_.time = time;
        current_.open = price;
        current_.high = price;
        current_.low = price;
        current_.close = price;
        current_.tickCount = 1;
        hasCurrent_ = true;
    }

    int periodSec_;
    Candle current_;
    bool hasCurrent_ = false;
};
