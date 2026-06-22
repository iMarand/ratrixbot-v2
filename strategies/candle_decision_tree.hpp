#pragma once
#include "strategy_base.hpp"
#include "../indicators/ohlc_builder.hpp"
#include "../indicators/candle_patterns.hpp"
#include <sstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <memory>
#include <array>

// ============================================================================
// Candle Decision Tree Strategy
//
// A lightweight decision tree that classifies Rise/Fall based on
// features extracted from the most recent candles:
//   - body_ratio:          bodySize / range  (Doji if < 0.1)
//   - upper_wick_ratio:    upperWick / range (Shooting Star if > 0.6)
//   - lower_wick_ratio:    lowerWick / range (Hammer if > 0.6)
//   - is_engulfing:        current body engulfs previous body
//   - direction_change:    candle color flipped from previous
//   - range_vs_avg:        current range / avg range of last N candles
//   - consec_same_dir:     consecutive candles in same direction
//
// The tree is built via information-gain (ID3) splits on these features.
// During live/backtest, ticks are aggregated into candles and fed through
// the tree to produce a signal.
// ============================================================================

struct CandleFeatures {
    double bodyRatio       = 0.5;   // [0, 1]
    double upperWickRatio  = 0.0;   // [0, 1]
    double lowerWickRatio  = 0.0;   // [0, 1]
    double isEngulfing     = 0.0;   // 0 or 1
    double directionChange = 0.0;   // 0 or 1
    double rangeVsAvg      = 1.0;   // ratio
    double consecSameDir   = 0.0;   // count

    static constexpr int NUM_FEATURES = 7;

    double get(int idx) const {
        switch (idx) {
            case 0: return bodyRatio;
            case 1: return upperWickRatio;
            case 2: return lowerWickRatio;
            case 3: return isEngulfing;
            case 4: return directionChange;
            case 5: return rangeVsAvg;
            case 6: return consecSameDir;
            default: return 0.0;
        }
    }

    static const char* featureName(int idx) {
        static const char* names[] = {
            "body_ratio", "upper_wick", "lower_wick",
            "engulfing", "dir_change", "range_vs_avg", "consec_dir"
        };
        return (idx >= 0 && idx < NUM_FEATURES) ? names[idx] : "?";
    }
};

// ============================================================================
// Decision Tree Node
// ============================================================================

struct DTNode {
    bool isLeaf = false;
    Signal prediction = Signal::None;  // only if isLeaf

    int    featureIdx = -1;  // which feature to split on
    double threshold  = 0.0; // split threshold
    std::unique_ptr<DTNode> left;   // feature <= threshold
    std::unique_ptr<DTNode> right;  // feature > threshold
};

// ============================================================================
// ID3 Tree Builder
// ============================================================================

class DecisionTreeBuilder {
public:
    struct Sample {
        CandleFeatures features;
        Signal label;  // Rise or Fall
    };

    // Build a decision tree from labeled samples
    static std::unique_ptr<DTNode> build(const std::vector<Sample>& data,
                                          int maxDepth, int minSamples) {
        return buildRecursive(data, 0, maxDepth, minSamples);
    }

private:
    static double entropy(int pos, int neg) {
        if (pos == 0 || neg == 0) return 0.0;
        int total = pos + neg;
        double pPos = (double)pos / total;
        double pNeg = (double)neg / total;
        return -pPos * std::log2(pPos) - pNeg * std::log2(pNeg);
    }

    static std::unique_ptr<DTNode> buildRecursive(const std::vector<Sample>& data,
                                                    int depth, int maxDepth, int minSamples) {
        auto node = std::make_unique<DTNode>();

        // Count rise vs fall
        int rises = 0, falls = 0;
        for (const auto& s : data) {
            if (s.label == Signal::Rise) rises++;
            else falls++;
        }

        // Leaf conditions
        if (depth >= maxDepth || (int)data.size() < minSamples ||
            rises == 0 || falls == 0) {
            node->isLeaf = true;
            node->prediction = (rises >= falls) ? Signal::Rise : Signal::Fall;
            return node;
        }

        // Find best split
        double parentEntropy = entropy(rises, falls);
        double bestGain = -1.0;
        int bestFeature = 0;
        double bestThreshold = 0.0;

        for (int f = 0; f < CandleFeatures::NUM_FEATURES; f++) {
            // Collect unique values for this feature
            std::vector<double> vals;
            vals.reserve(data.size());
            for (const auto& s : data) vals.push_back(s.features.get(f));
            std::sort(vals.begin(), vals.end());

            // Try midpoints between consecutive unique values
            for (size_t i = 1; i < vals.size(); i++) {
                if (vals[i] == vals[i-1]) continue;
                double thresh = (vals[i-1] + vals[i]) / 2.0;

                int leftR = 0, leftF = 0, rightR = 0, rightF = 0;
                for (const auto& s : data) {
                    if (s.features.get(f) <= thresh) {
                        if (s.label == Signal::Rise) leftR++; else leftF++;
                    } else {
                        if (s.label == Signal::Rise) rightR++; else rightF++;
                    }
                }

                int leftTotal = leftR + leftF;
                int rightTotal = rightR + rightF;
                if (leftTotal < 2 || rightTotal < 2) continue;

                double leftEnt = entropy(leftR, leftF);
                double rightEnt = entropy(rightR, rightF);
                double weightedEnt = ((double)leftTotal / data.size()) * leftEnt +
                                     ((double)rightTotal / data.size()) * rightEnt;
                double gain = parentEntropy - weightedEnt;

                if (gain > bestGain) {
                    bestGain = gain;
                    bestFeature = f;
                    bestThreshold = thresh;
                }
            }
        }

        if (bestGain <= 0.001) {
            // No meaningful split found
            node->isLeaf = true;
            node->prediction = (rises >= falls) ? Signal::Rise : Signal::Fall;
            return node;
        }

        // Split
        node->featureIdx = bestFeature;
        node->threshold = bestThreshold;

        std::vector<Sample> leftData, rightData;
        for (const auto& s : data) {
            if (s.features.get(bestFeature) <= bestThreshold)
                leftData.push_back(s);
            else
                rightData.push_back(s);
        }

        node->left = buildRecursive(leftData, depth + 1, maxDepth, minSamples);
        node->right = buildRecursive(rightData, depth + 1, maxDepth, minSamples);

        return node;
    }
};

// ============================================================================
// Candle Feature Extractor
// ============================================================================

class CandleFeatureExtractor {
public:
    // Returns features when a new candle is available, plus its "actual outcome"
    // label for training (did price go up or down after this candle?)
    bool update(const Candle& candle) {
        history_.push_back(candle);
        if (history_.size() < 3) return false;

        features_ = extract();
        return true;
    }

    const CandleFeatures& features() const { return features_; }

    CandleFeatures extract() const {
        CandleFeatures f;
        const Candle& curr = history_.back();
        double rng = curr.range();
        if (rng < 1e-12) rng = 1e-12;

        f.bodyRatio = curr.bodySize() / rng;
        f.upperWickRatio = curr.upperWick() / rng;
        f.lowerWickRatio = curr.lowerWick() / rng;

        // Engulfing check against previous candle
        if (history_.size() >= 2) {
            const Candle& prev = history_[history_.size() - 2];
            bool bullEngulf = prev.isRed() && curr.isGreen() &&
                              curr.open <= prev.close && curr.close >= prev.open;
            bool bearEngulf = prev.isGreen() && curr.isRed() &&
                              curr.open >= prev.close && curr.close <= prev.open;
            f.isEngulfing = (bullEngulf || bearEngulf) ? 1.0 : 0.0;
            f.directionChange = (prev.isGreen() != curr.isGreen()) ? 1.0 : 0.0;
        }

        // Range vs average
        double avgRange = 0;
        int lookback = std::min((int)history_.size(), 10);
        for (int i = (int)history_.size() - lookback; i < (int)history_.size(); i++) {
            avgRange += history_[i].range();
        }
        avgRange /= lookback;
        if (avgRange > 1e-12) f.rangeVsAvg = rng / avgRange;

        // Consecutive same direction
        int consec = 0;
        bool lastDir = curr.isGreen();
        for (int i = (int)history_.size() - 2; i >= 0; i--) {
            if (history_[i].isGreen() == lastDir) consec++;
            else break;
        }
        f.consecSameDir = (double)consec;

        // Keep history bounded
        if (history_.size() > 50) {
            history_.erase(history_.begin(), history_.begin() + 20);
        }

        return f;
    }

private:
    mutable std::vector<Candle> history_;
    CandleFeatures features_;
};

// ============================================================================
// CandleDecisionTreeStrategy
//
// The strategy builds a decision tree from the first portion of price data
// (self-training), then uses it to generate signals.
// For grid search, the tree is pre-built by the factory with training data
// injected through a static thread_local pointer.
// ============================================================================

class CandleDecisionTreeStrategy : public StrategyBase {
public:
    CandleDecisionTreeStrategy(int candlePeriod, int maxDepth, int minSamples,
                                int tradeDuration = 15)
        : ohlc_(candlePeriod), period_(candlePeriod),
          maxDepth_(maxDepth), minSamples_(minSamples),
          tradeDuration_(tradeDuration) {}

    // Set an externally-built tree (used by grid search)
    void setTree(std::unique_ptr<DTNode> tree) {
        tree_ = std::move(tree);
        treeReady_ = true;
    }

    Signal onPrice(int64_t time, double price) override {
        auto candleOpt = ohlc_.update(time, price);

        if (!candleOpt.has_value()) return Signal::None;

        if (!treeReady_) {
            // Accumulate candles for self-training
            trainCandles_.push_back(*candleOpt);
            trainPrices_.push_back(price);
            trainTimes_.push_back(time);

            if (trainCandles_.size() >= 100) {
                buildTreeFromHistory();
            }
            return Signal::None;
        }

        // Extract features and traverse tree
        if (!featureExtractor_.update(*candleOpt)) return Signal::None;
        return traverseTree(featureExtractor_.features());
    }

    std::string name() const override { return "candle_tree"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "candle_period=" << period_
           << " max_depth=" << maxDepth_
           << " min_samples=" << minSamples_;
        return ss.str();
    }

private:
    OHLCBuilder ohlc_;
    CandleFeatureExtractor featureExtractor_;
    int period_;
    int maxDepth_;
    int minSamples_;
    int tradeDuration_;

    std::unique_ptr<DTNode> tree_;
    bool treeReady_ = false;

    // Self-training data
    std::vector<Candle> trainCandles_;
    std::vector<double> trainPrices_;
    std::vector<int64_t> trainTimes_;

    Signal traverseTree(const CandleFeatures& f) const {
        if (!tree_) return Signal::None;
        const DTNode* node = tree_.get();
        while (!node->isLeaf) {
            if (f.get(node->featureIdx) <= node->threshold)
                node = node->left.get();
            else
                node = node->right.get();
            if (!node) return Signal::None;
        }
        return node->prediction;
    }

    void buildTreeFromHistory() {
        // Build labeled samples: for each candle, label = did next candle go up?
        std::vector<DecisionTreeBuilder::Sample> samples;
        CandleFeatureExtractor ext;

        for (size_t i = 0; i + 1 < trainCandles_.size(); i++) {
            if (!ext.update(trainCandles_[i])) continue;

            // Label: did price go up or down over the trade duration?
            int64_t entryTime = trainCandles_[i].time;
            int64_t exitTime = entryTime + tradeDuration_;

            // Find the candle closest to exit time
            bool priceUp = false;
            for (size_t j = i + 1; j < trainCandles_.size(); j++) {
                if (trainCandles_[j].time >= exitTime) {
                    priceUp = trainCandles_[j].close > trainCandles_[i].close;
                    break;
                }
            }

            DecisionTreeBuilder::Sample s;
            s.features = ext.features();
            s.label = priceUp ? Signal::Rise : Signal::Fall;
            samples.push_back(s);
        }

        if (samples.size() >= 20) {
            tree_ = DecisionTreeBuilder::build(samples, maxDepth_, minSamples_);
            treeReady_ = true;
        }

        // Clear training data
        trainCandles_.clear();
        trainPrices_.clear();
        trainTimes_.clear();
    }
};
