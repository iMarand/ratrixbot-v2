#pragma once
#include "strategy_base.hpp"
#include "ml_features.hpp"
#include "../indicators/ohlc_builder.hpp"
#include <vector>
#include <memory>
#include <random>
#include <sstream>
#include <cmath>
#include <algorithm>

// ============================================================================
// Random Forest Strategy
//
// A from-scratch bagged ensemble of classification trees (no external ML lib).
// Each tree is trained on a bootstrap resample of the candle-feature samples
// and, at every split, only considers a random subset of features -- the two
// ingredients that decorrelate the trees and give a random forest its edge
// over a single decision tree.
//
// Self-training: like the other learning strategies, it accumulates the first
// chunk of candles, builds the forest once, then predicts on every candle
// after that. A signal is only emitted when the share of trees agreeing on a
// direction meets `voteThreshold` -- a built-in confidence filter that cuts
// down low-conviction (loss-streak-prone) trades.
// ============================================================================

namespace rf {

struct Sample {
    CandleFeatures features;
    int label; // 1 = Rise, 0 = Fall
};

struct Node {
    bool   isLeaf     = false;
    int    prediction = 1;      // majority class at a leaf (1=Rise, 0=Fall)
    int    featureIdx = -1;
    double threshold  = 0.0;
    std::unique_ptr<Node> left;   // feature <= threshold
    std::unique_ptr<Node> right;  // feature > threshold
};

inline double gini(int pos, int neg) {
    int total = pos + neg;
    if (total == 0) return 0.0;
    double p = (double)pos / total;
    double q = (double)neg / total;
    return 1.0 - (p * p + q * q);
}

// Build one tree on the given (already-bootstrapped) sample set, considering a
// random subset of `featureSubset` features at each split.
inline std::unique_ptr<Node> buildTree(const std::vector<Sample>& data,
                                       int depth, int maxDepth, int minSamples,
                                       int featureSubset, std::mt19937& rng) {
    auto node = std::make_unique<Node>();

    int pos = 0, neg = 0;
    for (const auto& s : data) (s.label == 1 ? pos : neg)++;

    if (depth >= maxDepth || (int)data.size() < minSamples || pos == 0 || neg == 0) {
        node->isLeaf = true;
        node->prediction = (pos >= neg) ? 1 : 0;
        return node;
    }

    double parentGini = gini(pos, neg);
    double bestGain = 0.0;
    int    bestFeature = -1;
    double bestThreshold = 0.0;

    // Pick a random subset of feature indices to consider at this node.
    std::vector<int> feats(CandleFeatures::NUM_FEATURES);
    for (int i = 0; i < CandleFeatures::NUM_FEATURES; i++) feats[i] = i;
    std::shuffle(feats.begin(), feats.end(), rng);
    int k = std::min(featureSubset, CandleFeatures::NUM_FEATURES);

    for (int fi = 0; fi < k; fi++) {
        int f = feats[fi];
        std::vector<double> vals;
        vals.reserve(data.size());
        for (const auto& s : data) vals.push_back(s.features.get(f));
        std::sort(vals.begin(), vals.end());

        for (size_t i = 1; i < vals.size(); i++) {
            if (vals[i] == vals[i - 1]) continue;
            double thresh = (vals[i - 1] + vals[i]) / 2.0;

            int lp = 0, ln = 0, rp = 0, rn = 0;
            for (const auto& s : data) {
                if (s.features.get(f) <= thresh) { (s.label == 1 ? lp : ln)++; }
                else                              { (s.label == 1 ? rp : rn)++; }
            }
            int lt = lp + ln, rt = rp + rn;
            if (lt < 1 || rt < 1) continue;

            double weighted = ((double)lt / data.size()) * gini(lp, ln) +
                              ((double)rt / data.size()) * gini(rp, rn);
            double gain = parentGini - weighted;
            if (gain > bestGain) {
                bestGain = gain;
                bestFeature = f;
                bestThreshold = thresh;
            }
        }
    }

    if (bestFeature < 0 || bestGain <= 1e-6) {
        node->isLeaf = true;
        node->prediction = (pos >= neg) ? 1 : 0;
        return node;
    }

    node->featureIdx = bestFeature;
    node->threshold = bestThreshold;

    std::vector<Sample> leftData, rightData;
    for (const auto& s : data) {
        if (s.features.get(bestFeature) <= bestThreshold) leftData.push_back(s);
        else rightData.push_back(s);
    }

    node->left  = buildTree(leftData,  depth + 1, maxDepth, minSamples, featureSubset, rng);
    node->right = buildTree(rightData, depth + 1, maxDepth, minSamples, featureSubset, rng);
    return node;
}

inline int predictTree(const Node* node, const CandleFeatures& f) {
    while (node && !node->isLeaf) {
        node = (f.get(node->featureIdx) <= node->threshold) ? node->left.get()
                                                            : node->right.get();
    }
    return node ? node->prediction : 1;
}

} // namespace rf

class RandomForestStrategy : public StrategyBase {
public:
    RandomForestStrategy(int candlePeriod, int numTrees, int maxDepth,
                         int minSamples, double voteThreshold, int tradeDuration = 15,
                         int cooldownCandles = 2)
        : ohlc_(candlePeriod), period_(candlePeriod), numTrees_(numTrees),
          maxDepth_(maxDepth), minSamples_(minSamples),
          voteThreshold_(voteThreshold), tradeDuration_(tradeDuration),
          cooldownCandles_(cooldownCandles) {}

    Signal onPrice(int64_t time, double price) override {
        auto candleOpt = ohlc_.update(time, price);
        if (!candleOpt.has_value()) return Signal::None;

        if (!ready_) {
            trainCandles_.push_back(*candleOpt);
            if ((int)trainCandles_.size() >= kMinTrainCandles) buildForest();
            return Signal::None;
        }

        if (!extractor_.update(*candleOpt)) return Signal::None;

        // Human-like spacing: don't take a new trade for a few candles after
        // the last one. Clustered entries on the same wiggle tend to win or
        // lose together, which is what produces long consecutive-loss streaks.
        sinceSignal_++;
        Signal sig = predict(extractor_.features());
        if (sig == Signal::None) return Signal::None;
        if (sinceSignal_ < cooldownCandles_) return Signal::None;
        sinceSignal_ = 0;
        return sig;
    }

    std::string name() const override { return "random_forest"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "candle_period=" << period_ << " trees=" << numTrees_
           << " max_depth=" << maxDepth_ << " min_samples=" << minSamples_
           << " vote=" << voteThreshold_;
        return ss.str();
    }

private:
    static constexpr int kMinTrainCandles = 300;

    OHLCBuilder ohlc_;
    CandleFeatureExtractor extractor_;
    int    period_;
    int    numTrees_;
    int    maxDepth_;
    int    minSamples_;
    double voteThreshold_;
    int    tradeDuration_;
    int    cooldownCandles_;
    int    sinceSignal_ = 1000000; // large so the first valid signal can fire

    std::vector<Candle> trainCandles_;
    std::vector<std::unique_ptr<rf::Node>> forest_;
    bool ready_ = false;

    void buildForest() {
        // Label each candle by whether price closed higher `tradeDuration`
        // seconds later, then bag the resulting feature samples into trees.
        std::vector<rf::Sample> samples;
        CandleFeatureExtractor ext;
        for (size_t i = 0; i + 1 < trainCandles_.size(); i++) {
            if (!ext.update(trainCandles_[i])) continue;

            int64_t exitTime = trainCandles_[i].time + tradeDuration_;
            bool labelled = false, priceUp = false;
            for (size_t j = i + 1; j < trainCandles_.size(); j++) {
                if (trainCandles_[j].time >= exitTime) {
                    priceUp = trainCandles_[j].close > trainCandles_[i].close;
                    labelled = true;
                    break;
                }
            }
            if (!labelled) continue;

            rf::Sample s;
            s.features = ext.features();
            s.label = priceUp ? 1 : 0;
            samples.push_back(s);
        }

        if (samples.size() < 20) {
            trainCandles_.clear();
            return; // not enough to learn from; stays inactive -> no trades
        }

        std::mt19937 rng(1234567u);
        int featureSubset = std::max(1, (int)std::round(std::sqrt((double)CandleFeatures::NUM_FEATURES)));
        std::uniform_int_distribution<size_t> pick(0, samples.size() - 1);

        forest_.clear();
        for (int t = 0; t < numTrees_; t++) {
            std::vector<rf::Sample> boot;
            boot.reserve(samples.size());
            for (size_t i = 0; i < samples.size(); i++) boot.push_back(samples[pick(rng)]);
            forest_.push_back(rf::buildTree(boot, 0, maxDepth_, minSamples_, featureSubset, rng));
        }

        ready_ = true;
        trainCandles_.clear();
    }

    Signal predict(const CandleFeatures& f) const {
        if (forest_.empty()) return Signal::None;
        int riseVotes = 0;
        for (const auto& tree : forest_) riseVotes += rf::predictTree(tree.get(), f);
        double riseFrac = (double)riseVotes / forest_.size();
        if (riseFrac >= voteThreshold_) return Signal::Rise;
        if ((1.0 - riseFrac) >= voteThreshold_) return Signal::Fall;
        return Signal::None;
    }
};
