#include "utils/trading_logger.hpp"
#include "utils/logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace pmm {

TradingLogger::TradingLogger(const std::filesystem::path& log_dir)
    : log_dir_(log_dir) {
    ensureLogDir();
}

TradingLogger::~TradingLogger() {
    endSession();
}

void TradingLogger::ensureLogDir() {
    if (!std::filesystem::exists(log_dir_)) {
        std::filesystem::create_directories(log_dir_);
        LOG_DEBUG("Created log directory: {}", log_dir_.string());
    }
}

void TradingLogger::initializeFiles() {
    session_dir_ = log_dir_ / session_id_;
    std::filesystem::create_directories(session_dir_);
    LOG_DEBUG("Created session directory: {}", session_dir_.string());

    orders_file_.open(session_dir_ / "orders.csv");
    orders_file_ << "timestamp,market_id,order_id,token_id,side,price,size,status\n";
    
    fills_file_.open(session_dir_ / "fills.csv");
    fills_file_ << "timestamp,market_id,order_id,token_id,side,fill_price,fill_size,pnl\n";
    
    positions_file_.open(session_dir_ / "positions.csv");
    positions_file_ << "timestamp,market_id,token_id,position,avg_cost,market_value,unrealized_pnl\n";
}

void TradingLogger::closeFiles() {
    if (orders_file_.is_open()) orders_file_.close();
    if (fills_file_.is_open()) fills_file_.close();
    if (positions_file_.is_open()) positions_file_.close();
}

std::string TradingLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

void TradingLogger::startSession(const std::string& event_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    event_name_ = event_name;
    session_start_ = std::chrono::system_clock::now();
    
    auto time_t = std::chrono::system_clock::to_time_t(session_start_);
    std::stringstream ss;
    ss << "session_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    session_id_ = ss.str();
    
    initializeFiles();
    
    LOG_INFO("Trading session started: {} for event: {}", session_id_, event_name);
}

void TradingLogger::endSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!orders_file_.is_open()) {
        return;
    }
    
    closeFiles();
    
    auto session_end = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(session_end - session_start_);
    
    LOG_INFO("Trading session ended: {} (duration: {}s)", session_id_, duration.count());
    LOG_INFO("Session logs saved to: {}", session_dir_.string());
}

void TradingLogger::logOrderPlaced(const Order& order, const std::string& market_id) {
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

void TradingLogger::logOrderCancelled(const OrderId& order_id, const Order& order, const std::string& market_id) {
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

void TradingLogger::logOrderFilled(const std::string& market_id, const OrderId& order_id, const TokenId& token_id,
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
}

void TradingLogger::logPosition(const std::string& market_id, const TokenId& token_id, double position, double avg_cost,
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

} // namespace pmm
