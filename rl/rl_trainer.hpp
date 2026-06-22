#pragma once
#include "q_learning.hpp"
#include "state_features.hpp"
#include "../strategies/strategy_base.hpp"
#include "../signal_types.hpp"
#include <vector>
#include <memory>
#include <functional>
#include <iostream>
#include <cstdint>

// ============================================================================
// RL Trainer — trains the Q-learning agent on historical tick data.
//
// For each tick in the training set:
//   1. Extract market state features
//   2. Agent picks an action (which strategy to use)
//   3. That strategy produces a signal (or None)
//   4. If a signal fires, simulate the trade using the price data
//   5. Reward = P&L of the trade
//   6. Update Q(s, a) with the reward
//
// After training, the agent's Q-table encodes which strategy tends to
// produce positive P&L in each market regime.
// ============================================================================

struct RLTrainConfig {
    int    durationSec = 15;
    double stake       = 1.0;
    double payoutPct   = 0.95;
    int    trainEpochs = 3;       // how many passes over the data
};

struct RLTrainResult {
    int    totalUpdates    = 0;
    int    totalTrades     = 0;
    int    wins            = 0;
    double netPnl          = 0.0;
    double finalEpsilon    = 0.0;
    int    statesVisited   = 0;
};

class RLTrainer {
public:
    // strategies: list of strategy factories that the agent can choose from.
    // Each factory takes no args and returns a fresh strategy instance.
    RLTrainer(std::vector<std::function<std::unique_ptr<StrategyBase>()>> strategyFactories,
              const RLTrainConfig& cfg)
        : strategyFactories_(std::move(strategyFactories)),
          cfg_(cfg),
          agent_((int)strategyFactories_.size()) {}

    RLTrainResult train(const std::vector<int64_t>& times,
                        const std::vector<double>& prices) {
        size_t n = prices.size();
        if (n < 200) return {};

        RLTrainResult result;

        for (int epoch = 0; epoch < cfg_.trainEpochs; epoch++) {
            // Create fresh strategy instances and feature extractor for each epoch
            std::vector<std::unique_ptr<StrategyBase>> strategies;
            for (auto& factory : strategyFactories_) {
                strategies.push_back(factory());
            }
            StateFeatureExtractor features;

            std::optional<MarketState> prevState;

            for (size_t i = 0; i < n; i++) {
                auto state = features.update(prices[i], times[i]);
                if (!state.has_value()) {
                    // Still warming up indicators — feed all strategies to keep them in sync
                    for (auto& s : strategies) s->onPrice(times[i], prices[i]);
                    continue;
                }

                int stateIdx = state->toIndex();

                // Agent picks which strategy to use
                int action = agent_.selectAction(stateIdx);

                // Feed all strategies (keep them warmed up) but only act on the chosen one
                Signal chosenSignal = Signal::None;
                for (int a = 0; a < (int)strategies.size(); a++) {
                    Signal sig = strategies[a]->onPrice(times[i], prices[i]);
                    if (a == action) chosenSignal = sig;
                }

                if (chosenSignal != Signal::None) {
                    // Simulate trade
                    int64_t targetExit = times[i] + cfg_.durationSec;
                    size_t j = i + 1;
                    while (j < n && times[j] < targetExit) j++;
                    if (j >= n) continue;

                    bool priceUp   = prices[j] > prices[i];
                    bool priceDown = prices[j] < prices[i];
                    bool won = (chosenSignal == Signal::Rise) ? priceUp : priceDown;
                    double pnl = won ? (cfg_.stake * cfg_.payoutPct) : -cfg_.stake;

                    // Find next state for Q-update
                    int nextStateIdx = stateIdx; // fallback: same state
                    if (j < n) {
                        // Peek ahead to get next state (approximate)
                        nextStateIdx = stateIdx; // state doesn't change fast enough
                    }

                    agent_.update(stateIdx, action, pnl, nextStateIdx);
                    result.totalTrades++;
                    if (won) result.wins++;
                    result.netPnl += pnl;
                }

                prevState = state;
            }

            std::cout << "  RL epoch " << (epoch + 1) << "/" << cfg_.trainEpochs
                      << ": trades=" << result.totalTrades
                      << " wins=" << result.wins
                      << " P&L=" << result.netPnl
                      << " epsilon=" << agent_.epsilon() << "\n";
        }

        result.totalUpdates = agent_.totalUpdates();
        result.finalEpsilon = agent_.epsilon();

        // Count visited states
        auto policy = agent_.getPolicy();
        result.statesVisited = (int)policy.size();

        return result;
    }

    QLearningAgent& agent() { return agent_; }
    const QLearningAgent& agent() const { return agent_; }

private:
    std::vector<std::function<std::unique_ptr<StrategyBase>()>> strategyFactories_;
    RLTrainConfig cfg_;
    QLearningAgent agent_;
};
