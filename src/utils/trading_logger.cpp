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

    // Update the main logger to write to this session directory
    Logger::updateSessionDir(session_dir_.string(), "polymarket_mm");

    orders_file_.open(session_dir_ / "orders.csv");
    orders_file_ << "timestamp,market_id,order_id,token_id,side,price,size,status\n";
    
    fills_file_.open(session_dir_ / "fills.csv");
    fills_file_ << "timestamp,market_id,order_id,token_id,side,fill_price,fill_size,pnl\n";
    
    positions_file_.open(session_dir_ / "positions.csv");
    positions_file_ << "timestamp,market_id,token_id,position,avg_cost,opened_at,last_updated,entry_side,num_fills,total_cost\n";
    
    price_updates_file_.open(session_dir_ / "price_updates.csv");
    price_updates_file_ << "timestamp,market_id,token_id,mid_price,price_change_pct,price_change_abs,"
                        << "best_bid,best_ask,spread,spread_bps,bid_volume_5levels,ask_volume_5levels,"
                        << "total_volume,volume_imbalance,bid_levels_count,ask_levels_count,"
                        << "our_inventory,time_to_event_hours,seconds_since_last_update\n";
}

void TradingLogger::closeFiles() {
    if (orders_file_.is_open()) orders_file_.close();
    if (fills_file_.is_open()) fills_file_.close();
    if (positions_file_.is_open()) positions_file_.close();
    if (price_updates_file_.is_open()) price_updates_file_.close();
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
                                 const std::chrono::system_clock::time_point& opened_at,
                                 const std::chrono::system_clock::time_point& last_updated,
                                 Side entry_side, int num_fills, double total_cost) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!positions_file_.is_open()) return;
    
    // Format opened_at timestamp
    auto opened_time_t = std::chrono::system_clock::to_time_t(opened_at);
    std::stringstream opened_ss;
    opened_ss << std::put_time(std::gmtime(&opened_time_t), "%Y-%m-%dT%H:%M:%SZ");
    
    // Format last_updated timestamp
    auto updated_time_t = std::chrono::system_clock::to_time_t(last_updated);
    std::stringstream updated_ss;
    updated_ss << std::put_time(std::gmtime(&updated_time_t), "%Y-%m-%dT%H:%M:%SZ");
    
    positions_file_ << getCurrentTimestamp() << ","
                    << market_id << ","
                    << token_id << ","
                    << position << ","
                    << avg_cost << ","
                    << opened_ss.str() << ","
                    << updated_ss.str() << ","
                    << (entry_side == Side::BUY ? "BUY" : "SELL") << ","
                    << num_fills << ","
                    << total_cost << "\n";
    positions_file_.flush();
}

void TradingLogger::logPriceUpdate(const std::string& market_id, const TokenId& token_id,
                                   Price mid_price, double price_change_pct, double price_change_abs,
                                   Price best_bid, Price best_ask, Price spread, double spread_bps,
                                   double bid_volume, double ask_volume, double total_volume, double volume_imbalance,
                                   int bid_levels, int ask_levels,
                                   double our_inventory, double time_to_event_hours, double seconds_since_last_update) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!price_updates_file_.is_open()) return;
    
    price_updates_file_ << getCurrentTimestamp() << ","
                        << market_id << ","
                        << token_id << ","
                        << mid_price << ","
                        << price_change_pct << ","
                        << price_change_abs << ","
                        << best_bid << ","
                        << best_ask << ","
                        << spread << ","
                        << spread_bps << ","
                        << bid_volume << ","
                        << ask_volume << ","
                        << total_volume << ","
                        << volume_imbalance << ","
                        << bid_levels << ","
                        << ask_levels << ","
                        << our_inventory << ","
                        << time_to_event_hours << ","
                        << seconds_since_last_update << "\n";
    price_updates_file_.flush();
}

} // namespace pmm
