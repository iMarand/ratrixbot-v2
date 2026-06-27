// ============================================================================
// derivbot.cpp - Deriv synthetic-index RSI backtester & paper trader
//
// Single-file build using Boost.Beast (WebSocket+TLS) and nlohmann::json.
//
// SCOPE: backtest and paper modes only ever send "ticks_history" (historical
// data) and "ticks"+"subscribe" (live feed) requests -- no orders, no token,
// nothing at risk.
//
// The "--mode live" path DOES place real orders: it authorizes with a Deriv
// API token (--token) and buys MULTUP/MULTDOWN multiplier contracts with
// stop-loss / take-profit, plus a bot-side trailing stop. This is intended for
// a Deriv *demo (virtual)* account token so it can run unattended on a Linux
// VPS. Use a demo token. Real-money tokens trade real money -- your risk.
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
#include "strategies/random_forest.hpp"
#include "strategies/xgboost_strategy.hpp"
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
#include <cstdlib>

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

    // Rebound (exit-of-zone) entry. Instead of firing the instant RSI touches
    // an extreme, we ARM when price pushes RSI into the zone, then fire only
    // once RSI rebounds back out of the zone -- i.e. the move is actually
    // reversing. Overbought -> rebound down -> SELL (Fall); oversold ->
    // rebound up -> BUY (Rise). This tends to avoid entering against a strong
    // trend that just keeps pushing the extreme further.
    Signal onPrice(double price) {
        auto value = rsi_.update(price);
        if (!value.has_value()) return Signal::None;

        double rsiVal = *value;
        Signal signal = Signal::None;

        // Overbought rebound -> SELL
        if (rsiVal >= overbought_) {
            inOverbought_ = true;
        } else if (inOverbought_) {
            // RSI has rebounded back below the overbought line
            signal = Signal::Fall;
            inOverbought_ = false;
        }

        // Oversold rebound -> BUY
        if (rsiVal <= oversold_) {
            inOversold_ = true;
        } else if (inOversold_) {
            // RSI has rebounded back above the oversold line
            if (signal == Signal::None) signal = Signal::Rise;
            inOversold_ = false;
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
    bool inOversold_ = false;
    bool inOverbought_ = false;
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
    size_t  entryIdx = 0;   // index into the tick arrays (for horizon analysis)
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
                            std::function<Signal(int64_t, double)> strategyFn,
                            std::vector<Trade>* outTrades = nullptr) {
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
        t.entryIdx = i;
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
            double dollarPerPip = cfg.lotSize * 10.0;
            
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
    if (outTrades) *outTrades = trades;
    return result;
}

// ============================================================================
// Multi-horizon "what happened after entry" analysis.
//
// For every trade taken, re-checks the outcome if the contract had instead
// expired at +10s / +15s / +30s / +60s. This answers the exact question:
// "we lost at the chosen duration -- but did the market move into our favour a
// few seconds later?" If many losers would have won at a longer horizon, the
// duration is too short (or entries are slightly early); if a shorter horizon
// already wins more, holding longer is just giving profit back.
// ============================================================================

void analyzeHorizons(const std::vector<int64_t>& times,
                     const std::vector<double>& prices,
                     const std::vector<Trade>& trades,
                     const BacktestConfig& cfg) {
    if (trades.empty()) return;

    const std::vector<int> horizons = {10, 15, 30, 60};
    size_t n = prices.size();

    auto outcomeAt = [&](const Trade& t, int horizon, bool& resolved) -> bool {
        int64_t target = t.entryTime + horizon;
        size_t j = t.entryIdx + 1;
        while (j < n && times[j] < target) j++;
        if (j >= n) { resolved = false; return false; }
        resolved = true;
        bool up = prices[j] > t.entryPrice;
        bool down = prices[j] < t.entryPrice;
        return (t.direction == Direction::Rise) ? up : down; // tie = loss
    };

    std::cout << "\n--- Multi-horizon outcome analysis (chosen duration = "
              << cfg.durationSec << "s) ---\n";
    std::cout << "Horizon |  trades |  wins | win% |   net P&L | vs chosen: rescued losers\n";

    for (int h : horizons) {
        int resolvedTrades = 0, wins = 0, rescued = 0;
        double net = 0.0;
        for (const auto& t : trades) {
            bool resolved = false;
            bool won = outcomeAt(t, h, resolved);
            if (!resolved) continue;
            resolvedTrades++;
            if (won) wins++;
            net += won ? (cfg.stake * cfg.payoutPct) : -cfg.stake;
            // "Rescued": this trade LOST at the chosen duration but WINS at
            // this horizon -- i.e. holding to +h seconds would have saved it.
            if (!t.won && won && h > cfg.durationSec) rescued++;
        }
        double wr = resolvedTrades ? (100.0 * wins / resolvedTrades) : 0.0;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "  %3ds  |  %5d  | %5d | %4.1f | %+9.2f | %s%d\n",
                      h, resolvedTrades, wins, wr, net,
                      (h == cfg.durationSec ? "(chosen)  " : "          "), rescued);
        std::cout << line;
    }

    // Recommend the horizon with the best net P&L.
    int bestH = cfg.durationSec;
    double bestNet = -1e18;
    for (int h : horizons) {
        double net = 0.0;
        for (const auto& t : trades) {
            bool resolved = false;
            bool won = outcomeAt(t, h, resolved);
            if (!resolved) continue;
            net += won ? (cfg.stake * cfg.payoutPct) : -cfg.stake;
        }
        if (net > bestNet) { bestNet = net; bestH = h; }
    }
    std::cout << "Best duration by net P&L on this data: " << bestH << "s ("
              << (bestH == cfg.durationSec ? "matches your choice"
                                           : "consider switching to this")
              << ").\n";
    std::cout << "Reading: a high 'rescued losers' count at a longer horizon means\n"
              << "your entries are right but the contract expires too soon.\n"
              << "------------------------------------------------------------\n";
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
// Section 6b: LIVE trading on Deriv via multiplier contracts (real demo orders)
//
// Runs natively on a Linux VPS (no MT5, no Wine). Authorizes with an API token,
// streams ticks, runs the chosen strategy, and on a signal buys a MULTUP/
// MULTDOWN multiplier contract on the symbol with a fixed stop-loss/take-profit
// (calculated risk) plus a bot-side trailing stop that locks profit once the
// trade is in the money. One position at a time, with cooldown / max-trades /
// max-consecutive-loss / daily-loss guards -- the same discipline as the EA.
// ============================================================================

struct LiveTradeConfig {
    std::string symbol;
    std::string strategyName = "rsi";
    std::string token;
    double stake        = 1.0;     // stake per contract (account currency)
    int    multiplier   = 100;     // multiplier (e.g. gold: 50-150)
    double slAmount     = 0.0;     // stop-loss as money amount (0 = none)
    double tpAmount     = 0.0;     // take-profit as money amount (0 = none)
    double beAmount     = 0.0;     // profit at which trailing arms (0 = off)
    double trailAmount  = 0.0;     // giveback from peak profit that closes it
    int    cooldownSec  = 0;
    int    maxTrades    = 0;        // 0 = unlimited (per session)
    int    maxConsecLosses = 0;     // 0 = off
    double maxDailyLoss = 0.0;      // 0 = off (session loss cap)
    std::string csvOut  = "live_trades.csv";
};

// Read messages until one of the given msg_type arrives (or an error). Other
// messages seen in the meantime are dropped (used only briefly, while flat).
static bool recvUntilType(DerivWsClient& client, const std::string& type, json& out) {
    for (int i = 0; i < 50; i++) {
        std::string raw;
        if (!client.receiveText(raw)) return false;
        json msg;
        try { msg = json::parse(raw); } catch (...) { continue; }
        if (msg.contains("error")) {
            std::cerr << "Deriv API error: " << msg["error"].value("message", "unknown") << "\n";
            out = msg;
            return false;
        }
        if (msg.value("msg_type", "") == type) { out = msg; return true; }
    }
    return false;
}

void runLiveTrading(DerivWsClient& client, const LiveTradeConfig& cfg,
                    std::function<Signal(int64_t, double)> strategyFn,
                    std::function<double()> rsiReaderFn) {
    std::signal(SIGINT, detail::handleSigint);

    // 1. Authorize ----------------------------------------------------------
    if (!client.sendText(json({{"authorize", cfg.token}}).dump())) {
        std::cerr << "Failed to send authorize.\n"; return;
    }
    json auth;
    if (!recvUntilType(client, "authorize", auth)) {
        std::cerr << "Authorization failed. Check your --token.\n"; return;
    }
    std::string currency = auth["authorize"].value("currency", "USD");
    std::string loginid  = auth["authorize"].value("loginid", "?");
    double balance       = auth["authorize"].value("balance", 0.0);
    bool isVirtual       = auth["authorize"].value("is_virtual", 0) == 1;
    std::cout << "Authorized as " << loginid << " (" << currency << ") balance="
              << balance << (isVirtual ? "  [VIRTUAL/DEMO]" : "  [REAL MONEY]") << "\n";
    if (!isVirtual) {
        std::cout << "WARNING: this is a REAL-money account. Ctrl+C now if that was not intended.\n";
    }

    // 2. Subscribe to ticks -------------------------------------------------
    if (!client.sendText(json({{"ticks", cfg.symbol}, {"subscribe", 1}}).dump())) {
        std::cerr << "Failed to subscribe ticks.\n"; return;
    }

    std::ofstream csv(cfg.csvOut, std::ios::app);
    if (csv.tellp() == 0) csv << "open_time,dir,stake,multiplier,close_profit,balance\n";

    // Session state
    bool   inPosition = false;
    int64_t contractId = 0;
    Direction posDir = Direction::Rise;
    double peakProfit = 0.0;
    int64_t lastEntryTime = INT64_MIN / 2;
    int    tradesOpened = 0, wins = 0;
    int    consecLosses = 0;
    double sessionPnl = 0.0;

    auto riskBlocksEntry = [&](int64_t now) -> bool {
        if (inPosition) return true;
        if (cfg.cooldownSec > 0 && (now - lastEntryTime) < cfg.cooldownSec) return true;
        if (cfg.maxConsecLosses > 0 && consecLosses >= cfg.maxConsecLosses) return true;
        if (cfg.maxTrades > 0 && tradesOpened >= cfg.maxTrades) return true;
        if (cfg.maxDailyLoss > 0.0 && sessionPnl <= -cfg.maxDailyLoss) return true;
        return false;
    };

    auto openContract = [&](bool up, int64_t now) {
        json params = {
            {"amount", cfg.stake},
            {"basis", "stake"},
            {"contract_type", up ? "MULTUP" : "MULTDOWN"},
            {"currency", currency},
            {"symbol", cfg.symbol},
            {"multiplier", cfg.multiplier}
        };
        json limit = json::object();
        if (cfg.slAmount > 0.0) limit["stop_loss"]   = cfg.slAmount;
        if (cfg.tpAmount > 0.0) limit["take_profit"] = cfg.tpAmount;
        if (!limit.empty()) params["limit_order"] = limit;

        json buy = {{"buy", 1}, {"price", cfg.stake}, {"parameters", params}};
        if (client.sendText(buy.dump())) {
            posDir = up ? Direction::Rise : Direction::Fall;
            lastEntryTime = now;
            std::cout << "[BUY] " << (up ? "MULTUP" : "MULTDOWN") << " stake=" << cfg.stake
                      << " x" << cfg.multiplier << " (RSI=" << rsiReaderFn() << ")\n";
        }
    };

    std::cout << "Live trading on " << cfg.symbol << " with strategy '" << cfg.strategyName
              << "'. Press Ctrl+C to stop.\n";

    // 3. Event loop ---------------------------------------------------------
    while (!detail::g_stopRequested.load()) {
        std::string raw;
        if (!client.receiveText(raw)) { std::cerr << "Connection lost.\n"; break; }
        json msg;
        try { msg = json::parse(raw); } catch (...) { continue; }

        if (msg.contains("error")) {
            std::cerr << "Deriv API error: " << msg["error"].value("message", "unknown") << "\n";
            continue;
        }
        std::string type = msg.value("msg_type", "");

        if (type == "tick") {
            auto& tick = msg["tick"];
            int64_t t = tick.value("epoch", (int64_t)0);
            double  p = tick.value("quote", 0.0);
            if (t == 0) continue;

            Signal sig = strategyFn(t, p);
            if (sig != Signal::None && !riskBlocksEntry(t)) {
                openContract(sig == Signal::Rise, t);
            }
        }
        else if (type == "buy") {
            contractId = msg["buy"].value("contract_id", (int64_t)0);
            if (contractId != 0) {
                inPosition = true;
                peakProfit = 0.0;
                tradesOpened++;
                std::cout << "[OPEN] contract " << contractId
                          << " buy_price=" << msg["buy"].value("buy_price", 0.0) << "\n";
                // Subscribe to this contract's live updates.
                client.sendText(json({{"proposal_open_contract", 1},
                                       {"contract_id", contractId}, {"subscribe", 1}}).dump());
            }
        }
        else if (type == "proposal_open_contract") {
            auto& poc = msg["proposal_open_contract"];
            if (poc.is_null() || poc.value("contract_id", (int64_t)0) != contractId) continue;

            double profit = poc.value("profit", 0.0);
            bool   isSold = poc.value("is_sold", 0) == 1;

            if (!isSold && inPosition) {
                // Bot-side trailing stop: once profit reaches the break-even
                // threshold, close if it gives back trailAmount from the peak.
                peakProfit = std::max(peakProfit, profit);
                if (cfg.beAmount > 0.0 && cfg.trailAmount > 0.0 &&
                    peakProfit >= cfg.beAmount && profit <= peakProfit - cfg.trailAmount) {
                    std::cout << "[TRAIL] locking profit, selling contract " << contractId
                              << " (peak=" << peakProfit << " now=" << profit << ")\n";
                    client.sendText(json({{"sell", contractId}, {"price", 0}}).dump());
                }
            }

            if (isSold) {
                bool won = profit > 0.0;
                sessionPnl += profit;
                if (won) { wins++; consecLosses = 0; } else { consecLosses++; }
                balance += profit;
                csv << poc.value("date_start", (int64_t)0) << ","
                    << Trade::dirName(posDir) << "," << cfg.stake << ","
                    << cfg.multiplier << "," << profit << "," << balance << "\n";
                csv.flush();
                std::cout << "[CLOSED] contract " << contractId << " profit=" << profit
                          << " -> " << (won ? "WIN" : "LOSS")
                          << " | session P&L=" << sessionPnl
                          << " | streak losses=" << consecLosses << "\n";
                inPosition = false;
                contractId = 0;
            }
        }
    }

    std::cout << "\n===== Live Session Summary =====\n";
    std::cout << "Trades: " << tradesOpened << "   Wins: " << wins
              << "   Win rate: " << (tradesOpened > 0 ? (100.0 * wins / tradesOpened) : 0.0) << "%\n";
    std::cout << "Session P&L: " << sessionPnl << "   Balance: " << balance << "\n";
    std::cout << "Ledger: " << cfg.csvOut << "\n";
    std::cout << "================================\n";
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
        // Boom & Crash indices
        {"boom1000", "BOOM1000"},
        {"boom500",  "BOOM500"},
        {"boom300",  "BOOM300N"},
        {"crash1000","CRASH1000"},
        {"crash500", "CRASH500"},
        {"crash300", "CRASH300N"},
        // Bull & Bear market indices
        {"bull",     "RDBULL"},
        {"bullmarket","RDBULL"},
        {"bear",     "RDBEAR"},
        {"bearmarket","RDBEAR"},
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
        "  derivbot --mode live     --symbol gold --token <demo_token> --strategy random_forest [options]\n"
        "  derivbot --train --symbols jump10,jump25 --autoadjust\n\n"
        "Live trading (real DEMO orders via Deriv multiplier contracts; Linux-VPS friendly):\n"
        "  --token <api_token>     Deriv API token (use a DEMO/virtual token!). Required for --mode live\n"
        "  --multiplier <n>        Multiplier for the contract (e.g. gold 50-150, default 100)\n"
        "  --stake <amt>           Stake per contract (account currency)\n"
        "  --sl-amount <money>     Stop-loss as a money amount (calculated risk)\n"
        "  --tp-amount <money>     Take-profit as a money amount\n"
        "  --be-amount <money>     Profit at which the trailing stop arms\n"
        "  --trail-amount <money>  Giveback from peak profit that closes the trade (locks profit)\n"
        "  --daily-loss <money>    Stop opening trades after this session loss\n"
        "  --cooldown <sec>        Min seconds between entries\n"
        "  --max-consec-losses <n> Pause after N losses in a row\n"
        "  --max-trades <n>        Stop after N trades this session\n\n"
        "Symbols: jump10 jump25 jump50 jump75 jump100 step100 v75 eurusd gold etc\n"
        "  (or any raw Deriv symbol code, e.g. --symbol frxAUDCAD)\n"
        "  Run with --list-symbols to see every market Deriv currently offers,\n"
        "  pulled live -- the authoritative source, not a hardcoded guess.\n\n"
        "Strategy:\n"
        "  --strategy <rsi|confluence|ema_cross|macd|bollinger|stochastic|\n"
        "              multi_confluence|random_forest|xgboost>\n"
        "                                  Entry strategy (default rsi)\n"
        "  --cooldown <sec>              Minimum seconds between fired signals (default 0).\n\n"
        "Common options:\n"
        "  --duration <10|15|30|60> Contract duration in seconds (default 15)\n"
        "  --rsi-period <n>        RSI lookback period (default 14)\n"
        "  --oversold <n>          RSI oversold line; rebound back ABOVE it -> Rise (default 30)\n"
        "  --overbought <n>        RSI overbought line; rebound back BELOW it -> Fall (default 70)\n"
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

    // Live-trading (Deriv multiplier) options
    std::string apiToken;
    int    multiplier = 100;
    double slAmount = 0.0, tpAmount = 0.0, beAmount = 0.0, trailAmount = 0.0;
    double dailyLoss = 0.0;

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
        else if (arg == "--token") { apiToken = next(); }
        else if (arg == "--multiplier") { multiplier = std::stoi(next()); }
        else if (arg == "--sl-amount") { slAmount = std::stod(next()); }
        else if (arg == "--tp-amount") { tpAmount = std::stod(next()); }
        else if (arg == "--be-amount") { beAmount = std::stod(next()); }
        else if (arg == "--trail-amount") { trailAmount = std::stod(next()); }
        else if (arg == "--daily-loss") { dailyLoss = std::stod(next()); }
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

    if (mode == "backtest" || mode == "paper" || mode == "live") {
        if ((mode == "backtest" || mode == "paper") &&
            duration != 10 && duration != 15 && duration != 30 && duration != 60) {
            std::cerr << "This tool is scoped to 10s/15s/30s/60s durations only. Got: " << duration << "\n";
            return 1;
        }
        if (loadRunId.empty()) {
            if (strategyName != "rsi" && strategyName != "confluence" &&
                strategyName != "ema_cross" && strategyName != "macd" &&
                strategyName != "bollinger" && strategyName != "stochastic" &&
                strategyName != "multi_confluence" &&
                strategyName != "random_forest" && strategyName != "xgboost") {
                std::cerr << "Unknown --strategy: " << strategyName << "\n";
                return 1;
            }
        }
    }
    // Allow the token to come from the environment (DERIV_TOKEN) so it never
    // has to appear on the command line / process list (used by the dashboard).
    if (apiToken.empty()) {
        const char* envTok = std::getenv("DERIV_TOKEN");
        if (envTok && *envTok) apiToken = envTok;
    }
    if (mode == "live" && apiToken.empty()) {
        std::cerr << "--mode live requires --token <deriv_api_token> or the DERIV_TOKEN env var (use a DEMO token).\n";
        return 1;
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
        // ML strategies self-train on the first ~300 candles, then predict. With
        // no dedicated CLI flags, use solid defaults; tune them via --train.
        else if (strategyName == "random_forest")
            genericStrat = std::make_unique<RandomForestStrategy>(/*candlePeriod*/15, /*numTrees*/30, /*maxDepth*/4, /*minSamples*/10, /*voteThreshold*/0.67, duration);
        else if (strategyName == "xgboost")
            genericStrat = std::make_unique<XGBoostStrategy>(/*candlePeriod*/15, /*nRounds*/40, /*maxDepth*/3, /*learningRate*/0.1, /*probMargin*/0.10, duration);

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

        std::vector<Trade> trades;
        BacktestResult result = runBacktest(cfg, times, prices, strategyFn, &trades);
        printBacktestSummary(cfg, result);

        // Only meaningful for duration-based (synthetic) exits, not TP/SL.
        bool usedTPSL = isForexOrCommodity(cfg.symbol) && cfg.tpPips > 0 && cfg.slPips > 0;
        if (!usedTPSL) analyzeHorizons(times, prices, trades, cfg);

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

    } else if (mode == "live") {
        LiveTradeConfig cfg;
        cfg.symbol = symbol;
        cfg.strategyName = strategyName;
        cfg.token = apiToken;
        cfg.stake = stake;
        cfg.multiplier = multiplier;
        cfg.slAmount = slAmount;
        cfg.tpAmount = tpAmount;
        cfg.beAmount = beAmount;
        cfg.trailAmount = trailAmount;
        cfg.cooldownSec = cooldownSec;
        cfg.maxTrades = maxTrades;
        cfg.maxConsecLosses = maxConsecLosses;
        cfg.maxDailyLoss = dailyLoss;
        if (!csvOut.empty()) cfg.csvOut = csvOut;

        runLiveTrading(client, cfg, strategyFn, rsiReaderFn);

    } else {
        std::cerr << "Unknown --mode: " << mode << " (expected 'backtest', 'paper' or 'live')\n";
        return 1;
    }

    return 0;
}