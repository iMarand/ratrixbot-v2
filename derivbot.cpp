// ============================================================================
// derivbot.cpp - Deriv synthetic-index RSI backtester & paper trader
//
// Single-file build using Boost.Beast (WebSocket+TLS) and nlohmann::json.
//
// SCOPE: this tool only ever sends "ticks_history" (historical data) and
// "ticks"+"subscribe" (live feed) requests to Deriv. There is no buy/sell/
// proposal call anywhere in this file and no API token is used. Paper
// trading positions are tracked purely in local memory/CSV -- no real
// orders are ever placed and no real money is ever at risk.
//
// Build (MSYS2 MinGW64), with boost + openssl + nlohmann-json installed:
//   pacman -S mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-nlohmann-json
//   g++ derivbot.cpp -o derivbot.exe -std=c++17 -lssl -lcrypto -lws2_32 -lwsock32 -lboost_system
//
// (On Linux/macOS, drop -lws2_32 -lwsock32; they're Windows socket libs.)
// ============================================================================

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include "signal_types.hpp"
#include "rsi.hpp"
#include "confluence_strategy.hpp"

#include "strategies/ema_crossover.hpp"
#include "strategies/macd_strategy.hpp"
#include "strategies/bollinger_strategy.hpp"
#include "strategies/stochastic_strategy.hpp"
#include "strategies/multi_confluence.hpp"
#include "train/train_engine.hpp"
#include "train/results_manager.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <atomic>
#include <csignal>
#include <algorithm>
#include <functional>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <chrono>

// Helper to parse comma-separated strings
static std::vector<std::string> parseCsvString(const std::string& s) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

static std::vector<double> parseCsvDouble(const std::string& s) {
    std::vector<double> result;
    for (const auto& item : parseCsvString(s)) {
        try { result.push_back(std::stod(item)); } catch (...) {}
    }
    return result;
}

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = boost::asio::ip::tcp;
using nlohmann::json;

// ============================================================================
// Section 1: Edge-triggered RSI threshold strategy
// (the RSI indicator itself now lives in rsi.hpp, shared with confluence_strategy.hpp)
// ============================================================================

class RsiThresholdStrategy {
public:
    RsiThresholdStrategy(int rsiPeriod, double oversold, double overbought)
        : rsi_(rsiPeriod), oversold_(oversold), overbought_(overbought) {}

    Signal onPrice(double price) {
        auto value = rsi_.update(price);
        if (!value.has_value()) return Signal::None;

        double rsiVal = *value;
        Signal signal = Signal::None;

        if (rsiVal <= oversold_ && !oversoldArmed_) {
            signal = Signal::Rise;
            oversoldArmed_ = true;
        } else if (rsiVal > oversold_) {
            oversoldArmed_ = false;
        }

        if (rsiVal >= overbought_ && !overboughtArmed_) {
            if (signal == Signal::None) signal = Signal::Fall;
            overboughtArmed_ = true;
        } else if (rsiVal < overbought_) {
            overboughtArmed_ = false;
        }

        lastRsi_ = rsiVal;
        return signal;
    }

    bool isReady() const { return rsi_.isReady(); }
    double lastRsi() const { return lastRsi_; }

private:
    RSI rsi_;
    double oversold_;
    double overbought_;
    bool oversoldArmed_ = false;
    bool overboughtArmed_ = false;
    double lastRsi_ = 50.0;
};

// ============================================================================
// Section 1b: Cooldown gate -- enforces a minimum spacing between fired
// signals, regardless of which strategy produced them. This exists because
// without it, a strategy can re-fire repeatedly on the same underlying price
// wiggle (RSI dips below 30, ticks back above just enough to re-arm, dips
// again seconds later) -- producing several *correlated* trades that tend to
// win or lose together, which is what drives ugly consecutive-loss streaks.
// ============================================================================

class CooldownGate {
public:
    explicit CooldownGate(int cooldownSec) : cooldownSec_(cooldownSec) {}

    Signal filter(int64_t time, Signal sig) {
        if (sig == Signal::None) return Signal::None;
        if (cooldownSec_ > 0 && (time - lastFireTime_) < cooldownSec_) return Signal::None;
        lastFireTime_ = time;
        return sig;
    }

private:
    int cooldownSec_;
    int64_t lastFireTime_ = INT64_MIN / 2;
};

// ============================================================================
// Section 3: Trade record
// ============================================================================

enum class Direction { Rise, Fall };

struct Trade {
    int64_t entryTime = 0;
    double  entryPrice = 0.0;
    int64_t exitTime = 0;
    double  exitPrice = 0.0;
    Direction direction = Direction::Rise;
    double  stake = 0.0;
    double  payoutPct = 0.0;
    bool    won = false;
    double  pnl = 0.0;

    void settle() {
        bool priceUp = exitPrice > entryPrice;
        bool priceDown = exitPrice < entryPrice;
        won = (direction == Direction::Rise) ? priceUp : priceDown; // tie = loss
        pnl = won ? (stake * payoutPct) : -stake;
    }

    static const char* dirName(Direction d) {
        return d == Direction::Rise ? "RISE" : "FALL";
    }
};

// ============================================================================
// Section 4: Deriv WebSocket client (Boost.Beast, synchronous/blocking)
// ============================================================================

class DerivWsClient {
public:
    DerivWsClient() : ssl_ctx_(ssl::context::tlsv12_client), ws_(ioc_, ssl_ctx_) {
        // NOTE: certificate verification is disabled here for simplicity.
        // For production use, prefer ssl_ctx_.set_default_verify_paths() and
        // ssl::verify_peer, with a trusted CA bundle available at runtime.
        ssl_ctx_.set_verify_mode(ssl::verify_none);
    }

    bool connect(const std::string& host, const std::string& port, const std::string& path) {
        try {
            tcp::resolver resolver(ioc_);
            auto const results = resolver.resolve(host, port);

            auto ep = beast::get_lowest_layer(ws_).connect(results);

            if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host.c_str())) {
                throw beast::system_error(
                    beast::error_code(static_cast<int>(::ERR_get_error()),
                                       net::error::get_ssl_category()));
            }

            ws_.next_layer().handshake(ssl::stream_base::client);

            ws_.set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(http::field::user_agent, "DerivBot/1.0");
                }));

            std::string hostPort = host + ":" + std::to_string(ep.port());
            http::response<http::string_body> upgradeResponse;
            try {
                ws_.handshake(upgradeResponse, hostPort, path);
            } catch (const std::exception& e) {
                std::cerr << "Connect failed: " << e.what() << std::endl;
                if (!upgradeResponse.body().empty() || upgradeResponse.result_int() != 0) {
                    std::cerr << "  HTTP status: " << upgradeResponse.result_int() << std::endl;
                    std::cerr << "  Response: " << upgradeResponse.body() << std::endl;
                }
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Connect failed: " << e.what() << std::endl;
            return false;
        }
    }

    bool sendText(const std::string& msg) {
        try {
            ws_.text(true);
            ws_.write(net::buffer(msg));
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Send failed: " << e.what() << std::endl;
            return false;
        }
    }

    // Blocking receive of one complete message (Beast reassembles fragments
    // internally, so unlike a raw WinHTTP client this needs no manual loop).
    bool receiveText(std::string& out) {
        try {
            beast::flat_buffer buffer;
            ws_.read(buffer);
            out = beast::buffers_to_string(buffer.data());
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Receive failed: " << e.what() << std::endl;
            return false;
        }
    }

    void close() {
        try { ws_.close(websocket::close_code::normal); } catch (...) {}
    }

private:
    net::io_context ioc_;
    ssl::context ssl_ctx_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
};

// ============================================================================
// Section 5: Backtester
// ============================================================================

struct BacktestConfig {
    std::string symbol;
    std::string strategyName = "rsi";
    int historyCount = 5000;
    int durationSec = 15;
    int rsiPeriod = 14;
    double oversold = 30.0;
    double overbought = 70.0;
    double stake = 1.0;
    double payoutPct = 0.95;
    double lotSize = 0.5;
    double tpPips = 0.0;
    double slPips = 0.0;
    double spreadPips = 0.0;
    int cooldownSec = 0;
    std::string csvOut = "backtest_trades.csv";
    int maxConsecLosses = 0; // 0 = no limit; otherwise stop opening new trades after N losses in a row
};

struct BacktestResult {
    int totalTrades = 0;
    int wins = 0;
    int losses = 0;
    double netPnl = 0.0;
    double maxDrawdown = 0.0;
    double winRatePct = 0.0;
    int maxConsecLossStreak = 0;
    int tradesSkippedByRiskControl = 0;
};

bool fetchTickHistoryPage(DerivWsClient& client, const std::string& symbol, int count,
                           const std::string& end,
                           std::vector<int64_t>& outTimes, std::vector<double>& outPrices) {
    json req = {
        {"ticks_history", symbol},
        {"count", count},
        {"end", end},
        {"style", "ticks"}
    };

    if (!client.sendText(req.dump())) return false;

    std::string raw;
    if (!client.receiveText(raw)) return false;

    json resp;
    try {
        resp = json::parse(raw);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse ticks_history response: " << e.what() << std::endl;
        return false;
    }

    if (resp.contains("error")) {
        std::cerr << "Deriv API error: " << resp["error"].value("message", "unknown error") << std::endl;
        return false;
    }
    if (!resp.contains("history")) {
        std::cerr << "Unexpected response, no 'history' field: " << raw.substr(0, 300) << std::endl;
        return false;
    }

    auto& hist = resp["history"];
    outTimes = hist["times"].get<std::vector<int64_t>>();
    outPrices = hist["prices"].get<std::vector<double>>();

    if (outTimes.size() != outPrices.size()) {
        std::cerr << "Tick history times/prices size mismatch for '" << symbol << "'.\n";
        return false;
    }
    if (outTimes.empty()) {
        std::cerr << "Tick history for '" << symbol << "' came back empty.\n"
                   << "  Possible causes: market is closed right now (forex/commodities\n"
                   << "  don't trade on weekends), the symbol code is wrong, or this\n"
                   << "  app_id doesn't have access to it. Raw response:\n  "
                   << raw.substr(0, 400) << "\n";
        return false;
    }
    return true;
}

// Deriv caps a single ticks_history request at 5000 ticks. To fetch more,
// page backward in time: each subsequent request asks for ticks ending
// right before the earliest tick already collected, then everything is
// merged in chronological order.
bool fetchTickHistory(DerivWsClient& client, const std::string& symbol, int totalCount,
                       std::vector<int64_t>& outTimes, std::vector<double>& outPrices) {
    const int kMaxPerRequest = 5000;
    outTimes.clear();
    outPrices.clear();

    std::string end = "latest";
    int remaining = totalCount;

    while (remaining > 0) {
        int batchCount = std::min(remaining, kMaxPerRequest);

        std::vector<int64_t> batchTimes;
        std::vector<double> batchPrices;
        if (!fetchTickHistoryPage(client, symbol, batchCount, end, batchTimes, batchPrices)) {
            // If we already collected something, return what we have rather
            // than failing the whole fetch outright.
            return !outTimes.empty();
        }

        // Prepend this older batch in front of what's already collected.
        outTimes.insert(outTimes.begin(), batchTimes.begin(), batchTimes.end());
        outPrices.insert(outPrices.begin(), batchPrices.begin(), batchPrices.end());

        std::cout << "  fetched batch of " << batchTimes.size() << " ticks (total so far: "
                  << outTimes.size() << ")\n";

        if ((int)batchTimes.size() < batchCount) {
            // Server returned fewer ticks than asked for -- no more history available.
            break;
        }

        remaining -= (int)batchTimes.size();
        // Next page ends right before the earliest tick we just collected.
        end = std::to_string(batchTimes.front() - 1);
    }

    return !outTimes.empty();
}

BacktestResult runBacktest(const BacktestConfig& cfg,
                            const std::vector<int64_t>& times,
                            const std::vector<double>& prices,
                            std::function<Signal(int64_t, double)> strategyFn) {
    std::vector<Trade> trades;

    int currentConsecLosses = 0;
    bool riskControlTripped = false;
    int skippedByRiskControl = 0;

    size_t n = prices.size();
    for (size_t i = 0; i < n; i++) {
        Signal sig = strategyFn(times[i], prices[i]);
        if (sig == Signal::None) continue;

        if (cfg.maxConsecLosses > 0 && currentConsecLosses >= cfg.maxConsecLosses) {
            // Risk control: refuse to open new trades while on a losing streak
            // at/above the configured limit. Re-arms automatically the moment
            // a trade would have won (see streak reset below), mirroring how
            // you'd actually want a live bot to pause-and-resume.
            skippedByRiskControl++;
            riskControlTripped = true;
            continue;
        }

        int64_t entryTime = times[i];
        double entryPrice = prices[i];
        Trade t;
        t.entryTime = entryTime;
        t.entryPrice = entryPrice;
        t.direction = (sig == Signal::Rise) ? Direction::Rise : Direction::Fall;
        t.stake = cfg.stake;
        t.payoutPct = cfg.payoutPct;

        bool useTPSL = isForexOrCommodity(cfg.symbol) && cfg.tpPips > 0 && cfg.slPips > 0;
        
        if (useTPSL) {
            double pipSize = getPipSize(cfg.symbol);
            double tpDist = cfg.tpPips * pipSize;
            double slDist = cfg.slPips * pipSize;
            double spreadDist = cfg.spreadPips * pipSize;
            
            if (slDist <= spreadDist) {
                t.exitTime = entryTime;
                t.exitPrice = entryPrice; // instantly filled
                t.won = false;
                t.pnl = -(cfg.slPips * dollarPerPip);
            } else {
                bool resolved = false;
                for (size_t j = i + 1; j < n; j++) {
                    double move = prices[j] - entryPrice;
                    if (sig == Signal::Fall) move = -move; // invert for sell

                    if (move >= (tpDist + spreadDist)) {
                        t.exitTime = times[j];
                        t.exitPrice = (sig == Signal::Rise) ? (entryPrice + tpDist) : (entryPrice - tpDist);
                        t.won = true;
                        t.pnl = cfg.tpPips * dollarPerPip;
                        resolved = true;
                        break;
                    }
                    if (move <= -(slDist - spreadDist)) {
                        t.exitTime = times[j];
                        t.exitPrice = (sig == Signal::Rise) ? (entryPrice - slDist) : (entryPrice + slDist);
                        t.won = false;
                        t.pnl = -(cfg.slPips * dollarPerPip);
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) continue; // skip unresolved trades
            }
        } else {
            int64_t targetExitTime = entryTime + cfg.durationSec;
            size_t j = i + 1;
            while (j < n && times[j] < targetExitTime) j++;
            if (j >= n) continue;

            t.exitTime = times[j];
            t.exitPrice = prices[j];
            t.settle();
        }

        trades.push_back(t);

        if (t.won) currentConsecLosses = 0;
        else currentConsecLosses++;
    }

    std::ofstream csv(cfg.csvOut);
    csv << "entry_time,entry_price,exit_time,exit_price,direction,won,pnl\n";
    double equity = 0.0, peak = 0.0, maxDD = 0.0;
    int wins = 0;
    int streak = 0, maxStreak = 0;
    for (auto& t : trades) {
        csv << t.entryTime << "," << t.entryPrice << "," << t.exitTime << ","
            << t.exitPrice << "," << Trade::dirName(t.direction) << ","
            << (t.won ? 1 : 0) << "," << t.pnl << "\n";
        equity += t.pnl;
        peak = std::max(peak, equity);
        maxDD = std::min(maxDD, equity - peak);
        if (t.won) wins++;

        streak = t.won ? 0 : streak + 1;
        maxStreak = std::max(maxStreak, streak);
    }
    csv.close();

    std::cout << "\n--- Last " << std::min((int)trades.size(), 20) << " Trades Ledger ---\n";
    int startIdx = std::max(0, (int)trades.size() - 20);
    for (int i = startIdx; i < (int)trades.size(); i++) {
        const auto& t = trades[i];
        std::cout << "Entry: " << t.entryPrice << " -> Exit: " << t.exitPrice 
                  << " | " << Trade::dirName(t.direction) 
                  << " | " << (t.won ? "WIN" : "LOSS")
                  << " | PnL: $" << t.pnl;
        if (isForexOrCommodity(cfg.symbol) && cfg.lotSize > 0) {
            double dollarPerPip = cfg.lotSize * 10.0;
            double pips = t.pnl / dollarPerPip;
            std::cout << " (" << pips << " pips)";
        }
        std::cout << "\n";
    }
    std::cout << "---------------------------------\n";

    if (riskControlTripped) {
        std::cout << "Risk control engaged " << skippedByRiskControl
                   << " time(s): skipped opening new trades while on a losing"
                   << " streak >= " << cfg.maxConsecLosses << ".\n";
    }

    BacktestResult result;
    result.totalTrades = (int)trades.size();
    result.wins = wins;
    result.losses = result.totalTrades - wins;
    result.netPnl = equity;
    result.maxDrawdown = maxDD;
    result.winRatePct = result.totalTrades > 0 ? (100.0 * wins / result.totalTrades) : 0.0;
    result.maxConsecLossStreak = maxStreak;
    result.tradesSkippedByRiskControl = skippedByRiskControl;
    return result;
}

void printBacktestSummary(const BacktestConfig& cfg, const BacktestResult& r) {
    std::cout << "\n===== Backtest Summary =====\n";
    std::cout << "Symbol:          " << cfg.symbol << "\n";
    std::cout << "Strategy:        " << cfg.strategyName << "   Cooldown: " << cfg.cooldownSec << "s\n";
    std::cout << "Duration:        " << cfg.durationSec << "s\n";
    std::cout << "RSI period:      " << cfg.rsiPeriod
               << " (oversold<=" << cfg.oversold << ", overbought>=" << cfg.overbought << ")\n";
    std::cout << "Stake/trade:     " << cfg.stake << "   Payout: " << (cfg.payoutPct * 100) << "%\n";
    std::cout << "Total trades:    " << r.totalTrades << "\n";
    std::cout << "Wins / Losses:   " << r.wins << " / " << r.losses << "\n";
    std::cout << "Win rate:        " << r.winRatePct << "%\n";
    std::cout << "Net P&L:         " << r.netPnl << "\n";
    std::cout << "Max drawdown:    " << r.maxDrawdown << "\n";
    std::cout << "Max loss streak: " << r.maxConsecLossStreak << " in a row\n";
    std::cout << "Trade ledger written to: " << cfg.csvOut << "\n";
    std::cout << "=============================\n";
}

// ============================================================================
// Section 6: Live paper trading (simulated only -- no buy/sell call exists)
// ============================================================================

struct PaperTradeConfig {
    std::string symbol;
    std::string strategyName = "rsi";
    int durationSec = 15;
    int rsiPeriod = 14;
    double oversold = 30.0;
    double overbought = 70.0;
    double stake = 1.0;
    double payoutPct = 0.95;
    double lotSize = 0.5;
    double tpPips = 0.0;
    double slPips = 0.0;
    double spreadPips = 0.0;
    int cooldownSec = 0;
    std::string csvOut = "paper_trades.csv";
    int maxTrades = 0;
};

namespace detail {
    std::atomic<bool> g_stopRequested{false};
    void handleSigint(int) { g_stopRequested.store(true); }
}

void runPaperTrading(DerivWsClient& client, const PaperTradeConfig& cfg,
                      std::function<Signal(int64_t, double)> strategyFn,
                      std::function<double()> rsiReaderFn) {
    std::signal(SIGINT, detail::handleSigint);

    json sub = { {"ticks", cfg.symbol}, {"subscribe", 1} };
    if (!client.sendText(sub.dump())) {
        std::cerr << "Failed to send tick subscription request." << std::endl;
        return;
    }

    std::ofstream csv(cfg.csvOut);
    csv << "entry_time,entry_price,exit_time,exit_price,direction,won,pnl\n";

    struct OpenPosition {
        int64_t entryTime;
        double entryPrice;
        Direction direction;
        int64_t targetExitTime;
        bool isTPSL;
        double tpPrice;
        double slPrice;
    };
    std::vector<OpenPosition> openPositions;

    double equity = 0.0;
    int totalTrades = 0, wins = 0;

    std::cout << "Paper trading started on " << cfg.symbol
              << " (" << cfg.durationSec << "s duration). Press Ctrl+C to stop.\n";

    while (!detail::g_stopRequested.load()) {
        std::string raw;
        if (!client.receiveText(raw)) {
            std::cerr << "Connection lost." << std::endl;
            break;
        }

        json msg;
        try { msg = json::parse(raw); } catch (...) { continue; }

        if (msg.contains("error")) {
            std::cerr << "Deriv API error: " << msg["error"].value("message", "unknown") << std::endl;
            continue;
        }
        if (!msg.contains("tick")) continue;

        auto& tick = msg["tick"];
        int64_t tickTime = tick.value("epoch", (int64_t)0);
        double tickPrice = tick.value("quote", 0.0);
        if (tickTime == 0) continue;

        for (size_t i = 0; i < openPositions.size();) {
            bool exitHit = false;
            auto& pos = openPositions[i];
            
            if (pos.isTPSL) {
                if (pos.direction == Direction::Rise) {
                    if (tickPrice >= pos.tpPrice) { pos.targetExitTime = tickTime; exitHit = true; } // TP won
                    else if (tickPrice <= pos.slPrice) { pos.targetExitTime = tickTime; exitHit = true; } // SL lost
                } else {
                    if (tickPrice <= pos.tpPrice) { pos.targetExitTime = tickTime; exitHit = true; } // TP won
                    else if (tickPrice >= pos.slPrice) { pos.targetExitTime = tickTime; exitHit = true; } // SL lost
                }
            } else {
                if (tickTime >= pos.targetExitTime) exitHit = true;
            }

            if (exitHit) {
                Trade t;
                t.entryTime = pos.entryTime;
                t.entryPrice = pos.entryPrice;
                t.exitTime = tickTime;
                t.exitPrice = tickPrice;
                t.direction = pos.direction;
                t.stake = cfg.stake;
                t.payoutPct = cfg.payoutPct;

                if (pos.isTPSL) {
                    double dollarPerPip = cfg.lotSize * 10.0;
                    if (pos.direction == Direction::Rise) {
                        t.won = (tickPrice >= pos.tpPrice);
                    } else {
                        t.won = (tickPrice <= pos.tpPrice);
                    }
                    t.pnl = t.won ? (cfg.tpPips * dollarPerPip) : -(cfg.slPips * dollarPerPip);
                } else {
                    t.settle();
                }

                csv << t.entryTime << "," << t.entryPrice << "," << t.exitTime << ","
                    << t.exitPrice << "," << Trade::dirName(t.direction) << ","
                    << (t.won ? 1 : 0) << "," << t.pnl << "\n";
                csv.flush();

                equity += t.pnl;
                totalTrades++;
                if (t.won) wins++;

                std::cout << "[SETTLED] " << Trade::dirName(t.direction)
                          << " entry=" << t.entryPrice << " exit=" << t.exitPrice
                          << " -> " << (t.won ? "WIN" : "LOSS")
                          << " pnl=" << t.pnl << " | equity=" << equity << "\n";

                openPositions.erase(openPositions.begin() + i);
            } else {
                i++;
            }
        }

        Signal sig = strategyFn(tickTime, tickPrice);
        if (sig != Signal::None) {
            OpenPosition pos;
            pos.entryTime = tickTime;
            pos.entryPrice = tickPrice;
            pos.direction = (sig == Signal::Rise) ? Direction::Rise : Direction::Fall;
            
            bool useTPSL = isForexOrCommodity(cfg.symbol) && cfg.tpPips > 0 && cfg.slPips > 0;
            pos.isTPSL = useTPSL;
            if (useTPSL) {
                double pipSize = getPipSize(cfg.symbol);
                double tpDist = cfg.tpPips * pipSize;
                double slDist = cfg.slPips * pipSize;
                if (pos.direction == Direction::Rise) {
                    pos.tpPrice = tickPrice + tpDist;
                    pos.slPrice = tickPrice - slDist;
                } else {
                    pos.tpPrice = tickPrice - tpDist;
                    pos.slPrice = tickPrice + slDist;
                }
            } else {
                pos.targetExitTime = tickTime + cfg.durationSec;
            }
            openPositions.push_back(pos);

            std::cout << "[OPENED]  " << Trade::dirName(pos.direction)
                      << " @ " << pos.entryPrice << " (RSI=" << rsiReaderFn() << ")\n";
        }

        if (cfg.maxTrades > 0 && totalTrades >= cfg.maxTrades) {
            std::cout << "Reached --max-trades limit, stopping.\n";
            break;
        }
    }

    std::cout << "\n===== Paper Trading Summary =====\n";
    std::cout << "Total trades: " << totalTrades << "   Wins: " << wins
               << "   Losses: " << (totalTrades - wins) << "\n";
    std::cout << "Win rate: " << (totalTrades > 0 ? (100.0 * wins / totalTrades) : 0.0) << "%\n";
    std::cout << "Net P&L: " << equity << "\n";
    std::cout << "Ledger written to: " << cfg.csvOut << "\n";
    std::cout << "==================================\n";
}

// ============================================================================
// Section 7: CLI entry point
// ============================================================================

static std::string resolveSymbol(const std::string& alias) {
    // Convenience aliases for the most commonly requested markets. This is
    // NOT the authoritative list -- Deriv adds/retires symbols over time, and
    // exact codes can change. Use --list-symbols to pull the live, current
    // list directly from Deriv before trusting any alias here. You can also
    // always just pass a raw Deriv symbol code directly via --symbol.
    static const std::unordered_map<std::string, std::string> aliases = {
        // Deriv synthetic indices
        {"jump10",  "JD10"},
        {"jump25",  "JD25"},
        {"jump50",  "JD50"},
        {"jump75",  "JD75"},
        {"jump100", "JD100"},
        {"step100", "stpRNG"},
        {"step",    "stpRNG"},
        {"v10",     "R_10"},
        {"v25",     "R_25"},
        {"v50",     "R_50"},
        {"v75",     "R_75"},
        {"v100",    "R_100"},
        {"volatility75", "R_75"},
        // Forex (Deriv prefixes forex pairs with "frx")
        {"eurusd",  "frxEURUSD"},
        {"gbpusd",  "frxGBPUSD"},
        {"usdjpy",  "frxUSDJPY"},
        {"audusd",  "frxAUDUSD"},
        {"audcad",  "frxAUDCAD"},
        {"usdcad",  "frxUSDCAD"},
        {"nzdusd",  "frxNZDUSD"},
        // Commodities (also "frx"-prefixed on Deriv)
        {"gold",    "frxXAUUSD"},
        {"xauusd",  "frxXAUUSD"},
        {"silver",  "frxXAGUSD"},
        {"xagusd",  "frxXAGUSD"},
        {"oil",     "frxXBRUSD"},
    };
    auto it = aliases.find(alias);
    if (it != aliases.end()) return it->second;
    return alias; // assume the user passed a raw Deriv symbol code already
}

// ----------------------------------------------------------------------------
// Live symbol discovery -- queries Deriv's actual current symbol list via the
// active_symbols API call. This is the authoritative source for "what's the
// real code for Gold / EUR-USD / Jump 200 / etc right now", since Deriv adds
// and retires instruments over time and any hardcoded alias table can go stale.
// ----------------------------------------------------------------------------

struct ActiveSymbolInfo {
    std::string symbol;
    std::string displayName;
    std::string market;
    std::string submarket;
};

bool fetchActiveSymbols(DerivWsClient& client, std::vector<ActiveSymbolInfo>& out) {
    json req = { {"active_symbols", "brief"}, {"product_type", "basic"} };
    if (!client.sendText(req.dump())) return false;

    std::string raw;
    if (!client.receiveText(raw)) return false;

    json resp;
    try { resp = json::parse(raw); } catch (const std::exception& e) {
        std::cerr << "Failed to parse active_symbols response: " << e.what() << std::endl;
        return false;
    }
    if (resp.contains("error")) {
        std::cerr << "Deriv API error: " << resp["error"].value("message", "unknown error") << std::endl;
        return false;
    }
    if (!resp.contains("active_symbols")) return false;

    for (auto& s : resp["active_symbols"]) {
        ActiveSymbolInfo info;
        info.symbol = s.value("symbol", "");
        info.displayName = s.value("display_name", "");
        info.market = s.value("market", "");
        info.submarket = s.value("submarket", "");
        out.push_back(info);
    }
    return true;
}

void printSymbolList(const std::vector<ActiveSymbolInfo>& symbols) {
    std::unordered_map<std::string, std::vector<const ActiveSymbolInfo*>> byMarket;
    for (auto& s : symbols) byMarket[s.market].push_back(&s);

    for (auto& [market, list] : byMarket) {
        std::cout << "\n[" << market << "]\n";
        for (auto* s : list) {
            std::cout << "  " << s->symbol << "  (" << s->displayName << ")\n";
        }
    }
}

// Checks whether `symbol` exists in Deriv's live list. If not, prints the
// closest matches (by substring against the code or display name) instead of
// just letting a confusing ticks_history error surface later.
bool verifySymbolOrSuggest(const std::vector<ActiveSymbolInfo>& symbols, const std::string& symbol) {
    for (auto& s : symbols) {
        if (s.symbol == symbol) return true;
    }

    std::cerr << "Symbol '" << symbol << "' was not found in Deriv's current active symbol list.\n";

    std::string lowerTarget = symbol;
    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::tolower);

    std::vector<const ActiveSymbolInfo*> matches;
    for (auto& s : symbols) {
        std::string lowerSym = s.symbol, lowerName = s.displayName;
        std::transform(lowerSym.begin(), lowerSym.end(), lowerSym.begin(), ::tolower);
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerSym.find(lowerTarget) != std::string::npos ||
            lowerName.find(lowerTarget) != std::string::npos ||
            lowerTarget.find(lowerSym) != std::string::npos) {
            matches.push_back(&s);
        }
    }

    if (!matches.empty()) {
        std::cerr << "Did you mean one of these?\n";
        for (auto* s : matches) {
            std::cerr << "  " << s->symbol << "  (" << s->displayName << ")\n";
        }
    } else {
        std::cerr << "No close matches found. Run with --list-symbols to see everything Deriv currently offers.\n";
    }
    return false;
}

static void printUsage() {
    std::cout <<
        "derivbot - Deriv synthetic index strategy backtester & paper trader\n\n"
        "Usage:\n"
        "  derivbot --mode backtest --symbol jump10 --duration 15 [options]\n"
        "  derivbot --mode paper    --symbol jump100 --duration 30 [options]\n"
        "  derivbot --train --symbols jump10,jump25 --autoadjust\n\n"
        "Symbols: jump10 jump25 jump50 jump75 jump100 step100 v75 eurusd gold etc\n"
        "  (or any raw Deriv symbol code, e.g. --symbol frxAUDCAD)\n"
        "  Run with --list-symbols to see every market Deriv currently offers,\n"
        "  pulled live -- the authoritative source, not a hardcoded guess.\n\n"
        "Strategy:\n"
        "  --strategy <rsi|confluence|ema_cross|macd|bollinger|stochastic|multi_confluence>\n"
        "                                  Entry strategy (default rsi)\n"
        "  --cooldown <sec>              Minimum seconds between fired signals (default 0).\n\n"
        "Common options:\n"
        "  --duration <15|30>      Contract duration in seconds (default 15)\n"
        "  --rsi-period <n>        RSI lookback period (default 14)\n"
        "  --oversold <n>          RSI oversold threshold -> Rise signal (default 30)\n"
        "  --overbought <n>        RSI overbought threshold -> Fall signal (default 70)\n"
        "  --stake <n>             Stake per trade (default 1.0)\n"
        "  --payout <pct>          Payout fraction on a win, e.g. 0.95 (default 0.95)\n"
        "  --count <n>             [backtest] number of historical ticks to pull (default 5000)\n"
        "  --max-trades <n>        [paper] stop after this many settled trades (default unlimited)\n"
        "  --max-consec-losses <n> [backtest] pause opening new trades after N losses in a row\n"
        "                            (0 = no limit, default 0)\n"
        "  --app-id <id>           Deriv app_id (default 1089, Deriv's public demo app id)\n"
        "  --csv <path>            Output CSV path for the trade ledger\n\n"
        "Confluence-strategy options (only used when --strategy confluence):\n"
        "  --sr-lookback <n>       Ticks each side to confirm a swing high/low (default 5)\n"
        "  --sr-tolerance <pct>    Fraction of price counted as \"at\" a level (default 0.0002 = 0.02%)\n"
        "  --sr-pullback <pct>     Fraction of price required to count as a pullback (default 0.0005 = 0.05%)\n"
        "  --sr-max-levels <n>     Levels remembered per side (default 6)\n"
        "  --sr-level-expiry <s>   Forget a level untouched for this many seconds (default 3600)\n\n"
        "Training options:\n"
        "  --train                 Enter training/optimization mode\n"
        "  --autoadjust            Enable continuous learning loop\n"
        "  --all-symbols           Train across all available Deriv symbols\n"
        "  --symbols <list>        Comma-separated list (e.g. jump10,v75,gold)\n"
        "  --train-hours <n>       Auto-stop training after N hours (default: unlimited)\n"
        "  --top-k <n>             Keep top N strategies per symbol (default: 10)\n"
        "  --train-test-split <pct> Train/test split ratio (default: 0.8)\n"
        "  --results-dir <path>    Output directory for results (default: results/)\n"
        "  --train-history <n>     Number of ticks to fetch for training (default 50000)\n"
        "  --strategies <list>     Comma-separated list of strategies to test (default all)\n"
        "  --durations <list>      Comma-separated trade durations (default 15,30,60)\n"
        "  --candles <list>        Comma-separated candle periods (default 5,10,15,30,60)\n"
        "  --no-rl                 Skip RL Meta-Learner phase entirely\n"
        "  --loop-delay <sec>      Seconds to wait between autoadjust cycles (default 0)\n\n"
        "Results Management:\n"
        "  --list-runs             List all finished training runs\n"
        "  --delete-run <id>       Delete a specific run\n"
        "  --delete-all-runs       Delete all runs\n"
        "  --load-run <id>         Load a specific run for backtest/paper trade\n"
        "  --rank <n>              Rank of strategy to load (default 1)\n\n"
        "  --help                  Show this message\n";
}

int main(int argc, char** argv) {
    std::string mode, symbolAlias, appId = "1089", csvOut;
    std::string strategyName = "rsi", trainedOn;
    int duration = 15, rsiPeriod = 14, count = 5000, maxTrades = 0, cooldownSec = 0, maxConsecLosses = 0;
    double oversold = 30.0, overbought = 70.0, stake = 1.0, payout = 0.95;

    int srLookback = 5, srMaxLevels = 6;
    double srTolerancePct = 0.0002, srPullbackPct = 0.0005;
    int64_t srLevelExpiry = 3600;

    bool autoAdjust = false, allSymbols = false;
    std::string symbolsList;
    double trainHours = 0.0, trainSplit = 0.8;
    int topK = 10;
    std::string resultsDir = "results";
    
    // Advanced training flags
    int trainHistory = 50000;
    std::string allowedStrategiesStr;
    std::string durationsStr;
    std::string candlesStr;
    bool skipRL = false;
    int loopDelay = 0;

    std::string loadRunId, deleteRunId;
    int rank = 1;
    
    std::string seedRunId;
    int seedRank = 0;
    double lotSize = 0.5;
    double tpPips = 0, slPips = 0;
    double spreadPips = 0.0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

        if (arg == "--mode") mode = next();
        else if (arg == "--symbol") symbolAlias = next();
        else if (arg == "--strategy") strategyName = next();
        else if (arg == "--cooldown") cooldownSec = std::stoi(next());
        else if (arg == "--duration") duration = std::stoi(next());
        else if (arg == "--rsi-period") rsiPeriod = std::stoi(next());
        else if (arg == "--oversold") oversold = std::stod(next());
        else if (arg == "--overbought") overbought = std::stod(next());
        else if (arg == "--stake") stake = std::stod(next());
        else if (arg == "--payout") payout = std::stod(next());
        else if (arg == "--count") count = std::stoi(next());
        else if (arg == "--max-trades") maxTrades = std::stoi(next());
        else if (arg == "--max-consec-losses") maxConsecLosses = std::stoi(next());
        else if (arg == "--app-id") appId = next();
        else if (arg == "--csv") csvOut = next();
        else if (arg == "--sr-lookback") srLookback = std::stoi(next());
        else if (arg == "--sr-tolerance") srTolerancePct = std::stod(next());
        else if (arg == "--sr-pullback") srPullbackPct = std::stod(next());
        else if (arg == "--sr-max-levels") srMaxLevels = std::stoi(next());
        else if (arg == "--sr-level-expiry") srLevelExpiry = std::stoll(next());
        else if (arg == "--train") mode = "train";
        else if (arg == "--autoadjust") autoAdjust = true;
        else if (arg == "--all-symbols") allSymbols = true;
        else if (arg == "--symbols") symbolsList = next();
        else if (arg == "--train-hours") trainHours = std::stod(next());
        else if (arg == "--top-k") topK = std::stoi(next());
        else if (arg == "--train-test-split") trainSplit = std::stod(next());
        else if (arg == "--results-dir") resultsDir = next();
        else if (arg == "--train-history") trainHistory = std::stoi(next());
        else if (arg == "--strategies") allowedStrategiesStr = next();
        else if (arg == "--durations") durationsStr = next();
        else if (arg == "--candles") candlesStr = next();
        else if (arg == "--no-rl") skipRL = true;
        else if (arg == "--loop-delay") loopDelay = std::stoi(next());
        else if (arg == "--list-runs") { mode = "list-runs"; }
        else if (arg == "--delete-run") { mode = "delete-run"; deleteRunId = next(); }
        else if (arg == "--delete-all-runs") { mode = "delete-all-runs"; }
        else if (arg == "--load-run") { loadRunId = next(); }
        else if (arg == "--rank") { rank = std::stoi(next()); }
        else if (arg == "--trained-on") { trainedOn = next(); }
        else if (arg == "--seed-run") { seedRunId = next(); }
        else if (arg == "--seed-rank") { seedRank = std::stoi(next()); }
        else if (arg == "--lot") { lotSize = std::stod(next()); }
        else if (arg == "--tp") { tpPips = std::stod(next()); }
        else if (arg == "--sl") { slPips = std::stod(next()); }
        else if (arg == "--spread") { spreadPips = std::stod(next()); }
        else if (arg == "--list-symbols") { mode = "list-symbols"; }
        else if (arg == "--help") { printUsage(); return 0; }
        else { std::cerr << "Unknown argument: " << arg << "\n"; printUsage(); return 1; }
    }

    if (mode.empty()) { printUsage(); return 1; }
    if (mode != "list-symbols" && mode != "train" && mode != "list-runs" && mode != "delete-run" && mode != "delete-all-runs" && symbolAlias.empty()) { printUsage(); return 1; }
    
    if (mode == "list-runs") {
        ResultsManager::listRuns(resultsDir);
        return 0;
    } else if (mode == "delete-run") {
        ResultsManager::deleteRun(resultsDir, deleteRunId);
        return 0;
    } else if (mode == "delete-all-runs") {
        ResultsManager::deleteAllRuns(resultsDir);
        return 0;
    }

    if (mode == "backtest" || mode == "paper") {
        if (duration != 15 && duration != 30 && duration != 60) {
            std::cerr << "This tool is scoped to 15s/30s/60s durations only. Got: " << duration << "\n";
            return 1;
        }
        if (loadRunId.empty()) {
            if (strategyName != "rsi" && strategyName != "confluence" &&
                strategyName != "ema_cross" && strategyName != "macd" &&
                strategyName != "bollinger" && strategyName != "stochastic" &&
                strategyName != "multi_confluence") {
                std::cerr << "Unknown --strategy: " << strategyName << "\n";
                return 1;
            }
        }
    }

    std::string path = "/websockets/v3?app_id=" + appId + "&l=EN";
    DerivWsClient client;
    std::cout << "Connecting to wss://ws.derivws.com" << path << " ...\n";
    if (!client.connect("ws.derivws.com", "443", path)) {
        std::cerr << "Failed to connect to Deriv WebSocket API.\n";
        return 1;
    }

    if (mode == "list-symbols") {
        std::vector<ActiveSymbolInfo> symbols;
        std::cout << "Fetching live symbol list from Deriv...\n";
        if (!fetchActiveSymbols(client, symbols)) {
            std::cerr << "Failed to fetch active symbols.\n";
            return 1;
        }
        printSymbolList(symbols);
        return 0;
    }

    if (mode == "train") {
        std::vector<std::string> targets;
        if (allSymbols) {
            std::vector<ActiveSymbolInfo> symbols;
            if (!fetchActiveSymbols(client, symbols)) return 1;
            for (auto& s : symbols) targets.push_back(s.symbol);
        } else {
            for (auto& s : parseCsvString(symbolsList)) {
                targets.push_back(resolveSymbol(s));
            }
        }

        TrainConfig tCfg;
        tCfg.symbols = targets;
        tCfg.historyCount = trainHistory;
        tCfg.topK = topK;
        tCfg.trainSplit = trainSplit;
        tCfg.durationSec = duration;
        tCfg.stake = stake;
        tCfg.payoutPct = payout;
        tCfg.lotSize = lotSize;
        tCfg.spreadPips = spreadPips;
        tCfg.resultsDir = resultsDir;
        tCfg.autoAdjust = autoAdjust;
        tCfg.trainHours = trainHours;
        tCfg.seedRunId = seedRunId;
        tCfg.seedRank = seedRank;
        
        if (!allowedStrategiesStr.empty()) tCfg.allowedStrategies = parseCsvString(allowedStrategiesStr);
        if (!durationsStr.empty()) tCfg.durations = parseCsvDouble(durationsStr);
        if (!candlesStr.empty()) tCfg.candlePeriods = parseCsvDouble(candlesStr);
        tCfg.skipRL = skipRL;
        tCfg.baseTp = tpPips;
        tCfg.baseSl = slPips;

        std::string runDir = ReportGenerator::createResultsDir(tCfg.resultsDir);
        std::cout << "Results will be saved to: " << runDir << "\n";
        auto startTime = std::chrono::steady_clock::now();

        do {
            DerivWsClient client;
            std::string path = "/websockets/v3?app_id=" + appId + "&l=EN";
            if (!client.connect("ws.derivws.com", "443", path)) {
                std::cerr << "Failed to connect to Deriv WebSocket API for training.\n";
            } else {
                if (tCfg.symbols.empty()) {
                    if (allSymbols) {
                        std::vector<ActiveSymbolInfo> symbols;
                        if (!fetchActiveSymbols(client, symbols)) return 1;
                        for (auto& s : symbols) tCfg.symbols.push_back(s.symbol);
                    } else {
                        std::stringstream ss(symbolsList);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            tCfg.symbols.push_back(resolveSymbol(item));
                        }
                    }
                    if (tCfg.symbols.empty()) {
                        std::cerr << "No symbols specified for training.\n";
                        return 1;
                    }
                }

                TrainEngine engine(client, tCfg);
                engine.run(runDir);
                client.close();
            }

            if (!autoAdjust) break; // single pass

            if (trainHours > 0) {
                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<double> elapsed = now - startTime;
                if (elapsed.count() >= trainHours * 3600.0) {
                    std::cout << "\nReached --train-hours limit (" << trainHours << "h). Stopping auto-adjust.\n";
                    break;
                }
            }

            if (!detail::g_stopRequested.load() && loopDelay > 0) {
                std::cout << "\nAuto-adjust enabled. Waiting " << loopDelay << " seconds before re-fetching and refining...\n";
                for (int i = 0; i < loopDelay && !detail::g_stopRequested.load(); i++) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        } while (!detail::g_stopRequested.load());

        std::cout << "\nTraining session complete.\n";
        return 0;
    }

    std::string symbol = resolveSymbol(symbolAlias);
    std::cout << "Connected. Symbol resolved to: " << symbol << "\n";

    {
        std::vector<ActiveSymbolInfo> symbols;
        if (fetchActiveSymbols(client, symbols)) {
            if (!verifySymbolOrSuggest(symbols, symbol)) {
                return 1;
            }
        } else {
            std::cerr << "Warning: could not verify symbol against Deriv's live list, proceeding anyway.\n";
        }
    }

    // Build whichever strategy was requested, then wrap its signal output
    // through the cooldown gate so both strategies get the same protection
    // against re-firing on the same correlated price wiggle.
    std::unique_ptr<RsiThresholdStrategy> rsiStrat;
    std::unique_ptr<ConfluenceStrategy> confStrat;
    std::unique_ptr<StrategyBase> genericStrat;
    CooldownGate cooldown(cooldownSec);

    std::function<Signal(int64_t, double)> strategyFn;
    std::function<double()> rsiReaderFn;

    if (!loadRunId.empty()) {
        std::string loadSymbol = trainedOn.empty() ? symbol : resolveSymbol(trainedOn);
        genericStrat = ResultsManager::loadStrategy(resultsDir, loadRunId, loadSymbol, rank, strategyName, &duration, &tpPips, &slPips);
        if (!genericStrat) return 1;

        StrategyBase* ptr = genericStrat.get();
        strategyFn = [&cooldown, ptr](int64_t t, double p) {
            return cooldown.filter(t, ptr->onPrice(t, p));
        };
        rsiReaderFn = [ptr]() { return ptr->lastRsi(); };
    } else if (strategyName == "confluence") {
        ConfluenceStrategy::Config ccfg;
        ccfg.rsiPeriod = rsiPeriod;
        ccfg.oversold = oversold;
        ccfg.overbought = overbought;
        ccfg.swingLookback = srLookback;
        ccfg.levelTolerancePct = srTolerancePct;
        ccfg.pullbackPct = srPullbackPct;
        ccfg.maxLevels = srMaxLevels;
        ccfg.levelExpirySec = srLevelExpiry;
        confStrat = std::make_unique<ConfluenceStrategy>(ccfg);

        ConfluenceStrategy* ptr = confStrat.get();
        strategyFn = [&cooldown, ptr](int64_t t, double p) {
            return cooldown.filter(t, ptr->onPrice(t, p));
        };
        rsiReaderFn = [ptr]() { return ptr->lastRsi(); };
    } else if (strategyName == "rsi") {
        rsiStrat = std::make_unique<RsiThresholdStrategy>(rsiPeriod, oversold, overbought);

        RsiThresholdStrategy* ptr = rsiStrat.get();
        strategyFn = [&cooldown, ptr](int64_t t, double p) {
            return cooldown.filter(t, ptr->onPrice(p));
        };
        rsiReaderFn = [ptr]() { return ptr->lastRsi(); };
    } else {
        if (strategyName == "ema_cross") genericStrat = std::make_unique<EmaCrossoverStrategy>();
        else if (strategyName == "macd") genericStrat = std::make_unique<MacdStrategy>();
        else if (strategyName == "bollinger") genericStrat = std::make_unique<BollingerStrategy>();
        else if (strategyName == "stochastic") genericStrat = std::make_unique<StochasticStrategy>();
        else if (strategyName == "multi_confluence") genericStrat = std::make_unique<MultiConfluenceStrategy>(MultiConfluenceStrategy::Config{});

        StrategyBase* ptr = genericStrat.get();
        strategyFn = [&cooldown, ptr](int64_t t, double p) {
            return cooldown.filter(t, ptr->onPrice(t, p));
        };
        rsiReaderFn = [ptr]() { return ptr->lastRsi(); };
    }

    if (mode == "backtest") {
        BacktestConfig cfg;
        cfg.symbol = symbol;
        cfg.strategyName = strategyName;
        cfg.historyCount = count;
        cfg.durationSec = duration;
        cfg.rsiPeriod = rsiPeriod;
        cfg.oversold = oversold;
        cfg.overbought = overbought;
        cfg.stake = stake;
        cfg.payoutPct = payout;
        cfg.cooldownSec = cooldownSec;
        cfg.maxConsecLosses = maxConsecLosses;
        cfg.tpPips = tpPips;
        cfg.slPips = slPips;
        cfg.lotSize = lotSize;
        if (!csvOut.empty()) cfg.csvOut = csvOut;

        std::vector<int64_t> times;
        std::vector<double> prices;
        std::cout << "Fetching " << cfg.historyCount << " historical ticks for " << symbol << "...\n";
        if (!fetchTickHistory(client, symbol, cfg.historyCount, times, prices)) {
            std::cerr << "Failed to fetch tick history.\n";
            return 1;
        }
        std::cout << "Fetched " << prices.size() << " ticks. Running backtest...\n";

        BacktestResult result = runBacktest(cfg, times, prices, strategyFn);
        printBacktestSummary(cfg, result);

        if (confStrat) {
            auto& d = confStrat->diagnostics();
            std::cout << "\n--- Confluence funnel diagnostics ---\n";
            std::cout << "Swing highs found:        " << d.swingHighsFound << "\n";
            std::cout << "Swing lows found:         " << d.swingLowsFound << "\n";
            std::cout << "Resistance: approached=" << d.resistanceApproached
                       << " pulledBack=" << d.resistancePulledBack
                       << " retested=" << d.resistanceRetested
                       << " fired=" << d.resistanceFired << "\n";
            std::cout << "Support:    approached=" << d.supportApproached
                       << " pulledBack=" << d.supportPulledBack
                       << " retested=" << d.supportRetested
                       << " fired=" << d.supportFired << "\n";
            std::cout << "If 'approached' is 0: --sr-tolerance is too tight, or this\n"
                       << "  symbol's price almost never revisits old swing levels.\n"
                       << "If 'approached' > 0 but 'pulledBack' is 0: --sr-pullback is too\n"
                       << "  large for this instrument's typical price movement.\n"
                       << "If 'retested' > 0 but 'fired' is 0: the retest pattern does\n"
                       << "  happen, but RSI is never in the extreme zone exactly when it\n"
                       << "  does -- try widening --oversold/--overbought toward 50, or\n"
                       << "  accept that this confluence doesn't line up for this symbol.\n"
                       << "--------------------------------------\n";
        }

    } else if (mode == "paper") {
        PaperTradeConfig cfg;
        cfg.symbol = symbol;
        cfg.strategyName = strategyName;
        cfg.durationSec = duration;
        cfg.rsiPeriod = rsiPeriod;
        cfg.oversold = oversold;
        cfg.overbought = overbought;
        cfg.stake = stake;
        cfg.payoutPct = payout;
        cfg.cooldownSec = cooldownSec;
        cfg.maxTrades = maxTrades;
        cfg.tpPips = tpPips;
        cfg.slPips = slPips;
        cfg.lotSize = lotSize;
        if (!csvOut.empty()) cfg.csvOut = csvOut;

        runPaperTrading(client, cfg, strategyFn, rsiReaderFn);

    } else {
        std::cerr << "Unknown --mode: " << mode << " (expected 'backtest' or 'paper')\n";
        return 1;
    }

    return 0;
}