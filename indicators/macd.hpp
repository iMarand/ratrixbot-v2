#pragma once
#include "ema.hpp"
#include <optional>

// MACD (Moving Average Convergence Divergence)
//   MACD line   = EMA(fast) − EMA(slow)
//   Signal line  = EMA(signalPeriod) of MACD line
//   Histogram    = MACD − Signal
//
// All three are returned together once the slowest component is seeded.
struct MACDOutput {
    double macd      = 0.0;
    double signal    = 0.0;
    double histogram = 0.0;
};

class MACD {
public:
    MACD(int fastPeriod = 12, int slowPeriod = 26, int signalPeriod = 9)
        : fast_(fastPeriod), slow_(slowPeriod), signal_(signalPeriod) {}

    std::optional<MACDOutput> update(double price) {
        auto f = fast_.update(price);
        auto s = slow_.update(price);

        if (!f.has_value() || !s.has_value()) return std::nullopt;

        double macdLine = *f - *s;
        auto sig = signal_.update(macdLine);

        if (!sig.has_value()) return std::nullopt;

        MACDOutput out;
        out.macd      = macdLine;
        out.signal    = *sig;
        out.histogram = macdLine - *sig;
        lastOutput_   = out;
        return out;
    }

    bool isReady() const { return slow_.isReady() && lastOutput_.has_value(); }
    MACDOutput lastOutput() const { return lastOutput_.value_or(MACDOutput{}); }

private:
    EMA fast_;
    EMA slow_;
    EMA signal_;
    std::optional<MACDOutput> lastOutput_;
};
