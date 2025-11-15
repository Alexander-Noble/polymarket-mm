#include "strategy/strategy_engine.hpp"
#include <iostream>

namespace pmm {

StrategyEngine::StrategyEngine(EventQueue& queue, TradingMode mode)
    : event_queue_(queue),
    trade_logger_(std::make_unique<TradeLogger>("./logs")),
    order_manager_(queue, mode, trade_logger_.get()),
    running_(false) {
    std::cout << "StrategyEngine initialized\n";
}

StrategyEngine::~StrategyEngine() {
    stop();
}

void StrategyEngine::start() {
    if (running_.load()) {
        std::cout << "StrategyEngine already running\n";
        return;
    }
    
    running_ = true;
    strategy_thread_ = std::thread(&StrategyEngine::run, this);
    std::cout << "StrategyEngine started\n";
}

void StrategyEngine::stop() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "Stopping StrategyEngine...\n";
    running_.store(false);
    
    // Push a shutdown event to unblock the queue
    event_queue_.push(Event::shutdown("Strategy shutdown"));
    
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    
    std::cout << "StrategyEngine stopped\n";
}

void StrategyEngine::run() {
    std::cout << "StrategyEngine event loop started\n";
    
    auto last_snapshot = std::chrono::steady_clock::now();

    while (running_.load()) {
        Event event = event_queue_.pop();
        
        // Route to appropriate handler
        switch (event.type) {
            case EventType::BOOK_SNAPSHOT:
                handleBookSnapshot(event);
                break;
                
            case EventType::PRICE_LEVEL_UPDATE:
                handlePriceUpdate(event);
                break;
                
            case EventType::ORDER_FILL:
                handleOrderFill(event);
                break;
                
            case EventType::ORDER_REJECTED:
                handleOrderRejected(event);
                break;
                
            case EventType::SHUTDOWN:
                std::cout << "Received shutdown event\n";
                running_.store(false);
                break;
                
            default:
                std::cout << "Unknown event type\n";
                break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_snapshot > std::chrono::seconds(60)) {
            snapshotPositions();
            last_snapshot = now;
        }
    }
    
    std::cout << "StrategyEngine event loop exited\n";
}

void StrategyEngine::handleBookSnapshot(const Event& event) {
    auto& payload = std::get<BookSnapshotPayload>(event.payload);

    std::string market_name = payload.token_id;
    auto it = market_metadata_.find(payload.token_id);
    if (it != market_metadata_.end()) {
        market_name = it->second.title + " - " + it->second.outcome;
    }
    std::cout << "Processing book snapshot for " << market_name << "\n";
    std::cout << "       Bids: " << payload.bids.size() 
              << " levels | Asks: " << payload.asks.size() << " levels\n";
        
    OrderBook& book = getOrCreateOrderBook(payload.token_id, market_name);
    book.clear();
    
    for (const auto& [price, size] : payload.bids) {
        book.updateBid(price, size);
    }
    for (const auto& [price, size] : payload.asks) {
        book.updateAsk(price, size);
    }
    
    std::cout << "Order book updated: " << market_name
              << " - Best bid: " << book.getBestBid()
              << ", Best ask: " << book.getBestAsk()
              << ", Spread: " << book.getSpread() << "\n";
    
    
    order_manager_.updateOrderBook(payload.token_id, book);
    calculateQuotes(payload.token_id, market_name);
}

void StrategyEngine::handlePriceUpdate(const Event& event) {
    auto& payload = std::get<PriceLevelUpdatePayload>(event.payload);
    auto token_id = payload.token_id;
    auto market_name = market_metadata_.find(token_id) != market_metadata_.end() ?
                       market_metadata_[token_id].title + " - " + market_metadata_[token_id].outcome :
                       token_id;
    std::cout << "Processing price update for " << market_name << "\n";
    std::cout << "       Bids: " << payload.bids.size() 
              << " levels | Asks: " << payload.asks.size() << " levels\n";

    OrderBook& book = getOrCreateOrderBook(token_id, market_name);
    
    for (const auto& [price, size] : payload.bids) {
        book.updateBid(price, size);
    }
    for (const auto& [price, size] : payload.asks) {
        book.updateAsk(price, size);
    }
    
    std::cout << "Price levels updated: " << market_name
              << " - Best bid: " << book.getBestBid()
              << ", Best ask: " << book.getBestAsk() << "\n";

    calculateQuotes(token_id, market_name);
}

void StrategyEngine::handleOrderFill(const Event& event) {
    auto& payload = std::get<OrderFillPayload>(event.payload);
    auto market_name = market_metadata_.find(payload.token_id) != market_metadata_.end() ?
                    market_metadata_[payload.token_id].title + " - " + market_metadata_[payload.token_id].outcome :
                    payload.token_id;
    
    std::cout << "\n>>> FILL EVENT: " << payload.order_id << "\n";
    std::cout << "    Market: " << market_name << "\n";
    std::cout << "    Side: " << (payload.side == Side::BUY ? "BUY" : "SELL") << "\n";
    std::cout << "    Size: " << payload.filled_size << " @ " << payload.fill_price << "\n";
    
    updatePosition(payload.token_id, payload.filled_size, payload.fill_price, payload.side);
    
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto& pos = positions_[payload.token_id];
        std::cout << "    New position: " << pos.quantity 
                  << " @ avg " << pos.avg_entry_price 
                  << " | Realized PnL: $" << pos.realized_pnl << "\n";
    }
    
    auto mm_it = market_makers_.find(payload.token_id);
    if (mm_it != market_makers_.end()) {
        mm_it->second.updateInventory(
            payload.side,
            payload.filled_size,
            payload.fill_price
        );
    }
    
    if (trade_logger_) {
        trade_logger_->logOrderFilled(
            market_name,
            payload.order_id,
            payload.token_id,
            payload.fill_price,
            payload.filled_size,
            payload.side,
            mm_it->second.getRealizedPnL()
        );
    }

    calculateQuotes(payload.token_id, market_name);
}

void StrategyEngine::handleOrderRejected(const Event& event) {
    auto& payload = std::get<OrderRejectedPayload>(event.payload);
    std::cerr << "Order rejected: " << payload.order_id
              << " - Reason: " << payload.reason << "\n";
    
    // TODO: Handle rejection logic
}

void StrategyEngine::calculateQuotes(const TokenId& token_id, 
                                   const std::string& market_name) {
    auto it = order_books_.find(token_id);
    if (it == order_books_.end()) {
        std::cerr << "No order book found for token: " << token_id << ", market: " << market_name << "\n";
        return;
    }
    
    const OrderBook& book = it->second;
    
    if (!book.hasValidBBO()) {
        std::cout << "No valid BBO for " << token_id << ", skipping quote calculation\n";
        return;
    }
    
    auto mm_it = market_makers_.try_emplace(
        token_id, 
        0.02,      // spread
        1000.0     // risk_aversion
    ).first;
    
    auto quote_opt = mm_it->second.generateQuote(book);
    
    if (quote_opt.has_value()) {
        const Quote& quote = quote_opt.value();
        auto existing_orders = order_manager_.getOpenOrders(token_id);
        
        bool has_matching_bid = false;
        bool has_matching_ask = false;
        
        for (const auto& order : existing_orders) {
            if (order.side == Side::BUY && std::abs(order.price - quote.bid_price) < 0.001) {
                has_matching_bid = true;
            }
            if (order.side == Side::SELL && std::abs(order.price - quote.ask_price) < 0.001) {
                has_matching_ask = true;
            }
        }
        
        if (has_matching_bid && has_matching_ask) {
            std::cout << "  Orders already at target prices, no update needed\n";
            return;
        }
        std::cout << "\n  PLACING ORDERS:\n";
        std::cout << "  BID: " << quote.bid_price << " x " << quote.bid_size << "\n";
        std::cout << "  ASK: " << quote.ask_price << " x " << quote.ask_size << "\n";
        
        order_manager_.cancelAllOrders(token_id, market_name);
        
        order_manager_.placeOrder(token_id, Side::BUY, quote.bid_price, quote.bid_size, market_name);
        order_manager_.placeOrder(token_id, Side::SELL, quote.ask_price, quote.ask_size, market_name);
    }
}

OrderBook& StrategyEngine::getOrCreateOrderBook(const TokenId& token_id, const std::string& market_name) {
    auto it = order_books_.find(token_id);
    if (it == order_books_.end()) {
        std::cout << "Creating new order book for token: " << market_name << "\n";
        auto result = order_books_.emplace(token_id, OrderBook(token_id));
        return result.first->second;
    }
    return it->second;
}

void StrategyEngine::registerMarket(const TokenId& token_id, 
                    const std::string& title,
                    const std::string& outcome,
                    const std::string& market_id = "") {
    market_metadata_[token_id] = {title, outcome, market_id};
    std::cout << "Registered: " << title << " - " << outcome << "\n";
}

size_t StrategyEngine::getPositionCount() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    size_t count = 0;
    for (const auto& [token_id, position] : positions_) {
        if (std::abs(position.quantity) > 0.001) {
            count++;
        }
    }
    return count;
}

size_t StrategyEngine::getActiveOrderCount() const {
    return order_manager_.getActiveOrderCount();
}

double StrategyEngine::getTotalPnL() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    double total = 0.0;
    for (const auto& [token_id, position] : positions_) {
        total += position.realized_pnl;
    }
    return total;
}

double StrategyEngine::getUnrealizedPnL() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    double unrealized = 0.0;
    
    for (const auto& [token_id, position] : positions_) {
        if (std::abs(position.quantity) < 0.001) continue;
        
        auto ob_it = order_books_.find(token_id);
        if (ob_it != order_books_.end() && ob_it->second.getMid() > 0) {
            double current_price = ob_it->second.getMid();
            unrealized += position.quantity * (current_price - position.avg_entry_price);
        }
    }
    
    return unrealized;
}

void StrategyEngine::updatePosition(const TokenId& token_id, double qty, double price, Side side) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    
    auto& pos = positions_[token_id];
    double signed_qty = (side == Side::BUY) ? qty : -qty;
    
    // Update position and average entry price
    if ((pos.quantity > 0 && signed_qty > 0) || (pos.quantity < 0 && signed_qty < 0)) {
        // Adding to position - update average price
        double total_cost = (pos.quantity * pos.avg_entry_price) + (signed_qty * price);
        pos.quantity += signed_qty;
        pos.avg_entry_price = total_cost / pos.quantity;
    } else if (std::abs(signed_qty) >= std::abs(pos.quantity)) {
        // Closing or flipping position - realize PnL
        double pnl = pos.quantity * (price - pos.avg_entry_price);
        pos.realized_pnl += pnl;
        
        pos.quantity += signed_qty;
        pos.avg_entry_price = price;
    } else {
        // Partial close - realize proportional PnL
        double pnl = -signed_qty * (price - pos.avg_entry_price);
        pos.realized_pnl += pnl;
        pos.quantity += signed_qty;
    }
}

void StrategyEngine::startLogging(const std::string& event_name) {
    if (trade_logger_) {
        trade_logger_->startSession(event_name);
    }
}

void StrategyEngine::snapshotPositions() {
    if (!trade_logger_) return;
    
    std::unordered_map<TokenId, double> positions;
    std::unordered_map<TokenId, double> avg_costs;
    std::unordered_map<TokenId, double> market_values;
    
    for (const auto& [token_id, mm] : market_makers_) {
        positions[token_id] = mm.getInventory();
        
        auto ob_it = order_books_.find(token_id);
        if (ob_it != order_books_.end()) {
            double mid = ob_it->second.getMid();
            market_values[token_id] = mm.getInventory() * mid;
            avg_costs[token_id] = mid;  // Simplified
        }
    }
    
    trade_logger_->snapshotPositions(positions, avg_costs, market_values);
}

} // namespace pmm
