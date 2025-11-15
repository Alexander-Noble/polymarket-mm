#pragma once

#include "core/types.hpp"
#include <fstream>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace pmm {

struct TradeState {
    std::unordered_map<TokenId, Size> positions;          // token -> position size
    std::unordered_map<TokenId, Price> avg_costs;          // token -> avg cost
    std::unordered_map<TokenId, double> realized_pnl;       // token -> realized P&L
    double total_realized_pnl = 0.0;
    int total_trades = 0;
    Volume total_volume = 0.0;
};

struct SessionMetrics {
    std::string session_id;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    int total_trades;
    Volume total_volume;
    double realized_pnl;
    double unrealized_pnl;
    double total_pnl;
    int winning_trades;
    int losing_trades;
    double avg_win;
    double avg_loss;
    double largest_win;
    double largest_loss;
    std::chrono::seconds uptime;
};

class TradeLogger {
public:
    TradeLogger(const std::filesystem::path& log_dir, const std::string& session_name = "");
    ~TradeLogger();
    
    void startSession(const std::string& event_name);
    void endSession();
    std::string getSessionId() const { return session_id_; }

    void logOrderPlaced(const Order& order, const std::string& market_id);
    void logOrderCancelled(const OrderId& order_id, const Order& order, const std::string& market_id);
    void logOrderFilled(const std::string& market_id, const OrderId& order_id, const TokenId& token_id, 
                       Price fill_price, Size fill_size, Side side, double pnl = 0.0);

    void logPosition(const std::string& market_id, const TokenId& token_id, Size position, Price avg_cost, 
                    double market_value, double unrealized_pnl);

    void logPnL(const std::string& market_id, const TokenId& token_id, double realized_pnl, double unrealized_pnl);
        
    void snapshotPositions(const std::unordered_map<TokenId, Size>& positions,
                          const std::unordered_map<TokenId, Price>& avg_costs,
                          const std::unordered_map<TokenId, double>& market_values);
    
    // State recovery
    TradeState loadState() const;
    
    // Metrics
    SessionMetrics getSessionMetrics() const;
    
private:
    std::filesystem::path log_dir_;
    std::filesystem::path session_dir_;
    std::string session_id_;
    std::string event_name_;
    std::chrono::system_clock::time_point session_start_;
    
    std::ofstream orders_file_;
    std::ofstream fills_file_;
    std::ofstream positions_file_;
    std::ofstream pnl_file_;
    std::ofstream metrics_file_;
    
    std::mutex mutex_;
    
    // Running statistics
    int total_trades_ = 0;
    double total_volume_ = 0.0;
    double total_realized_pnl_ = 0.0;
    int winning_trades_ = 0;
    int losing_trades_ = 0;
    double largest_win_ = 0.0;
    double largest_loss_ = 0.0;
    double sum_wins_ = 0.0;
    double sum_losses_ = 0.0;
    
    void ensureLogDir();
    void initializeFiles();
    void closeFiles();
    std::string getCurrentTimestamp() const;
    void saveStateSnapshot();
};

} // namespace pmm