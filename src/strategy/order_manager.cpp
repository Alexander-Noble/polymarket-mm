#include "strategy/order_manager.hpp"
#include "utils/trading_logger.hpp"
#include "utils/logger.hpp"
#include <iostream>

namespace pmm {

OrderManager::OrderManager(EventQueue& event_queue, TradingMode mode, TradingLogger* trading_logger)
    : event_queue_(event_queue),
      trading_mode_(mode),
      next_order_id_(1),
      trading_logger_(trading_logger) {
    
    std::string mode_str = (mode == TradingMode::PAPER) ? "PAPER TRADING" : "LIVE";
    LOG_INFO("OrderManager initialized ({})", mode_str);
}

void OrderManager::setTradingMode(TradingMode mode) {
    if (mode != trading_mode_) {
        LOG_INFO("Switching trading mode from {} to {}", 
                 (trading_mode_ == TradingMode::PAPER ? "PAPER" : "LIVE"),
                 (mode == TradingMode::PAPER ? "PAPER" : "LIVE"));
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

    if (trading_logger_) {
        // Get market context if available
        Price market_mid = 0.0;
        Price market_spread = 0.0;
        Price best_bid = 0.0;
        Price best_ask = 0.0;
        Price our_bid = 0.0;
        Price our_ask = 0.0;
        
        auto book_it = market_books_.find(token_id);
        if (book_it != market_books_.end()) {
            const auto& book = book_it->second;
            market_mid = book.getMid();
            market_spread = book.getSpread();
            best_bid = book.getBestBid();
            best_ask = book.getBestAsk();
        }
        
        // Find our paired orders for this token to calculate our spread
        for (const auto& [oid, o] : orders_) {
            if (o.token_id == token_id && o.status == OrderStatus::OPEN) {
                if (o.side == Side::BUY) {
                    our_bid = std::max(our_bid, o.price);
                } else {
                    our_ask = (our_ask == 0.0) ? o.price : std::min(our_ask, o.price);
                }
            }
        }
        // Include the current order being placed
        if (order.side == Side::BUY) {
            our_bid = std::max(our_bid, order.price);
        } else {
            our_ask = (our_ask == 0.0) ? order.price : std::min(our_ask, order.price);
        }
        
        trading_logger_->logOrderPlaced(order, market_id, market_mid, market_spread, best_bid, best_ask, our_bid, our_ask);
    }

    if (trading_mode_ == TradingMode::PAPER) {
        LOG_DEBUG("[PAPER] Order placed: {} - {} {} @ {}", order_id, (side == Side::BUY ? "BUY" : "SELL"), size, price);
    } else {
        LOG_INFO("[LIVE] Placing order: {} - {} {} @ {}", order_id, (side == Side::BUY ? "BUY" : "SELL"), size, price);
        placeOrderLive(order);
    }
    
    return order_id;
}

bool OrderManager::cancelOrder(const OrderId& order_id, const std::string& market_id, CancelReason reason) {
    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        LOG_ERROR("Order not found: {}", order_id);
        return false;
    }

    Order& order = it->second;
    order.status = OrderStatus::CANCELLED;
    
    if (trading_logger_) {
        trading_logger_->logOrderCancelled(order_id, order, market_id, reason);
    }

    if (trading_mode_ == TradingMode::PAPER) {
        LOG_DEBUG("[PAPER] Order cancelled: {}", order_id);
        orders_.erase(it);
    } else {
        LOG_INFO("[LIVE] Cancelling order: {}", order_id);
        cancelOrderLive(order_id);
    }
    return true;
}

bool OrderManager::cancelAllOrders(const TokenId& token_id, const std::string& market_id, CancelReason reason) {
    std::vector<OrderId> to_cancel;
    
    for (const auto& [order_id, order] : orders_) {
        if (order.token_id == token_id) {
            to_cancel.push_back(order_id);
        }
    }
    
    for (const auto& order_id : to_cancel) {
        if (!cancelOrder(order_id, market_id, reason)) {
            LOG_ERROR("Failed to cancel order: {}", order_id);
            return false;
        }
    }
    return true;
}

bool OrderManager::cancelAllOrders(CancelReason reason) {
    std::vector<OrderId> to_cancel;
    
    for (const auto& [order_id, _] : orders_) {
        to_cancel.push_back(order_id);
    }
    
    for (const auto& order_id : to_cancel) {
        if (!cancelOrder(order_id, "cancel_all", reason)) {
            LOG_ERROR("Failed to cancel order: {}", order_id);
            return false;
        }
    }
    return true;
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
                LOG_INFO("[PAPER] BUY order {} crossed! Market ask {} <= our bid {}", order_id, book.getBestAsk(), order.price);
            }
        } else { // SELL
            // Sell order fills if best bid >= our ask price
            if (book.getBestBid() > 0 && book.getBestBid() >= order.price) {
                should_fill = true;
                fill_price = order.price; // We receive our ask price
                LOG_INFO("[PAPER] SELL order {} crossed! Market bid {} >= our ask {}", order_id, book.getBestBid(), order.price);
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
    LOG_INFO("[PAPER FILL] {} {} @ {} (order: {})", side_str, fill_size, fill_price, order_id);
    
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

size_t OrderManager::getBidCount() const {
    size_t count = 0;
    for (const auto& [_, order] : orders_) {
        if (order.side == Side::BUY) {
            count++;
        }
    }
    return count;
}

size_t OrderManager::getAskCount() const {
    size_t count = 0;
    for (const auto& [_, order] : orders_) {
        if (order.side == Side::SELL) {
            count++;
        }
    }
    return count;
}

// Stubs for live trading (to be implemented)
void OrderManager::placeOrderLive(const Order& order) {
    // TODO: Implement Polymarket API order placement
    LOG_ERROR("Live order placement not yet implemented");
}

void OrderManager::cancelOrderLive(const OrderId& order_id) {
    // TODO: Implement Polymarket API order cancellation
    LOG_ERROR("Live order cancellation not yet implemented");
}

} // namespace pmm