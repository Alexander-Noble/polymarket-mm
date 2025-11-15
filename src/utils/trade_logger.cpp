#include "utils/trade_logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace pmm {

TradeLogger::TradeLogger(const std::filesystem::path& log_dir, const std::string& session_name)
    : log_dir_(log_dir) {
    
    ensureLogDir();
    
    // Generate session ID
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "session_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    if (!session_name.empty()) {
        ss << "_" << session_name;
    }
    session_id_ = ss.str();
    
    session_start_ = now;
    
    std::cout << "TradeLogger initialized - Session: " << session_id_ << "\n";
}

TradeLogger::~TradeLogger() {
    endSession();
}

void TradeLogger::ensureLogDir() {
    if (!std::filesystem::exists(log_dir_)) {
        std::filesystem::create_directories(log_dir_);
        std::cout << "Created log directory: " << log_dir_ << "\n";
    }
}

void TradeLogger::initializeFiles() {
    session_dir_ = log_dir_ / session_id_;
    std::cout << "Creating session directory: " << session_dir_ << "\n";
    std::filesystem::create_directories(session_dir_);
    
    // Open CSV files with headers
    orders_file_.open(session_dir_ / "orders.csv");
    orders_file_ << "timestamp,market_id,order_id,token_id,side,price,size,status\n";
    
    fills_file_.open(session_dir_ / "fills.csv");
    fills_file_ << "timestamp,market_id,order_id,token_id,side,fill_price,fill_size,pnl\n";
    
    positions_file_.open(session_dir_ / "positions.csv");
    positions_file_ << "timestamp,market_id,token_id,position,avg_cost,market_value,unrealized_pnl\n";
    
    pnl_file_.open(session_dir_ / "pnl.csv");
    pnl_file_ << "timestamp,market_id,token_id,realized_pnl,unrealized_pnl\n";
    
    metrics_file_.open(session_dir_ / "metrics.csv");
    metrics_file_ << "timestamp,total_trades,total_volume,realized_pnl,winning_trades,losing_trades\n";
    
    std::cout << "Created session directory: " << session_dir_ << "\n";
}

void TradeLogger::closeFiles() {
    if (orders_file_.is_open()) orders_file_.close();
    if (fills_file_.is_open()) fills_file_.close();
    if (positions_file_.is_open()) positions_file_.close();
    if (pnl_file_.is_open()) pnl_file_.close();
    if (metrics_file_.is_open()) metrics_file_.close();
}

std::string TradeLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

void TradeLogger::startSession(const std::string& event_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    event_name_ = event_name;
    session_start_ = std::chrono::system_clock::now();
    
    initializeFiles();
    
    std::cout << "Session started: " << session_id_ << " for event: " << event_name << "\n";
}

void TradeLogger::endSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!orders_file_.is_open()) {
        return;  // Session never started
    }
    
    // Save final metrics
    auto metrics = getSessionMetrics();
    
    // Write summary to JSON
    nlohmann::json summary;
    summary["session_id"] = session_id_;
    summary["event_name"] = event_name_;
    summary["start_time"] = std::chrono::system_clock::to_time_t(metrics.start_time);
    summary["end_time"] = std::chrono::system_clock::to_time_t(metrics.end_time);
    summary["uptime_seconds"] = metrics.uptime.count();
    summary["total_trades"] = metrics.total_trades;
    summary["total_volume"] = metrics.total_volume;
    summary["realized_pnl"] = metrics.realized_pnl;
    summary["unrealized_pnl"] = metrics.unrealized_pnl;
    summary["total_pnl"] = metrics.total_pnl;
    summary["winning_trades"] = metrics.winning_trades;
    summary["losing_trades"] = metrics.losing_trades;
    summary["win_rate"] = metrics.total_trades > 0 ? 
                          (double)metrics.winning_trades / metrics.total_trades : 0.0;
    summary["avg_win"] = metrics.avg_win;
    summary["avg_loss"] = metrics.avg_loss;
    summary["largest_win"] = metrics.largest_win;
    summary["largest_loss"] = metrics.largest_loss;
    
    std::ofstream summary_file(session_dir_ / "session_summary.json");
    summary_file << std::setw(2) << summary << "\n";
    summary_file.close();
    
    closeFiles();
    
    std::cout << "\n=== Session Summary ===\n";
    std::cout << "Total Trades: " << metrics.total_trades << "\n";
    std::cout << "Total Volume: $" << metrics.total_volume << "\n";
    std::cout << "Realized P&L: $" << metrics.realized_pnl << "\n";
    std::cout << "Win Rate: " << (metrics.total_trades > 0 ? 
                                   (double)metrics.winning_trades / metrics.total_trades * 100 : 0) 
              << "%\n";
    std::cout << "Uptime: " << metrics.uptime.count() << "s\n";
    std::cout << "Session logs saved to: " << session_dir_ << "\n";
}

void TradeLogger::logOrderPlaced(const Order& order, const std::string& market_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!orders_file_.is_open()) return;
    
    orders_file_ << getCurrentTimestamp() << ","
                 << market_id << ","
                 << order.order_id << ","
                 << order.token_id << ","
                 << (order.side == Side::BUY ? "BUY" : "SELL") << ","
                 << order.price << ","
                 << order.size << ","
                 << "OPEN\n";
    orders_file_.flush();
}

void TradeLogger::logOrderCancelled(const OrderId& order_id, const Order& order, const std::string& market_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!orders_file_.is_open()) return;
    
    orders_file_ << getCurrentTimestamp() << ","
                 << market_id << ","
                 << order_id << ","
                 << order.token_id << ","
                 << (order.side == Side::BUY ? "BUY" : "SELL") << ","
                 << order.price << ","
                 << order.size << ","
                 << "CANCELLED\n";
    orders_file_.flush();
}

void TradeLogger::logOrderFilled(const std::string& market_id, const OrderId& order_id, const TokenId& token_id,
                                 Price fill_price, Size fill_size, Side side, double pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!fills_file_.is_open()) return;
    
    fills_file_ << getCurrentTimestamp() << ","
                << market_id << ","
                << order_id << ","
                << token_id << ","
                << (side == Side::BUY ? "BUY" : "SELL") << ","
                << fill_price << ","
                << fill_size << ","
                << pnl << "\n";
    fills_file_.flush();
    
    // Update statistics
    total_trades_++;
    total_volume_ += fill_price * fill_size;
    
    if (pnl > 0) {
        winning_trades_++;
        sum_wins_ += pnl;
        largest_win_ = std::max(largest_win_, pnl);
    } else if (pnl < 0) {
        losing_trades_++;
        sum_losses_ += std::abs(pnl);
        largest_loss_ = std::max(largest_loss_, std::abs(pnl));
    }
    
    total_realized_pnl_ += pnl;
}

void TradeLogger::logPosition(const std::string& market_id, const TokenId& token_id, double position, double avg_cost,
                              double market_value, double unrealized_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!positions_file_.is_open()) return;
    
    positions_file_ << getCurrentTimestamp() << ","
                    << market_id << ","
                    << token_id << ","
                    << position << ","
                    << avg_cost << ","
                    << market_value << ","
                    << unrealized_pnl << "\n";
    positions_file_.flush();
}

void TradeLogger::logPnL(const std::string& market_id, const TokenId& token_id, double realized_pnl, double unrealized_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!pnl_file_.is_open()) return;
    
    pnl_file_ << getCurrentTimestamp() << ","
              << market_id << ","
              << token_id << ","
              << realized_pnl << ","
              << unrealized_pnl << "\n";
    pnl_file_.flush();
}

void TradeLogger::snapshotPositions(
    const std::unordered_map<TokenId, double>& positions,
    const std::unordered_map<TokenId, double>& avg_costs,
    const std::unordered_map<TokenId, double>& market_values) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Log current metrics
    if (metrics_file_.is_open()) {
        metrics_file_ << getCurrentTimestamp() << ","
                     << total_trades_ << ","
                     << total_volume_ << ","
                     << total_realized_pnl_ << ","
                     << winning_trades_ << ","
                     << losing_trades_ << "\n";
        metrics_file_.flush();
    }
    
    // Save state snapshot as JSON for quick recovery
    saveStateSnapshot();
}

void TradeLogger::saveStateSnapshot() {
    // Save to latest_state.json for quick recovery
    nlohmann::json state;
    state["session_id"] = session_id_;
    state["timestamp"] = getCurrentTimestamp();
    state["total_trades"] = total_trades_;
    state["total_volume"] = total_volume_;
    state["realized_pnl"] = total_realized_pnl_;
    
    std::ofstream state_file(log_dir_ / "latest_state.json");
    state_file << std::setw(2) << state << "\n";
    state_file.close();
}

TradeState TradeLogger::loadState() const {
    TradeState state;
    
    // Try to load from latest_state.json
    std::filesystem::path state_path = log_dir_ / "latest_state.json";
    
    if (std::filesystem::exists(state_path)) {
        try {
            std::ifstream state_file(state_path);
            nlohmann::json j;
            state_file >> j;
            
            state.total_realized_pnl = j.value("realized_pnl", 0.0);
            state.total_trades = j.value("total_trades", 0);
            state.total_volume = j.value("total_volume", 0.0);
            
            std::cout << "Loaded state from previous session:\n";
            std::cout << "  Total trades: " << state.total_trades << "\n";
            std::cout << "  Total volume: $" << state.total_volume << "\n";
            std::cout << "  Realized P&L: $" << state.total_realized_pnl << "\n";
            
        } catch (const std::exception& e) {
            std::cerr << "Error loading state: " << e.what() << "\n";
        }
    } else {
        std::cout << "No previous state found, starting fresh\n";
    }
    
    return state;
}

SessionMetrics TradeLogger::getSessionMetrics() const {
    SessionMetrics metrics;
    
    metrics.session_id = session_id_;
    metrics.start_time = session_start_;
    metrics.end_time = std::chrono::system_clock::now();
    metrics.uptime = std::chrono::duration_cast<std::chrono::seconds>(
        metrics.end_time - metrics.start_time
    );
    
    metrics.total_trades = total_trades_;
    metrics.total_volume = total_volume_;
    metrics.realized_pnl = total_realized_pnl_;
    metrics.unrealized_pnl = 0.0;  // Would need to be passed in
    metrics.total_pnl = metrics.realized_pnl + metrics.unrealized_pnl;
    
    metrics.winning_trades = winning_trades_;
    metrics.losing_trades = losing_trades_;
    
    metrics.avg_win = winning_trades_ > 0 ? sum_wins_ / winning_trades_ : 0.0;
    metrics.avg_loss = losing_trades_ > 0 ? sum_losses_ / losing_trades_ : 0.0;
    
    metrics.largest_win = largest_win_;
    metrics.largest_loss = largest_loss_;
    
    return metrics;
}

} // namespace pmm