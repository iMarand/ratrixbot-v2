#pragma once
#include "../strategies/strategy_registry.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>

namespace fs = std::filesystem;

class ResultsManager {
public:
    static void listRuns(const std::string& baseDir) {
        if (!fs::exists(baseDir) || !fs::is_directory(baseDir)) {
            std::cout << "No results directory found at: " << baseDir << "\n";
            return;
        }

        std::vector<std::string> runs;
        for (const auto& entry : fs::directory_iterator(baseDir)) {
            if (entry.is_directory()) {
                runs.push_back(entry.path().filename().string());
            }
        }

        if (runs.empty()) {
            std::cout << "No runs found in " << baseDir << "\n";
            return;
        }

        std::sort(runs.begin(), runs.end()); // sorts chronologically by timestamp

        std::cout << "========================================\n"
                  << " Finished Training Runs\n"
                  << "========================================\n";

        for (const auto& run : runs) {
            std::string runPath = baseDir + "/" + run;
            std::vector<std::string> symbols;
            for (const auto& symDir : fs::directory_iterator(runPath)) {
                if (symDir.is_directory()) {
                    symbols.push_back(symDir.path().filename().string());
                }
            }
            std::cout << "[+] " << run << "  (Symbols: ";
            for (size_t i = 0; i < symbols.size(); i++) {
                std::cout << symbols[i] << (i + 1 == symbols.size() ? "" : ", ");
            }
            std::cout << ")\n";
        }
        std::cout << "========================================\n"
                  << "Use --load-run <run_id> --symbol <symbol> --rank <1-10>\n"
                  << "to backtest or paper trade a specific result.\n";
    }

    static void deleteRun(const std::string& baseDir, const std::string& runId) {
        std::string path = baseDir + "/" + runId;
        if (fs::exists(path)) {
            fs::remove_all(path);
            std::cout << "Deleted run: " << runId << "\n";
        } else {
            std::cerr << "Run not found: " << runId << "\n";
        }
    }

    static void deleteAllRuns(const std::string& baseDir) {
        if (fs::exists(baseDir)) {
            for (const auto& entry : fs::directory_iterator(baseDir)) {
                fs::remove_all(entry.path());
            }
            std::cout << "Deleted all runs in " << baseDir << ".\n";
        } else {
            std::cout << "No results directory found.\n";
        }
    }

    static std::unique_ptr<StrategyBase> loadStrategy(const std::string& baseDir,
                                                      const std::string& runId,
                                                      const std::string& symbol,
                                                      int rank,
                                                      std::string& outStrategyName) {
        std::string jsonPath = baseDir + "/" + runId + "/" + symbol + "/summary.json";
        if (!fs::exists(jsonPath)) {
            std::cerr << "Cannot find summary.json for run " << runId << " symbol " << symbol << "\n";
            return nullptr;
        }

        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            std::cerr << "Failed to open " << jsonPath << "\n";
            return nullptr;
        }

        try {
            nlohmann::json j;
            file >> j;

            auto strategies = j["strategies"];
            if (rank < 1 || rank > (int)strategies.size()) {
                std::cerr << "Invalid rank " << rank << ". Only ranks 1 to " << strategies.size() << " are available.\n";
                return nullptr;
            }

            auto s = strategies[rank - 1]; // rank is 1-indexed
            outStrategyName = s["strategyName"].get<std::string>();
            ParamSet params = s["params"].get<ParamSet>();

            std::cout << "Loading rank " << rank << " strategy: " << outStrategyName << "\n";
            std::cout << "Parameters restored from " << runId << ".\n";

            auto allGrid = StrategyRegistry::getAllGridEntries();
            for (const auto& entry : allGrid) {
                if (entry.name == outStrategyName) {
                    return entry.factory(params);
                }
            }
            std::cerr << "Strategy '" << outStrategyName << "' not found in registry.\n";
            return nullptr;
        } catch (const std::exception& e) {
            std::cerr << "Error parsing JSON: " << e.what() << "\n";
            return nullptr;
        }
    }
};
