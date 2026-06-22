#pragma once
#include "rsi.hpp"
#include "signal_types.hpp"
#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

// ============================================================================
// ConfluenceStrategy
//
// Instead of firing on every RSI threshold crossing, this only signals when
// THREE things line up at once:
//
//   1. A recent swing high/low has been identified as a support/resistance
//      level (confirmed via a simple "highest/lowest of its neighbors"
//      pivot check, with a built-in confirmation delay -- you can't know a
//      point was a swing high until enough later ticks have passed).
//
//   2. Price approaches that level, pulls back away from it by a meaningful
//      margin, then comes back to RETEST it. A single touch doesn't count --
//      this is meant to filter out noise and only react to an actual
//      two-touch pattern.
//
//   3. RSI is in the overbought/oversold zone at the moment of that retest.
//
// Resistance retest + overbought RSI -> Fall signal (expect rejection down).
// Support retest    + oversold RSI   -> Rise signal (expect bounce up).
//
// HONEST CAVEAT: support/resistance is a real-market concept that exists
// because real participants cluster real resting orders at prior highs/lows
// and round numbers. Deriv's synthetic indices are RNG-generated feeds with
// no order book and no participants -- there is no structural reason a
// level computed from past swing points should have any predictive pull on
// where the feed goes next. This is a legitimate thing to *test* against
// real historical data (that's exactly what the backtester is for), but
// don't assume going in that the pattern has to "work" just because it's a
// real technique in real markets.
// ============================================================================

class ConfluenceStrategy {
public:
    struct Config {
        int rsiPeriod = 14;
        double oversold = 30.0;
        double overbought = 70.0;

        int swingLookback = 5;            // ticks each side required to confirm a swing pivot
        double levelTolerancePct = 0.0002; // fraction of price counted as "at" a level (0.0002 = 0.02%)
        double pullbackPct = 0.0005;       // fraction of price that counts as a real pullback (0.05%)
        int maxLevels = 6;                 // levels remembered per side (support/resistance)
        int64_t levelExpirySec = 3600;     // forget a level if untouched for this long
    };

    explicit ConfluenceStrategy(const Config& cfg) : cfg_(cfg), rsi_(cfg.rsiPeriod) {}

    Signal onPrice(int64_t time, double price) {
        auto rsiVal = rsi_.update(price);
        if (rsiVal.has_value()) lastRsi_ = *rsiVal;

        updateSwingDetector(time, price);
        pruneExpiredLevels(time);

        if (!rsiVal.has_value()) return Signal::None;

        if (checkResistanceRetest(price, *rsiVal)) return Signal::Fall;
        if (checkSupportRetest(price, *rsiVal)) return Signal::Rise;
        return Signal::None;
    }

    bool isReady() const { return rsi_.isReady(); }
    double lastRsi() const { return lastRsi_; }

    // Mostly useful for debugging/logging -- how many levels are currently tracked.
    size_t resistanceLevelCount() const { return resistanceLevels_.size(); }
    size_t supportLevelCount() const { return supportLevels_.size(); }

    // Funnel diagnostics: shows exactly where the confluence requirements
    // are filtering trades out, instead of leaving you to guess why a run
    // produced zero (or very few) trades.
    struct Diagnostics {
        int swingHighsFound = 0;
        int swingLowsFound = 0;
        int resistanceApproached = 0;   // first touch of a resistance level
        int resistancePulledBack = 0;   // pulled away far enough to arm a retest
        int resistanceRetested = 0;     // came back to the level a second time
        int resistanceFired = 0;        // retest happened AND RSI was overbought
        int supportApproached = 0;
        int supportPulledBack = 0;
        int supportRetested = 0;
        int supportFired = 0;
    };
    const Diagnostics& diagnostics() const { return diag_; }

private:
    struct Level { double price; int64_t lastSeen; };
    enum class SetupState { Idle, Approached, PulledBack };
    struct Setup {
        SetupState state = SetupState::Idle;
        double levelPrice = 0.0;
        // True if RSI touched the extreme zone at ANY point since this setup
        // started (not just at the literal instant of the retest). RSI is
        // noisy tick-to-tick; demanding it be extreme at one precise tick
        // throws away setups where it was clearly extreme moments earlier in
        // the same approach-pullback-retest sequence.
        bool sawExtreme = false;
    };

    void updateSwingDetector(int64_t time, double price) {
        window_.push_back({time, price});
        int needed = cfg_.swingLookback * 2 + 1;
        if ((int)window_.size() > needed) window_.pop_front();
        if ((int)window_.size() < needed) return;

        int centerIdx = cfg_.swingLookback;
        double centerPrice = window_[centerIdx].second;
        int64_t centerTime = window_[centerIdx].first;

        bool isHigh = true, isLow = true;
        for (int i = 0; i < (int)window_.size(); i++) {
            if (i == centerIdx) continue;
            if (window_[i].second >= centerPrice) isHigh = false;
            if (window_[i].second <= centerPrice) isLow = false;
        }

        if (isHigh) { diag_.swingHighsFound++; addLevel(resistanceLevels_, centerPrice, centerTime); }
        if (isLow)  { diag_.swingLowsFound++;  addLevel(supportLevels_, centerPrice, centerTime); }
    }

    void addLevel(std::vector<Level>& levels, double price, int64_t time) {
        double tol = price * cfg_.levelTolerancePct;
        for (auto& lvl : levels) {
            if (std::abs(lvl.price - price) <= tol) {
                lvl.price = (lvl.price + price) / 2.0; // merge nearby levels into one
                lvl.lastSeen = time;
                return;
            }
        }
        levels.push_back({price, time});
        if ((int)levels.size() > cfg_.maxLevels) {
            levels.erase(levels.begin()); // drop the oldest tracked level
        }
    }

    void pruneExpiredLevels(int64_t now) {
        auto prune = [&](std::vector<Level>& levels) {
            levels.erase(std::remove_if(levels.begin(), levels.end(),
                [&](const Level& l) { return now - l.lastSeen > cfg_.levelExpirySec; }),
                levels.end());
        };
        prune(resistanceLevels_);
        prune(supportLevels_);
    }

    const Level* nearestLevel(const std::vector<Level>& levels, double price) const {
        const Level* best = nullptr;
        double bestDist = 1e18;
        for (auto& l : levels) {
            double d = std::abs(l.price - price);
            if (d < bestDist) { bestDist = d; best = &l; }
        }
        return best;
    }

    bool checkResistanceRetest(double price, double rsiVal) {
        const Level* nearest = nearestLevel(resistanceLevels_, price);
        if (!nearest) { resistanceSetup_ = Setup{}; return false; }

        double tol = nearest->price * cfg_.levelTolerancePct;
        double pullback = nearest->price * cfg_.pullbackPct;
        bool atLevel = std::abs(price - nearest->price) <= tol;
        bool pulledAway = (nearest->price - price) >= pullback;
        bool rsiExtreme = rsiVal >= cfg_.overbought;

        switch (resistanceSetup_.state) {
            case SetupState::Idle:
                if (atLevel) {
                    resistanceSetup_ = {SetupState::Approached, nearest->price, rsiExtreme};
                    diag_.resistanceApproached++;
                }
                return false;
            case SetupState::Approached:
                resistanceSetup_.sawExtreme = resistanceSetup_.sawExtreme || rsiExtreme;
                if (pulledAway) {
                    resistanceSetup_.state = SetupState::PulledBack;
                    diag_.resistancePulledBack++;
                }
                else if (price > nearest->price + tol) resistanceSetup_ = Setup{}; // broke through, invalidate
                return false;
            case SetupState::PulledBack:
                resistanceSetup_.sawExtreme = resistanceSetup_.sawExtreme || rsiExtreme;
                if (atLevel) {
                    diag_.resistanceRetested++;
                    bool fire = resistanceSetup_.sawExtreme;
                    if (fire) diag_.resistanceFired++;
                    resistanceSetup_ = Setup{}; // consume the setup whether it fired or not
                    return fire;
                }
                if (price > resistanceSetup_.levelPrice + tol * 2) resistanceSetup_ = Setup{};
                return false;
        }
        return false;
    }

    bool checkSupportRetest(double price, double rsiVal) {
        const Level* nearest = nearestLevel(supportLevels_, price);
        if (!nearest) { supportSetup_ = Setup{}; return false; }

        double tol = nearest->price * cfg_.levelTolerancePct;
        double pullback = nearest->price * cfg_.pullbackPct;
        bool atLevel = std::abs(price - nearest->price) <= tol;
        bool pulledAway = (price - nearest->price) >= pullback;
        bool rsiExtreme = rsiVal <= cfg_.oversold;

        switch (supportSetup_.state) {
            case SetupState::Idle:
                if (atLevel) {
                    supportSetup_ = {SetupState::Approached, nearest->price, rsiExtreme};
                    diag_.supportApproached++;
                }
                return false;
            case SetupState::Approached:
                supportSetup_.sawExtreme = supportSetup_.sawExtreme || rsiExtreme;
                if (pulledAway) {
                    supportSetup_.state = SetupState::PulledBack;
                    diag_.supportPulledBack++;
                }
                else if (price < nearest->price - tol) supportSetup_ = Setup{};
                return false;
            case SetupState::PulledBack:
                supportSetup_.sawExtreme = supportSetup_.sawExtreme || rsiExtreme;
                if (atLevel) {
                    diag_.supportRetested++;
                    bool fire = supportSetup_.sawExtreme;
                    if (fire) diag_.supportFired++;
                    supportSetup_ = Setup{};
                    return fire;
                }
                if (price < supportSetup_.levelPrice - tol * 2) supportSetup_ = Setup{};
                return false;
        }
        return false;
    }

    Config cfg_;
    RSI rsi_;
    double lastRsi_ = 50.0;
    Diagnostics diag_;

    std::deque<std::pair<int64_t, double>> window_;
    std::vector<Level> resistanceLevels_;
    std::vector<Level> supportLevels_;
    Setup resistanceSetup_;
    Setup supportSetup_;
};