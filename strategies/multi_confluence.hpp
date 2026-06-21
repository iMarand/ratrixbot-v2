#pragma once
#include "strategy_base.hpp"
#include "../rsi.hpp"
#include "../indicators/ema.hpp"
#include "../indicators/macd.hpp"
#include "../indicators/bollinger.hpp"
#include "../indicators/stochastic.hpp"
#include "../indicators/adx.hpp"
#include <sstream>
#include <bitset>

// ============================================================================
// Multi-Indicator Confluence Strategy
//
// The powerhouse: requires N out of M enabled indicators to agree before
// firing a signal. Each indicator votes Rise/Fall/Abstain on each tick;
// if at least `minAgreement` indicators agree on a direction, fire it.
//
// Which indicators to enable is controlled by a bitmask:
//   bit 0: RSI (oversold/overbought)
//   bit 1: EMA crossover
//   bit 2: MACD histogram sign
//   bit 3: Bollinger band touch
//   bit 4: Stochastic crossover
//   bit 5: ADX filter (only allows mean-reversion signals when ADX < 25)
//
// The ADX filter is special: it doesn't vote, it vetoes. If enabled and
// ADX > adxThreshold, the entire signal is suppressed (trending markets
// make mean-reversion strategies lose money).
// ============================================================================

class MultiConfluenceStrategy : public StrategyBase {
public:
    struct Config {
        // Which indicators are active (bitmask)
        uint32_t enabledMask = 0x1F;   // all 5 voting indicators enabled by default

        // Minimum number of enabled indicators that must agree
        int minAgreement = 3;

        // RSI params
        int rsiPeriod = 14;
        double rsiOversold  = 30.0;
        double rsiOverbought = 70.0;

        // EMA params
        int emaFast = 10;
        int emaSlow = 30;

        // MACD params
        int macdFast   = 12;
        int macdSlow   = 26;
        int macdSignal = 9;

        // Bollinger params
        int    bbPeriod = 20;
        double bbStdDev = 2.0;

        // Stochastic params
        int    stochK = 14;
        int    stochD = 3;
        double stochOversold  = 20.0;
        double stochOverbought = 80.0;

        // ADX filter params
        int    adxPeriod = 14;
        double adxThreshold = 25.0;   // suppress signals when ADX > this
        bool   adxFilterEnabled = true;
    };

    explicit MultiConfluenceStrategy(const Config& cfg)
        : cfg_(cfg),
          rsi_(cfg.rsiPeriod),
          emaFast_(cfg.emaFast), emaSlow_(cfg.emaSlow),
          macd_(cfg.macdFast, cfg.macdSlow, cfg.macdSignal),
          bb_(cfg.bbPeriod, cfg.bbStdDev),
          stoch_(cfg.stochK, cfg.stochD),
          adx_(cfg.adxPeriod) {}

    Signal onPrice(int64_t /*time*/, double price) override {
        // Update all indicators
        auto rsiVal   = rsi_.update(price);
        auto emaF     = emaFast_.update(price);
        auto emaS     = emaSlow_.update(price);
        auto macdOut  = macd_.update(price);
        auto bbOut    = bb_.update(price);
        auto stochOut = stoch_.update(price);
        auto adxVal   = adx_.update(price);

        if (rsiVal.has_value()) lastRsi_ = *rsiVal;

        int riseVotes = 0, fallVotes = 0;
        int activeIndicators = 0;

        // --- RSI vote (bit 0) ---
        if ((cfg_.enabledMask & 0x01) && rsiVal.has_value()) {
            activeIndicators++;
            if (*rsiVal <= cfg_.rsiOversold)  riseVotes++;
            if (*rsiVal >= cfg_.rsiOverbought) fallVotes++;
        }

        // --- EMA crossover vote (bit 1) ---
        if ((cfg_.enabledMask & 0x02) && emaF.has_value() && emaS.has_value()) {
            activeIndicators++;
            if (*emaF > *emaS) riseVotes++;    // fast above slow = bullish
            if (*emaF < *emaS) fallVotes++;    // fast below slow = bearish
        }

        // --- MACD vote (bit 2) ---
        if ((cfg_.enabledMask & 0x04) && macdOut.has_value()) {
            activeIndicators++;
            if (macdOut->histogram > 0) riseVotes++;
            if (macdOut->histogram < 0) fallVotes++;
        }

        // --- Bollinger vote (bit 3) ---
        if ((cfg_.enabledMask & 0x08) && bbOut.has_value()) {
            activeIndicators++;
            if (price <= bbOut->lower) riseVotes++;   // at lower band → bounce up
            if (price >= bbOut->upper) fallVotes++;   // at upper band → reject down
        }

        // --- Stochastic vote (bit 4) ---
        if ((cfg_.enabledMask & 0x10) && stochOut.has_value()) {
            activeIndicators++;
            if (stochOut->k < cfg_.stochOversold)  riseVotes++;
            if (stochOut->k > cfg_.stochOverbought) fallVotes++;
        }

        // --- ADX filter (bit 5) — vetoes, doesn't vote ---
        if (cfg_.adxFilterEnabled && adxVal.has_value()) {
            if (*adxVal > cfg_.adxThreshold) {
                // Market is trending: suppress mean-reversion signals
                return Signal::None;
            }
        }

        // Need at least minAgreement votes in the same direction
        int needed = std::min(cfg_.minAgreement, activeIndicators);
        if (needed <= 0) return Signal::None;

        // Edge trigger: don't re-fire the same direction until it resets
        if (riseVotes >= needed && !riseArmed_) {
            riseArmed_ = true;
            fallArmed_ = false;
            return Signal::Rise;
        }
        if (fallVotes >= needed && !fallArmed_) {
            fallArmed_ = true;
            riseArmed_ = false;
            return Signal::Fall;
        }

        // Reset arms when no indicator agrees anymore
        if (riseVotes < needed) riseArmed_ = false;
        if (fallVotes < needed) fallArmed_ = false;

        return Signal::None;
    }

    std::string name() const override { return "multi_confluence"; }

    std::string describeParams() const override {
        std::ostringstream ss;
        ss << "mask=0x" << std::hex << cfg_.enabledMask << std::dec
           << " agree=" << cfg_.minAgreement
           << " rsi=" << cfg_.rsiPeriod
           << " ema=" << cfg_.emaFast << "/" << cfg_.emaSlow
           << " macd=" << cfg_.macdFast << "/" << cfg_.macdSlow << "/" << cfg_.macdSignal
           << " bb=" << cfg_.bbPeriod << "/" << cfg_.bbStdDev
           << " stoch=" << cfg_.stochK << "/" << cfg_.stochD
           << " adx_thresh=" << cfg_.adxThreshold;
        return ss.str();
    }

    double lastRsi() const override { return lastRsi_; }

private:
    Config cfg_;
    RSI            rsi_;
    EMA            emaFast_;
    EMA            emaSlow_;
    MACD           macd_;
    BollingerBands bb_;
    Stochastic     stoch_;
    ADX            adx_;
    double lastRsi_   = 50.0;
    bool   riseArmed_ = false;
    bool   fallArmed_ = false;
};
