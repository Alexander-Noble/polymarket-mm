#pragma once

#include "core/types.hpp"
#include <fstream>
#include <filesystem>
#include <mutex>
#include <chrono>

namespace pmm {

class TradingLogger {
public:
    TradingLogger(const std::filesystem::path& log_dir);
    ~TradingLogger();
    
    void startSession(const std::string& event_name);
    void endSession();
    std::string getSessionId() const { return session_id_; }

    void logOrderPlaced(const Order& order, const std::string& market_id);
    void logOrderCancelled(const OrderId& order_id, const Order& order, const std::string& market_id);
    void logOrderFilled(const std::string& market_id, const OrderId& order_id, const TokenId& token_id, 
                       Price fill_price, Size fill_size, Side side, double pnl = 0.0);

    void logPosition(const std::string& market_id, const TokenId& token_id, Size position, Price avg_cost, 
                    double market_value, double unrealized_pnl);

private:
    std::filesystem::path log_dir_;
    std::filesystem::path session_dir_;
    std::string session_id_;
    std::string event_name_;
    std::chrono::system_clock::time_point session_start_;
    
    std::ofstream orders_file_;
    std::ofstream fills_file_;
    std::ofstream positions_file_;
    
    std::mutex mutex_;
    
    void ensureLogDir();
    void initializeFiles();
    void closeFiles();
    std::string getCurrentTimestamp() const;
};

} // namespace pmm
