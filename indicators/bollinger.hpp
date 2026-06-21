#pragma once
#include <optional>
#include <deque>
#include <cmath>
#include <numeric>

// Bollinger Bands: middle = SMA(period), upper/lower = middle ± k × σ
// Feed one price at a time. Returns bands once enough prices have been seen.
struct BollingerOutput {
    double upper    = 0.0;
    double middle   = 0.0;
    double lower    = 0.0;
    double bandwidth = 0.0;   // (upper - lower) / middle — measures squeeze/expansion
};

class BollingerBands {
public:
    BollingerBands(int period = 20, double numStdDev = 2.0)
        : period_(period), numStdDev_(numStdDev) {}

    std::optional<BollingerOutput> update(double price) {
        window_.push_back(price);
        if ((int)window_.size() > period_) window_.pop_front();
        if ((int)window_.size() < period_) return std::nullopt;

        double sum = std::accumulate(window_.begin(), window_.end(), 0.0);
        double mean = sum / period_;

        double sqSum = 0.0;
        for (double p : window_) {
            double diff = p - mean;
            sqSum += diff * diff;
        }
        double stddev = std::sqrt(sqSum / period_);

        BollingerOutput out;
        out.middle = mean;
        out.upper  = mean + numStdDev_ * stddev;
        out.lower  = mean - numStdDev_ * stddev;
        out.bandwidth = (mean > 0.0) ? (out.upper - out.lower) / mean : 0.0;
        lastOutput_ = out;
        return out;
    }

    bool isReady() const { return lastOutput_.has_value(); }
    BollingerOutput lastOutput() const { return lastOutput_.value_or(BollingerOutput{}); }

private:
    int    period_;
    double numStdDev_;
    std::deque<double> window_;
    std::optional<BollingerOutput> lastOutput_;
};
