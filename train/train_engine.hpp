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
#include <fstream>
#include <nlohmann/json.hpp>

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
    double lotSize      = 0.5;
    std::string resultsDir = "results";
    bool   autoAdjust   = false;
    double trainHours   = 0.0; // 0 = unlimited
    
    // Seed training: continue from a previous good run
    std::string seedRunId;
    int         seedRank = 0; // 0 = no seed
    
    // Advanced filtering
    std::vector<std::string> allowedStrategies;
    std::vector<double>      durations;
    std::vector<double>      candlePeriods;
    bool        skipRL       = false;
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
        gsCfg.lotSize = cfg_.lotSize;
        gsCfg.symbol = symbol;
        
        GridSearchEngine gs(gsCfg);
        auto allGrid = isForexOrCommodity(symbol) 
            ? StrategyRegistry::getForexGridEntries(cfg_.allowedStrategies, cfg_.durations, cfg_.candlePeriods) 
            : StrategyRegistry::getAllGridEntries(cfg_.allowedStrategies, cfg_.durations, cfg_.candlePeriods);
        
        // If seeding, inject the seed strategy's params + neighbors into the grid
        if (cfg_.seedRank > 0 && !cfg_.seedRunId.empty()) {
            injectSeedStrategy(allGrid, symbol);
        }
        
        auto topGrid = gs.run(allGrid, trainTimes, trainPrices, cfg_.topK);
        
        if (topGrid.empty()) {
            std::cerr << "Grid search yielded no viable strategies.\n";
            return;
        }

        // Prepare factories for Phase 2
        std::vector<std::function<std::unique_ptr<StrategyBase>()>> topFactories;
        for (const auto& res : topGrid) {
            for (const auto& entry : allGrid) {
                if (entry.name == res.strategyName) {
                    ParamSet p = res.params;
                    topFactories.push_back([entry, p]() { return entry.factory(p); });
                    break;
                }
            }
        }

        RLTrainResult rlRes;
        
        // Phase 2: RL Meta-Learner Training
        if (cfg_.skipRL) {
            std::cout << "\n--- Skipping RL Meta-Learner Phase (--no-rl) ---\n";
            rlRes.netPnl = 0.0;
        } else {
            RLTrainConfig rlCfg;
            rlCfg.durationSec = cfg_.durationSec;
            rlCfg.stake = cfg_.stake;
            rlCfg.payoutPct = cfg_.payoutPct;
            rlCfg.trainEpochs = 3;
            
            std::cout << "\nTraining RL agent on top " << topFactories.size() << " strategies...\n";
            RLTrainer rl(topFactories, rlCfg);
            
            // If seeding, load the previous Q-table so the RL agent continues learning
            if (cfg_.seedRank > 0 && !cfg_.seedRunId.empty()) {
                std::string seedQPath = cfg_.resultsDir + "/" + cfg_.seedRunId + "/" + symbol + "/qtable.json";
                if (std::filesystem::exists(seedQPath)) {
                    rl.agent().load(seedQPath);
                    std::cout << "  Loaded seed Q-table from " << cfg_.seedRunId << "\n";
                }
            }
            
            rlRes = rl.train(trainTimes, trainPrices);
            
            // Save Q-Table
            std::string symbolDir = runDir + "/" + symbol;
            std::filesystem::create_directories(symbolDir);
            std::string qTablePath = symbolDir + "/qtable.json";
            rl.agent().save(qTablePath);
        }

        // Save Results
        std::string symbolDir = runDir + "/" + symbol;
        std::filesystem::create_directories(symbolDir);
        
        std::string summaryPath = symbolDir + "/summary.txt";
        std::string jsonPath = symbolDir + "/summary.json";
        ReportGenerator::writeSummaryFiles(summaryPath, jsonPath, symbol, topGrid, rlRes);
        ReportGenerator::printConsoleSummary(symbol, topGrid, rlRes);
    }
    
    // Load seed strategy params from a previous run and add neighbor variations
    void injectSeedStrategy(std::vector<StrategyGridEntry>& grid, const std::string& symbol) {
        std::string jsonPath = cfg_.resultsDir + "/" + cfg_.seedRunId + "/" + symbol + "/summary.json";
        if (!std::filesystem::exists(jsonPath)) {
            std::cerr << "Seed run not found for " << symbol << ", skipping seed.\n";
            return;
        }
        
        std::ifstream file(jsonPath);
        nlohmann::json j;
        try { file >> j; } catch (...) { return; }
        
        auto strategies = j["strategies"];
        if (cfg_.seedRank < 1 || cfg_.seedRank > (int)strategies.size()) return;
        
        auto seedEntry = strategies[cfg_.seedRank - 1];
        std::string seedName = seedEntry["strategyName"].get<std::string>();
        ParamSet seedParams = seedEntry["params"].get<ParamSet>();
        
        std::cout << "  Seeding from " << cfg_.seedRunId << " rank " << cfg_.seedRank 
                  << " (" << seedName << ")\n";
        
        // Find this strategy's grid entry and inject the seed + neighbors
        for (auto& entry : grid) {
            if (entry.name == seedName) {
                // Add the exact seed params as the first combination
                entry.paramCombinations.insert(entry.paramCombinations.begin(), seedParams);
                
                // Generate neighbor variations: for each numeric param, try ±10% and ±20%
                for (auto& [key, val] : seedParams) {
                    for (double factor : {0.8, 0.9, 1.1, 1.2}) {
                        ParamSet neighbor = seedParams;
                        neighbor[key] = std::round(val * factor);
                        if (neighbor[key] != val && neighbor[key] > 0) {
                            entry.paramCombinations.push_back(neighbor);
                        }
                    }
                }
                
                std::cout << "  Injected " << entry.paramCombinations.size() 
                          << " combinations (seed + neighbors)\n";
                break;
            }
        }
    }

    DerivWsClient& client_;
    TrainConfig cfg_;
};
