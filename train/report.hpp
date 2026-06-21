#pragma once
#include "grid_search.hpp"
#include "../rl/rl_trainer.hpp"
#include "../rl/q_learning.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <chrono>

// ============================================================================
// Report Generator
//
// Formats training and grid search results into console output, CSV ledgers,
// and JSON summary files in the results/ directory.
// ============================================================================

namespace fs = std::filesystem;

class ReportGenerator {
public:
    static std::string createResultsDir(const std::string& baseDir) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
        std::string dirName = baseDir + "/run_" + ss.str();
        
        fs::create_directories(dirName);
        return dirName;
    }

    static void writeSummaryFiles(const std::string& txtPath, 
                                  const std::string& jsonPath,
                                  const std::string& symbol,
                                  const std::vector<GridSearchResult>& topGrid,
                                  const RLTrainResult& rlRes) {
        // Write TXT Summary
        std::ofstream file(txtPath);
        file << "========================================\n"
             << " Training Summary for " << symbol << "\n"
             << "========================================\n\n";
             
        file << "[Top " << topGrid.size() << " Grid Search Strategies]\n\n";
        for (size_t i = 0; i < topGrid.size(); i++) {
            const auto& res = topGrid[i];
            file << "Rank " << (i+1) << ": " << res.describe() << "\n"
                 << "  Score:      " << res.score << "\n"
                 << "  P&L:        " << res.netPnl << "\n"
                 << "  Win Rate:   " << res.winRatePct << "%\n"
                 << "  Trades:     " << res.totalTrades << "\n"
                 << "  Max DD:     " << res.maxDrawdown << "\n\n";
        }
        
        file << "[RL Agent Training]\n\n"
             << "  Total updates:  " << rlRes.totalUpdates << "\n"
             << "  States visited: " << rlRes.statesVisited << " (out of 54)\n"
             << "  Final Epsilon:  " << rlRes.finalEpsilon << "\n"
             << "  Total P&L:      " << rlRes.netPnl << " (" << rlRes.wins << "/" << rlRes.totalTrades << ")\n";
             
        // Write JSON Summary
        nlohmann::json j;
        j["symbol"] = symbol;
        j["rl_result"] = {
            {"netPnl", rlRes.netPnl},
            {"totalTrades", rlRes.totalTrades},
            {"wins", rlRes.wins},
            {"statesVisited", rlRes.statesVisited}
        };
        
        nlohmann::json strats = nlohmann::json::array();
        for (size_t i = 0; i < topGrid.size(); i++) {
            const auto& res = topGrid[i];
            nlohmann::json s;
            s["rank"] = i + 1;
            s["strategyName"] = res.strategyName;
            s["score"] = res.score;
            s["netPnl"] = res.netPnl;
            s["winRatePct"] = res.winRatePct;
            s["totalTrades"] = res.totalTrades;
            s["maxDrawdown"] = res.maxDrawdown;
            s["params"] = res.params; // std::map automatically serializes to json object
            strats.push_back(s);
        }
        j["strategies"] = strats;
        
        std::ofstream jfile(jsonPath);
        if (jfile.is_open()) {
            jfile << j.dump(2);
        }
    }

    static void printConsoleSummary(const std::string& symbol,
                                    const std::vector<GridSearchResult>& topGrid,
                                    const RLTrainResult& rlRes) {
        std::cout << "\n========================================\n"
                  << " Training Summary for " << symbol << "\n"
                  << "========================================\n";
             
        std::cout << "[Top " << topGrid.size() << " Strategies by Score]\n";
        for (size_t i = 0; i < topGrid.size(); i++) {
            const auto& res = topGrid[i];
            std::cout << "  " << (i+1) << ". " << res.strategyName << "  P&L: " << res.netPnl 
                      << "  WR: " << res.winRatePct << "%  Trades: " << res.totalTrades 
                      << "  DD: " << res.maxDrawdown << "\n"
                      << "     (" << res.paramString() << ")\n";
        }
        
        std::cout << "\n[RL Meta-Learner]\n"
                  << "  Trained on top strategies. Agent P&L: " << rlRes.netPnl 
                  << " (" << rlRes.wins << " wins / " << rlRes.totalTrades << " trades)\n"
                  << "  States visited: " << rlRes.statesVisited << " / 54\n"
                  << "========================================\n\n";
    }
};
