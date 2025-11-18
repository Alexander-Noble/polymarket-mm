#pragma once

#include "core/types.hpp"
#include "core/event_queue.hpp"
#include "data/order_book.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/order_manager.hpp"
#include "strategy/adverse_selection.hpp"
#include "utils/state_persistence.hpp"
#include "utils/trading_logger.hpp"
#include "utils/market_summary_logger.hpp"

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
                    const std::string& market_id,
                    const std::string& condition_id);
    
    void registerMarketMetadata(const TokenId& token_id,
                                const std::string& title,
                                const std::string& outcome,
                                const std::string& market_id,
                                const std::string& condition_id);
    
    void setEventEndTime(const std::string& condition_id, 
                        const std::chrono::system_clock::time_point& end_time);

    size_t getPositionCount() const;
    size_t getActiveOrderCount() const;
    size_t getBidCount() const;
    size_t getAskCount() const;
    size_t getActiveMarketCount() const;
    double getTotalPnL() const;
    double getUnrealizedPnL() const;
    double getTotalInventory() const;
    double getAverageSpread() const;
    size_t getFillCount() const;

    void startLogging(const std::string& event_name);
    void logInitialPositions();
    void snapshotPositions();
    
private:
    struct Position {
        double quantity = 0.0;
        double avg_entry_price = 0.0;
        double realized_pnl = 0.0;
        std::chrono::system_clock::time_point opened_at;
        std::chrono::system_clock::time_point last_updated;
        Side entry_side = Side::BUY;
        int num_fills = 0;
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
        std::chrono::steady_clock::time_point quote_created_at;
        int ttl_seconds;
        
        bool isExpired() const {
            auto now = std::chrono::steady_clock::now();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - quote_created_at);
            return age.count() >= ttl_seconds;
        }
        
        int getSecondsUntilExpiry() const {
            auto now = std::chrono::steady_clock::now();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - quote_created_at);
            return std::max(0, ttl_seconds - static_cast<int>(age.count()));
        }
    };

    struct PriceUpdateHistory {
        Price last_mid = 0.0;
        double last_bid_volume = 0.0;
        double last_ask_volume = 0.0;
        std::chrono::steady_clock::time_point last_update_time;
    };

    EventQueue& event_queue_;
    std::unique_ptr<StatePersistence> state_persistence_;
    std::unique_ptr<TradingLogger> trading_logger_;
    std::unique_ptr<MarketSummaryLogger> market_summary_logger_;
    std::unique_ptr<AdverseSelectionManager> as_manager_;
    OrderManager order_manager_;
    std::atomic<bool> running_;
    std::thread strategy_thread_;
    
    std::map<TokenId, OrderBook> order_books_;
    std::unordered_map<TokenId, MarketMaker> market_makers_;
    std::unordered_map<TokenId, MarketMetadata> market_metadata_;
    
    std::unordered_map<TokenId, Position> positions_;
    mutable std::mutex positions_mutex_;

    std::vector<FillMetrics> fill_history_;
    std::mutex fill_metrics_mutex_;
    std::atomic<size_t> total_fills_{0};
    std::atomic<bool> initial_positions_logged_{false};

    std::unordered_map<TokenId, QuoteSummary> active_quotes_;
    std::mutex quotes_mutex_;

    std::unordered_map<TokenId, PriceUpdateHistory> price_history_;
    std::mutex price_history_mutex_;

    void run();
    void checkPendingFillMetrics();
    void logQuoteSummary();
    void checkExpiredQuotes();
    
    void handleBookSnapshot(const Event& event);
    void handlePriceUpdate(const Event& event);
    void handleOrderFill(const Event& event);
    void handleOrderRejected(const Event& event);
    
    void calculateQuotes(const TokenId& token_id, 
                         const std::string& market_name,
                         CancelReason cancel_reason = CancelReason::QUOTE_UPDATE);
    
    OrderBook& getOrCreateOrderBook(const TokenId& token_id, 
                                   const std::string& market_name);

    void updatePosition(const TokenId& token_id, double qty, double price, Side side);
};

} // namespace pmm
