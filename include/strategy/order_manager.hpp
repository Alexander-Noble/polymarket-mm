#pragma once

#include "core/types.hpp"
#include "core/event_queue.hpp"
#include "data/order_book.hpp"
#include <unordered_map>
#include <vector>
#include <optional>
#include <chrono>

namespace pmm {


enum class TradingMode {
    PAPER,
    LIVE
};

class TradeLogger; 

class OrderManager {
public:
    explicit OrderManager(EventQueue& event_queue, TradingMode mode = TradingMode::PAPER, TradeLogger* logger = nullptr);
    
    OrderId placeOrder(const TokenId& token_id, Side side, Price price, Size size, const std::string& market_id);

    void cancelOrder(const OrderId& order_id, const std::string& market_id);
    void cancelAllOrders(const TokenId& token_id, const std::string& market_id);
    void cancelAllOrders();

    void updateOrderBook(const TokenId& token_id, const OrderBook& book);
    
    std::vector<Order> getOpenOrders(const TokenId& token_id) const;
    size_t getOpenOrderCount() const;
    size_t getActiveOrderCount() const { return getOpenOrderCount(); }

    void setTradingMode(TradingMode mode);
    TradingMode getTradingMode() const { return trading_mode_; }
    bool isPaperTrading() const { return trading_mode_ == TradingMode::PAPER; }

private:
    EventQueue& event_queue_;
    TradingMode trading_mode_;
    TradeLogger* trade_logger_; 

    std::unordered_map<OrderId, Order> orders_;
    uint64_t next_order_id_;
    std::unordered_map<std::string, OrderBook> market_books_;

    void checkForFills(const TokenId& token_id, const OrderBook& book);
    void generateFill(const OrderId& order_id, Price fill_price, Size fill_size);

    void placeOrderLive(const Order& order);
    void cancelOrderLive(const OrderId& order_id);
};

} // namespace pmm