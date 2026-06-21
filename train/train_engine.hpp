#pragma once
#include "grid_search.hpp"
#include "report.hpp"
#include "../rl/rl_trainer.hpp"
#include "../strategies/strategy_registry.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <functional>
#include <memory>
#include <thread>
#include <chrono>

// Forward declarations to break dependencies
class DerivWsClient;
bool fetchTickHistory(DerivWsClient& client, const std::string& symbol, int totalCount,
                      std::vector<int64_t>& outTimes, std::vector<double>& outPrices);
namespace detail {
    extern std::atomic<bool> g_stopRequested;
}

struct TrainConfig {
    std::vector<std::string> symbols;
    int    historyCount = 50000;
    int    topK         = 10;
    double trainSplit   = 0.8;
    int    durationSec  = 15;
    double stake        = 1.0;
    double payoutPct    = 0.95;
    std::string resultsDir = "results";
    bool   autoAdjust   = false;
    double trainHours   = 0.0; // 0 = unlimited
};

class TrainEngine {
public:
    TrainEngine(DerivWsClient& client, const TrainConfig& cfg)
        : client_(client), cfg_(cfg) {}

    void run(const std::string& runDir) {
        for (const auto& symbol : cfg_.symbols) {
            if (detail::g_stopRequested.load()) break;
            
            std::cout << "\n>>> Training on " << symbol << " <<<\n";
            trainSymbol(symbol, runDir);
        }
    }

private:
    void trainSymbol(const std::string& symbol, const std::string& runDir) {
        std::vector<int64_t> times;
        std::vector<double> prices;
        
        std::cout << "Fetching " << cfg_.historyCount << " ticks...\n";
        if (!fetchTickHistory(client_, symbol, cfg_.historyCount, times, prices)) {
            std::cerr << "Failed to fetch data for " << symbol << ", skipping.\n";
            return;
        }
        
        if (prices.size() < 1000) {
            std::cerr << "Not enough data for " << symbol << " (" << prices.size() << " ticks), skipping.\n";
            return;
        }

        int trainSize = (int)(prices.size() * cfg_.trainSplit);
        std::vector<int64_t> trainTimes(times.begin(), times.begin() + trainSize);
        std::vector<double> trainPrices(prices.begin(), prices.begin() + trainSize);
        
        std::vector<int64_t> testTimes(times.begin() + trainSize, times.end());
        std::vector<double> testPrices(prices.begin() + trainSize, prices.end());

        // Phase 1: Grid Search
        GridSearchEngine::Config gsCfg;
        gsCfg.durationSec = cfg_.durationSec;
        gsCfg.stake = cfg_.stake;
        gsCfg.payoutPct = cfg_.payoutPct;
        gsCfg.minTrades = 10;
        
        GridSearchEngine gs(gsCfg);
        auto allGrid = StrategyRegistry::getAllGridEntries();
        auto topGrid = gs.run(allGrid, trainTimes, trainPrices, cfg_.topK);
        
        if (topGrid.empty()) {
            std::cerr << "Grid search yielded no viable strategies.\n";
            return;
        }

        // Phase 2: RL Meta-Learner Training
        std::vector<std::function<std::unique_ptr<StrategyBase>()>> topFactories;
        for (const auto& res : topGrid) {
            // Find the factory for this strategy name
            for (const auto& entry : allGrid) {
                if (entry.name == res.strategyName) {
                    ParamSet p = res.params; // copy
                    topFactories.push_back([entry, p]() { return entry.factory(p); });
                    break;
                }
            }
        }

        RLTrainConfig rlCfg;
        rlCfg.durationSec = cfg_.durationSec;
        rlCfg.stake = cfg_.stake;
        rlCfg.payoutPct = cfg_.payoutPct;
        rlCfg.trainEpochs = 3;
        
        std::cout << "\nTraining RL agent on top " << topFactories.size() << " strategies...\n";
        RLTrainer rl(topFactories, rlCfg);
        RLTrainResult rlRes = rl.train(trainTimes, trainPrices);

        // Save Results
        std::string symbolDir = runDir + "/" + symbol;
        std::filesystem::create_directories(symbolDir);
        
        std::string summaryPath = symbolDir + "/summary.txt";
        std::string jsonPath = symbolDir + "/summary.json";
        ReportGenerator::writeSummaryFiles(summaryPath, jsonPath, symbol, topGrid, rlRes);
        ReportGenerator::printConsoleSummary(symbol, topGrid, rlRes);
        
        std::string qTablePath = symbolDir + "/qtable.json";
        rl.agent().save(qTablePath);
    }

    DerivWsClient& client_;
    TrainConfig cfg_;
};
