#pragma once
#include "state_features.hpp"
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

// ============================================================================
// Tabular Q-Learning Agent
//
// Q(s,a) ← Q(s,a) + α[r + γ·max_a' Q(s',a') − Q(s,a)]
//
// State:   discretized market regime (54 states from StateFeatureExtractor)
// Actions: indices into a list of strategy configurations
// Reward:  P&L from the trade taken by the selected strategy
//
// ε-greedy exploration with decaying ε.
// Q-table persists to/from JSON file for cross-session learning.
// ============================================================================

class QLearningAgent {
public:
    QLearningAgent(int numActions, double alpha = 0.1, double gamma = 0.95,
                   double epsilonStart = 1.0, double epsilonEnd = 0.05,
                   double epsilonDecay = 0.9995)
        : numActions_(numActions), alpha_(alpha), gamma_(gamma),
          epsilon_(epsilonStart), epsilonEnd_(epsilonEnd), epsilonDecay_(epsilonDecay),
          rng_(std::random_device{}())
    {
        // Initialize Q-table to zero: 54 states × numActions
        qTable_.resize(MarketState::NUM_STATES, std::vector<double>(numActions, 0.0));
        visitCount_.resize(MarketState::NUM_STATES, std::vector<int>(numActions, 0));
    }

    // Choose an action given current state (ε-greedy)
    int selectAction(int stateIdx) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < epsilon_) {
            // Explore: random action
            std::uniform_int_distribution<int> actionDist(0, numActions_ - 1);
            return actionDist(rng_);
        }
        // Exploit: best Q-value action
        return bestAction(stateIdx);
    }

    // Best action for a state (greedy, no exploration)
    int bestAction(int stateIdx) const {
        int best = 0;
        double bestQ = qTable_[stateIdx][0];
        for (int a = 1; a < numActions_; a++) {
            if (qTable_[stateIdx][a] > bestQ) {
                bestQ = qTable_[stateIdx][a];
                best = a;
            }
        }
        return best;
    }

    // Update Q-value after observing reward and next state
    void update(int stateIdx, int action, double reward, int nextStateIdx) {
        double maxNextQ = *std::max_element(
            qTable_[nextStateIdx].begin(), qTable_[nextStateIdx].end());

        double oldQ = qTable_[stateIdx][action];
        qTable_[stateIdx][action] = oldQ + alpha_ * (reward + gamma_ * maxNextQ - oldQ);
        visitCount_[stateIdx][action]++;

        // Decay epsilon
        epsilon_ = std::max(epsilonEnd_, epsilon_ * epsilonDecay_);
        totalUpdates_++;
    }

    // Get Q-value for a state-action pair
    double getQ(int stateIdx, int action) const {
        return qTable_[stateIdx][action];
    }

    double epsilon()      const { return epsilon_; }
    int    totalUpdates() const { return totalUpdates_; }
    int    numActions()   const { return numActions_; }

    // Get the best action and its Q-value for each state
    struct StatePolicy {
        int stateIdx;
        int bestAction;
        double bestQ;
        int totalVisits;
    };

    std::vector<StatePolicy> getPolicy() const {
        std::vector<StatePolicy> policy;
        for (int s = 0; s < MarketState::NUM_STATES; s++) {
            int totalVis = 0;
            for (int a = 0; a < numActions_; a++) totalVis += visitCount_[s][a];
            if (totalVis == 0) continue; // skip unvisited states

            int best = bestAction(s);
            policy.push_back({s, best, qTable_[s][best], totalVis});
        }
        return policy;
    }

    // Save Q-table to JSON file
    bool save(const std::string& path) const {
        nlohmann::json j;
        j["num_actions"]   = numActions_;
        j["epsilon"]       = epsilon_;
        j["total_updates"] = totalUpdates_;
        j["q_table"]       = qTable_;
        j["visit_count"]   = visitCount_;

        std::ofstream file(path);
        if (!file.is_open()) return false;
        file << j.dump(2);
        return true;
    }

    // Load Q-table from JSON file
    bool load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        try {
            nlohmann::json j;
            file >> j;

            int loadedActions = j.value("num_actions", 0);
            if (loadedActions != numActions_) {
                // Action space changed (different number of strategies);
                // can't reuse old Q-table
                return false;
            }

            epsilon_      = j.value("epsilon", epsilon_);
            totalUpdates_ = j.value("total_updates", 0);
            qTable_       = j["q_table"].get<std::vector<std::vector<double>>>();
            visitCount_   = j["visit_count"].get<std::vector<std::vector<int>>>();
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    int    numActions_;
    double alpha_;
    double gamma_;
    double epsilon_;
    double epsilonEnd_;
    double epsilonDecay_;
    int    totalUpdates_ = 0;

    std::vector<std::vector<double>> qTable_;      // [state][action] → Q-value
    std::vector<std::vector<int>>    visitCount_;   // [state][action] → visit count
    std::mt19937 rng_;
};
