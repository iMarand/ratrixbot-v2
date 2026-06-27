#pragma once
#include "../signal_types.hpp"
#include <string>
#include <cstdint>
#include <memory>
#include <vector>
#include <map>

// ============================================================================
// StrategyBase — uniform interface for all strategies so the backtester,
// paper trader, grid search, and RL trainer can work with any strategy
// interchangeably.
// ============================================================================

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // Feed one tick. Returns Rise/Fall/None.
    virtual Signal onPrice(int64_t time, double price) = 0;

    // Human-readable strategy name (e.g. "ema_cross", "macd")
    virtual std::string name() const = 0;

    // Describe the parameter set in use (for logging/reports)
    virtual std::string describeParams() const = 0;

    // Last RSI value (if available, for display). Default returns 50.
    virtual double lastRsi() const { return 50.0; }
};

// A named parameter set: maps param names to double values.
// Used by grid search to specify configurations.
using ParamSet = std::map<std::string, double>;

// Factory function type: given a ParamSet, create a strategy instance.
using StrategyFactory = std::function<std::unique_ptr<StrategyBase>(const ParamSet&)>;

// One entry in the grid search: a strategy factory + all parameter combinations.
struct StrategyGridEntry {
    std::string name;
    StrategyFactory factory;
    std::vector<ParamSet> paramCombinations;
};
