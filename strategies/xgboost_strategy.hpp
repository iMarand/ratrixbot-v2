#pragma once
#include "strategy_base.hpp"
#include "ml_features.hpp"
#include "../indicators/ohlc_builder.hpp"
#include <vector>
#include <memory>
#include <sstream>
#include <cmath>
#include <algorithm>

// ============================================================================
// XGBoost Strategy (gradient-boosted trees, from scratch)
//
// A compact implementation of gradient boosting on the logistic loss, using
// the same regularised-gain split criterion and leaf-weight formula as
// XGBoost:
//
//   leaf weight   w* = -G / (H + lambda)
//   split gain     = 1/2 [ G_L^2/(H_L+l) + G_R^2/(H_R+l) - G^2/(H+l) ] - gamma
//
// where G/H are the sums of first/second-order gradients of the logistic loss
// (g = p - y, h = p(1-p)). Predictions are additive over the trees, squashed
// through a sigmoid into a probability, and only turned into a trade when the
// probability is far enough from 0.5 (prob_margin) -- a confidence filter.
//
// Self-training mirrors the other learning strategies: accumulate the first
// chunk of candles, fit the booster once, then predict from then on.
// ============================================================================

namespace xgb {

struct Node {
    bool   isLeaf    = false;
    double leafValue = 0.0;
    int    featureIdx = -1;
    double threshold  = 0.0;
    std::unique_ptr<Node> left;   // feature <= threshold
    std::unique_ptr<Node> right;  // feature > threshold
};

struct TrainData {
    std::vector<CandleFeatures> X;
    std::vector<double> y;     // 0/1 labels
    std::vector<double> raw;   // current additive raw score per sample
    std::vector<double> g;     // first-order gradient
    std::vector<double> h;     // second-order gradient (hessian)
};

inline double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

// Recursively build one regression tree over the sample indices in `idx`.
inline std::unique_ptr<Node> buildTree(const TrainData& d,
                                       const std::vector<int>& idx,
                                       int depth, int maxDepth,
                                       double lambda, double gamma,
                                       double minChildWeight) {
    auto node = std::make_unique<Node>();

    double G = 0.0, H = 0.0;
    for (int i : idx) { G += d.g[i]; H += d.h[i]; }

    auto leafWeight = [&](double g, double h) { return -g / (h + lambda); };

    if (depth >= maxDepth || (int)idx.size() < 2) {
        node->isLeaf = true;
        node->leafValue = leafWeight(G, H);
        return node;
    }

    double bestGain = 0.0;
    int    bestFeature = -1;
    double bestThreshold = 0.0;
    double rootScore = (G * G) / (H + lambda);

    for (int f = 0; f < CandleFeatures::NUM_FEATURES; f++) {
        // Sort sample indices by this feature value.
        std::vector<int> order(idx);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return d.X[a].get(f) < d.X[b].get(f);
        });

        double GL = 0.0, HL = 0.0;
        for (size_t s = 0; s + 1 < order.size(); s++) {
            int cur = order[s];
            GL += d.g[cur];
            HL += d.h[cur];
            // Only split between distinct feature values.
            double vCur  = d.X[order[s]].get(f);
            double vNext = d.X[order[s + 1]].get(f);
            if (vCur == vNext) continue;

            double GR = G - GL, HR = H - HL;
            if (HL < minChildWeight || HR < minChildWeight) continue;

            double gain = 0.5 * ((GL * GL) / (HL + lambda) +
                                 (GR * GR) / (HR + lambda) - rootScore) - gamma;
            if (gain > bestGain) {
                bestGain = gain;
                bestFeature = f;
                bestThreshold = (vCur + vNext) / 2.0;
            }
        }
    }

    if (bestFeature < 0 || bestGain <= 0.0) {
        node->isLeaf = true;
        node->leafValue = leafWeight(G, H);
        return node;
    }

    node->featureIdx = bestFeature;
    node->threshold = bestThreshold;

    std::vector<int> leftIdx, rightIdx;
    for (int i : idx) {
        if (d.X[i].get(bestFeature) <= bestThreshold) leftIdx.push_back(i);
        else rightIdx.push_back(i);
    }

    node->left  = buildTree(d, leftIdx,  depth + 1, maxDepth, lambda, gamma, minChildWeight);
    node->right = buildTree(d, rightIdx, depth + 1, maxDepth, lambda, gamma, minChildWeight);
    return node;
}

inline double predictTree(const Node* node, const CandleFeatures& f) {
    while (node && !node->isLeaf) {
        node = (f.get(node->featureIdx) <= node->threshold) ? node->left.get()
                                                            : node->right.get();
    }
    return node ? node->leafValue : 0.0;
}

} // namespace xgb

class XGBoostStrategy : public StrategyBase {
public:
    XGBoostStrategy(int candlePeriod, int nRounds, int maxDepth,
                    double learningRate, double probMargin, int tradeDuration = 15,
                    int cooldownCandles = 2)
        : ohlc_(candlePeriod), period_(candlePeriod), nRounds_(nRounds),
          maxDepth_(maxDepth), learningRate_(learningRate),
          probMargin_(probMargin), tradeDuration_(tradeDuration),
          cooldownCandles_(cooldownCandles) {}

    Signal onPrice(int64_t time, double price) override {
        auto candleOpt = ohlc_.update(time, price);
        if (!candleOpt.has_value()) return Signal::None;

        if (!ready_) {
            trainCandles_.push_back(*candleOpt);
            if ((int)trainCandles_.size() >= kMinTrainCandles) trainBooster();
            return Signal::None;
        }

        if (!extractor_.update(*candleOpt)) return Signal::None;

        // Human-like spacing between trades (see RandomForestStrategy): avoids
        // firing several correlated trades on one move and the loss streaks
        // that come with it.
        sinceSignal_++;
        Signal sig = predict(extractor_.features());
        if (sig == Signal::None) return Signal::None;
        if (sinceSignal_ < cooldownCandles_) return Signal::None;
        sinceSignal_ = 0;
        return sig;
    }

    std::string name() const override { return "xgboost"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "candle_period=" << period_ << " rounds=" << nRounds_
           << " max_depth=" << maxDepth_ << " lr=" << learningRate_
           << " margin=" << probMargin_;
        return ss.str();
    }

private:
    static constexpr int kMinTrainCandles = 300;

    OHLCBuilder ohlc_;
    CandleFeatureExtractor extractor_;
    int    period_;
    int    nRounds_;
    int    maxDepth_;
    double learningRate_;
    double probMargin_;
    int    tradeDuration_;
    int    cooldownCandles_;
    int    sinceSignal_ = 1000000; // large so the first valid signal can fire

    std::vector<Candle> trainCandles_;
    std::vector<std::unique_ptr<xgb::Node>> trees_;
    double baseScore_ = 0.0;
    bool ready_ = false;

    void trainBooster() {
        xgb::TrainData d;
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

            d.X.push_back(ext.features());
            d.y.push_back(priceUp ? 1.0 : 0.0);
        }

        size_t n = d.X.size();
        if (n < 20) { trainCandles_.clear(); return; }

        // Initialise base score to the log-odds of the positive class.
        double posRate = 0.0;
        for (double v : d.y) posRate += v;
        posRate /= n;
        posRate = std::min(0.999, std::max(0.001, posRate));
        baseScore_ = std::log(posRate / (1.0 - posRate));

        d.raw.assign(n, baseScore_);
        d.g.assign(n, 0.0);
        d.h.assign(n, 0.0);

        const double lambda = 1.0, gamma = 0.0, minChildWeight = 1.0;
        std::vector<int> allIdx(n);
        for (size_t i = 0; i < n; i++) allIdx[i] = (int)i;

        trees_.clear();
        for (int r = 0; r < nRounds_; r++) {
            for (size_t i = 0; i < n; i++) {
                double p = xgb::sigmoid(d.raw[i]);
                d.g[i] = p - d.y[i];
                d.h[i] = std::max(1e-6, p * (1.0 - p));
            }
            auto tree = xgb::buildTree(d, allIdx, 0, maxDepth_, lambda, gamma, minChildWeight);
            for (size_t i = 0; i < n; i++) {
                d.raw[i] += learningRate_ * xgb::predictTree(tree.get(), d.X[i]);
            }
            trees_.push_back(std::move(tree));
        }

        ready_ = true;
        trainCandles_.clear();
    }

    Signal predict(const CandleFeatures& f) const {
        if (trees_.empty()) return Signal::None;
        double raw = baseScore_;
        for (const auto& t : trees_) raw += learningRate_ * xgb::predictTree(t.get(), f);
        double p = xgb::sigmoid(raw);
        if (p >= 0.5 + probMargin_) return Signal::Rise;
        if (p <= 0.5 - probMargin_) return Signal::Fall;
        return Signal::None;
    }
};
