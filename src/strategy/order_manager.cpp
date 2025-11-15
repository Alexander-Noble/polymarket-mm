#include "strategy/order_manager.hpp"
#include "utils/trade_logger.hpp"
#include <iostream>

namespace pmm {

OrderManager::OrderManager(EventQueue& event_queue, TradingMode mode, TradeLogger* logger)
    : event_queue_(event_queue),
      trading_mode_(mode),
      next_order_id_(1),
      trade_logger_(logger) {
    
    std::string mode_str = (mode == TradingMode::PAPER) ? "PAPER TRADING" : "LIVE";
    std::cout << "OrderManager initialized (" << mode_str << ")\n";
}

void OrderManager::setTradingMode(TradingMode mode) {
    if (mode != trading_mode_) {
        std::cout << "Switching trading mode from " 
                  << (trading_mode_ == TradingMode::PAPER ? "PAPER" : "LIVE")
                  << " to "
                  << (mode == TradingMode::PAPER ? "PAPER" : "LIVE") << "\n";
        trading_mode_ = mode;
    }
}

OrderId OrderManager::placeOrder(const TokenId& token_id, Side side, Price price, Size size, const std::string& market_id) {
    OrderId order_id = "ORD_" + std::to_string(next_order_id_++);
    
    Order order{
        order_id,
        token_id,
        side,
        price,
        size,
        0.0,  // filled_size
        OrderStatus::OPEN,
        std::chrono::steady_clock::now()
    };
    
    orders_[order_id] = order;

    if (trade_logger_) {
        trade_logger_->logOrderPlaced(order, market_id);
    }

    if (trading_mode_ == TradingMode::PAPER) {
        std::cout << "[PAPER] Order placed: " << order_id 
                  << " - " << (side == Side::BUY ? "BUY" : "SELL")
                  << " " << size << " @ " << price << "\n";
    } else {
        std::cout << "[LIVE] Placing order: " << order_id 
                  << " - " << (side == Side::BUY ? "BUY" : "SELL")
                  << " " << size << " @ " << price << "\n";
        placeOrderLive(order);
    }
    
    return order_id;
}

void OrderManager::cancelOrder(const OrderId& order_id, const std::string& market_id) {
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        std::cerr << "Order not found: " << order_id << "\n";
        return;
    }

    Order& order = it->second;
    order.status = OrderStatus::CANCELLED;
    
    if (trade_logger_) {
        trade_logger_->logOrderCancelled(order_id, order, market_id);
    }

    if (trading_mode_ == TradingMode::PAPER) {
        std::cout << "[PAPER] Order cancelled: " << order_id << "\n";
        orders_.erase(it);
    } else {
        std::cout << "[LIVE] Cancelling order: " << order_id << "\n";
        cancelOrderLive(order_id);
    }
}

void OrderManager::cancelAllOrders(const TokenId& token_id, const std::string& market_id) {
    std::vector<OrderId> to_cancel;
    
    for (const auto& [order_id, order] : orders_) {
        if (order.token_id == token_id) {
            to_cancel.push_back(order_id);
        }
    }
    
    for (const auto& order_id : to_cancel) {
        cancelOrder(order_id, market_id);
    }
}

void OrderManager::cancelAllOrders() {
    std::vector<OrderId> to_cancel;
    for (const auto& [order_id, _] : orders_) {
        to_cancel.push_back(order_id);
    }
    
    for (const auto& order_id : to_cancel) {
        cancelOrder(order_id, "cancel_all"); //TODO: maybe we add market ID to order
    }
}

void OrderManager::updateOrderBook(const TokenId& token_id, const OrderBook& book) {
    market_books_.insert_or_assign(token_id, book);
    
    // Only check for fills in paper trading mode
    if (trading_mode_ == TradingMode::PAPER) {
        checkForFills(token_id, book);
    }
}

void OrderManager::checkForFills(const TokenId& token_id, const OrderBook& book) {
    if (!isPaperTrading()) return;
    
    std::vector<OrderId> fills_to_process;
    
    for (auto& [order_id, order] : orders_) {
        if (order.token_id != token_id || order.status != OrderStatus::OPEN) {
            continue;
        }
        
        bool should_fill = false;
        Price fill_price = order.price;
        
        if (order.side == Side::BUY) {
            // Buy order fills if best ask <= our bid price
            if (book.getBestAsk() > 0 && book.getBestAsk() <= order.price) {
                should_fill = true;
                fill_price = order.price; // We pay our bid price
                std::cout << "[PAPER] BUY order " << order_id 
                         << " crossed! Market ask " << book.getBestAsk() 
                         << " <= our bid " << order.price << "\n";
            }
        } else { // SELL
            // Sell order fills if best bid >= our ask price
            if (book.getBestBid() > 0 && book.getBestBid() >= order.price) {
                should_fill = true;
                fill_price = order.price; // We receive our ask price
                std::cout << "[PAPER] SELL order " << order_id 
                         << " crossed! Market bid " << book.getBestBid() 
                         << " >= our ask " << order.price << "\n";
            }
        }
        
        if (should_fill) {
            fills_to_process.push_back(order_id);
        }
    }
    
    for (const auto& order_id : fills_to_process) {
        auto it = orders_.find(order_id);
        if (it != orders_.end()) {
            generateFill(order_id, it->second.price, it->second.size);
        }
    }
}

void OrderManager::generateFill(const OrderId& order_id, Price fill_price, Size fill_size) {
    auto it = orders_.find(order_id);
    if (it == orders_.end()) return;
    
    Order& order = it->second;
    
    order.filled_size += fill_size;
    if (order.filled_size >= order.size) {
        order.status = OrderStatus::FILLED;
    }
    
    std::string side_str = (order.side == Side::BUY) ? "BOUGHT" : "SOLD";
    std::cout << "[PAPER FILL] " << side_str << " " << fill_size 
              << " @ " << fill_price << " (order: " << order_id << ")\n";
    
    // Generate fill event
    auto fill_event = Event::orderFill(
        order_id,
        order.token_id,
        fill_price,
        fill_size,
        order.side
    );
    
    event_queue_.push(std::move(fill_event));
}

std::vector<Order> OrderManager::getOpenOrders(const TokenId& token_id) const {
    std::vector<Order> open_orders;
    for (const auto& [_, order] : orders_) {
        if (order.token_id == token_id) {
            open_orders.push_back(order);
        }
    }
    return open_orders;
}

size_t OrderManager::getOpenOrderCount() const {
    return orders_.size();
}

// Stubs for live trading (to be implemented)
void OrderManager::placeOrderLive(const Order& order) {
    // TODO: Implement Polymarket API order placement
    std::cerr << "Live order placement not yet implemented\n";
}

void OrderManager::cancelOrderLive(const OrderId& order_id) {
    // TODO: Implement Polymarket API order cancellation
    std::cerr << "Live order cancellation not yet implemented\n";
}

} // namespace pmm