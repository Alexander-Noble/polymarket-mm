#pragma once

#include "core/types.hpp"
#include "core/event_queue.hpp"
#include "data/order_book.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/order_manager.hpp"
#include "strategy/adverse_selection.hpp"
#include "utils/state_persistence.hpp"
#include "utils/trading_logger.hpp"

#include <map>
#include <atomic>
#include <thread>
#include <unordered_map>

namespace pmm {

class StrategyEngine {
public:
    explicit StrategyEngine(EventQueue& queue, TradingMode mode);
    ~StrategyEngine();
    
    void start();
    void stop();
    
    bool isRunning() const {
        return running_.load();
    }

    void registerMarket(const TokenId& token_id, 
                    const std::string& title,
                    const std::string& outcome,
                    const std::string& market_id);

    size_t getPositionCount() const;
    size_t getActiveOrderCount() const;
    size_t getBidCount() const;
    size_t getAskCount() const;
    size_t getActiveMarketCount() const;
    double getTotalPnL() const;
    double getUnrealizedPnL() const;
    double getTotalInventory() const;  // Absolute sum of all positions
    double getAverageSpread() const;    // Average spread across all active markets
    size_t getFillCount() const;        // Total fills since start

    void startLogging(const std::string& event_name);
    void snapshotPositions();
    
private:
    struct Position {
        double quantity = 0.0;
        double avg_entry_price = 0.0;
        double realized_pnl = 0.0;
    };

    struct FillMetrics {
        std::chrono::system_clock::time_point fill_time;
        TokenId token_id;
        OrderId order_id;
        Side side;
        Price fill_price;
        Price mid_at_fill;
        Price best_bid_at_fill;
        Price best_ask_at_fill;
        double spread_at_fill;
        double imbalance_at_fill;
        double inventory_before;
        double inventory_after;
        
        // To be populated later
        Price mid_30s_after = 0.0;
        Price mid_60s_after = 0.0;
        bool metrics_complete = false;
    };

    struct QuoteSummary {
        std::string market_name;
        Price bid_price;
        Price ask_price;
        Price mid;
        double spread_bps;
        double inventory;
        std::chrono::steady_clock::time_point last_update;
    };

    EventQueue& event_queue_;
    std::unique_ptr<StatePersistence> state_persistence_;
    std::unique_ptr<TradingLogger> trading_logger_;
    std::unique_ptr<AdverseSelectionManager> as_manager_;
    OrderManager order_manager_;
    std::atomic<bool> running_;
    std::thread strategy_thread_;
    
    std::map<TokenId, OrderBook> order_books_;
    std::unordered_map<TokenId, MarketMaker> market_makers_;
    std::unordered_map<TokenId, MarketMetadata> market_metadata_;
    
    std::unordered_map<TokenId, Position> positions_;
    mutable std::mutex positions_mutex_;

    // Fill metrics tracking
    std::vector<FillMetrics> fill_history_;
    std::mutex fill_metrics_mutex_;
    std::atomic<size_t> total_fills_{0};

    // Quote tracking for summary
    std::unordered_map<TokenId, QuoteSummary> active_quotes_;
    std::mutex quotes_mutex_;

    void run();
    void checkPendingFillMetrics();
    void logQuoteSummary();
    
    void handleBookSnapshot(const Event& event);
    void handlePriceUpdate(const Event& event);
    void handleOrderFill(const Event& event);
    void handleOrderRejected(const Event& event);
    
    void calculateQuotes(const TokenId& token_id, 
                         const std::string& market_name);
    
    OrderBook& getOrCreateOrderBook(const TokenId& token_id, 
                                   const std::string& market_name);

    void updatePosition(const TokenId& token_id, double qty, double price, Side side);
};

} // namespace pmm
