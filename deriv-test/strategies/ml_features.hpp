#pragma once
#include "../indicators/ohlc_builder.hpp"
#include <vector>
#include <algorithm>
#include <cmath>

// ============================================================================
// ml_features.hpp
//
// Shared candle-feature extraction used by the machine-learning strategies
// (Random Forest, XGBoost). Each finalized candle is turned into a numeric
// feature vector that describes -- in the same language a human chart reader
// uses -- what the candle is and the context it appears in:
//
//   * Colour:      is this candle green (close>open) or red? what was the last?
//   * Shape:       Doji, Hammer, Shooting Star (single-candle reversal shapes)
//   * Two-bar:     Bullish/Bearish Engulfing, Harami, Piercing, Dark Cloud
//   * Context:     trend slope and run-length -- candlestick patterns from
//                  candles.md are REVERSAL signals, so they only mean something
//                  relative to the trend they appear in.
//   * Volatility:  range vs recent average (narrow-range / expansion bars)
//
// Feeding the models this richer, named feature set (instead of 7 generic
// ratios) is what lets them actually "learn the candles" rather than fit
// noise. References: candles.md (Fidelity TA deck) -- Doji, Harami, Hammer,
// Shooting Star, Engulfing, Dark Cloud / Piercing, trend & confirmation.
// ============================================================================

struct CandleFeatures {
    double bodyRatio       = 0.5;   // |open-close| / range            [0,1]
    double upperWickRatio  = 0.0;   // upper wick / range              [0,1]
    double lowerWickRatio  = 0.0;   // lower wick / range              [0,1]
    double isGreen         = 0.0;   // current candle bullish?         (0/1)
    double prevGreen       = 0.0;   // previous candle bullish?        (0/1)
    double isDoji          = 0.0;   // indecision                      (0/1)
    double isHammer        = 0.0;   // long lower wick, body up top    (0/1)
    double isShootingStar  = 0.0;   // long upper wick, body at bottom (0/1)
    double bullEngulf      = 0.0;   // green body engulfs prev red     (0/1)
    double bearEngulf      = 0.0;   // red body engulfs prev green     (0/1)
    double isHarami        = 0.0;   // small body inside prev big body (0/1)
    double piercing        = 0.0;   // bullish piercing line           (0/1)
    double darkCloud       = 0.0;   // bearish dark cloud cover        (0/1)
    double directionChange = 0.0;   // colour flipped vs previous      (0/1)
    double rangeVsAvg      = 1.0;   // current range / avg of last N   (ratio)
    double consecSameDir   = 0.0;   // signed run length (+green/-red)
    double trendSlope      = 0.0;   // normalised momentum over last N

    static constexpr int NUM_FEATURES = 17;

    double get(int idx) const {
        switch (idx) {
            case 0:  return bodyRatio;
            case 1:  return upperWickRatio;
            case 2:  return lowerWickRatio;
            case 3:  return isGreen;
            case 4:  return prevGreen;
            case 5:  return isDoji;
            case 6:  return isHammer;
            case 7:  return isShootingStar;
            case 8:  return bullEngulf;
            case 9:  return bearEngulf;
            case 10: return isHarami;
            case 11: return piercing;
            case 12: return darkCloud;
            case 13: return directionChange;
            case 14: return rangeVsAvg;
            case 15: return consecSameDir;
            case 16: return trendSlope;
            default: return 0.0;
        }
    }

    static const char* featureName(int idx) {
        static const char* names[] = {
            "body_ratio", "upper_wick", "lower_wick", "is_green", "prev_green",
            "doji", "hammer", "shooting_star", "bull_engulf", "bear_engulf",
            "harami", "piercing", "dark_cloud", "dir_change", "range_vs_avg",
            "consec_dir", "trend_slope"
        };
        return (idx >= 0 && idx < NUM_FEATURES) ? names[idx] : "?";
    }
};

// ============================================================================
// CandleFeatureExtractor
//
// Maintains a rolling window of recent candles and produces a CandleFeatures
// vector for the most recent one. Returns false until enough history exists.
// ============================================================================

class CandleFeatureExtractor {
public:
    bool update(const Candle& candle) {
        history_.push_back(candle);
        if (history_.size() < 3) return false;

        features_ = extract();

        // Keep history bounded.
        if (history_.size() > 60) {
            history_.erase(history_.begin(), history_.begin() + 20);
        }
        return true;
    }

    const CandleFeatures& features() const { return features_; }

    CandleFeatures extract() const {
        CandleFeatures f;
        const Candle& curr = history_.back();
        double rng = curr.range();
        if (rng < 1e-12) rng = 1e-12;

        double body  = curr.bodySize();
        f.bodyRatio      = body / rng;
        f.upperWickRatio = curr.upperWick() / rng;
        f.lowerWickRatio = curr.lowerWick() / rng;
        f.isGreen        = curr.isGreen() ? 1.0 : 0.0;

        // Single-candle shapes (candles.md: Doji, Hammer, Shooting Star).
        f.isDoji         = (f.bodyRatio <= 0.10) ? 1.0 : 0.0;
        f.isHammer       = (f.lowerWickRatio >= 0.5 && f.upperWickRatio <= 0.15 &&
                            f.bodyRatio <= 0.4) ? 1.0 : 0.0;
        f.isShootingStar = (f.upperWickRatio >= 0.5 && f.lowerWickRatio <= 0.15 &&
                            f.bodyRatio <= 0.4) ? 1.0 : 0.0;

        // Two-candle patterns relative to the previous candle.
        if (history_.size() >= 2) {
            const Candle& prev = history_[history_.size() - 2];
            f.prevGreen = prev.isGreen() ? 1.0 : 0.0;
            f.directionChange = (prev.isGreen() != curr.isGreen()) ? 1.0 : 0.0;

            double prevBody = prev.bodySize();
            double prevMid  = (prev.open + prev.close) / 2.0;
            double currTop  = std::max(curr.open, curr.close);
            double currBot  = std::min(curr.open, curr.close);
            double prevTop  = std::max(prev.open, prev.close);
            double prevBot  = std::min(prev.open, prev.close);

            // Engulfing: current body completely covers previous body.
            f.bullEngulf = (prev.isRed() && curr.isGreen() &&
                            curr.open <= prev.close && curr.close >= prev.open) ? 1.0 : 0.0;
            f.bearEngulf = (prev.isGreen() && curr.isRed() &&
                            curr.open >= prev.close && curr.close <= prev.open) ? 1.0 : 0.0;

            // Harami: small body of opposite colour sitting inside a big body.
            f.isHarami = (prev.isGreen() != curr.isGreen() &&
                          prevBody > 1e-12 && body < prevBody * 0.6 &&
                          currTop <= prevTop && currBot >= prevBot) ? 1.0 : 0.0;

            // Piercing line (bullish): red prev, green curr opening below prev
            // close and closing back above the midpoint of the prev body.
            f.piercing = (prev.isRed() && curr.isGreen() &&
                          curr.open < prev.close && curr.close > prevMid &&
                          curr.close < prev.open) ? 1.0 : 0.0;

            // Dark cloud cover (bearish): mirror image of piercing.
            f.darkCloud = (prev.isGreen() && curr.isRed() &&
                           curr.open > prev.close && curr.close < prevMid &&
                           curr.close > prev.open) ? 1.0 : 0.0;
        }

        // Range vs average of last N candles (narrow-range vs expansion).
        double avgRange = 0;
        int lookback = std::min((int)history_.size(), 10);
        for (int i = (int)history_.size() - lookback; i < (int)history_.size(); i++) {
            avgRange += history_[i].range();
        }
        avgRange /= lookback;
        if (avgRange > 1e-12) f.rangeVsAvg = rng / avgRange;

        // Signed consecutive-direction run length: trend persistence.
        int consec = 1;
        bool lastDir = curr.isGreen();
        for (int i = (int)history_.size() - 2; i >= 0; i--) {
            if (history_[i].isGreen() == lastDir) consec++;
            else break;
        }
        f.consecSameDir = lastDir ? (double)consec : -(double)consec;

        // Trend slope: normalised close-to-close momentum over last N candles,
        // giving reversal patterns the trend context they need to be useful.
        int slopeBack = std::min((int)history_.size() - 1, 5);
        if (slopeBack >= 1 && avgRange > 1e-12) {
            double past = history_[history_.size() - 1 - slopeBack].close;
            f.trendSlope = (curr.close - past) / (avgRange * slopeBack);
        }

        return f;
    }

private:
    mutable std::vector<Candle> history_;
    CandleFeatures features_;
};
