#include "strategy/strategy_engine.hpp"
#include "utils/logger.hpp"
#include <iostream>

namespace pmm {

StrategyEngine::StrategyEngine(EventQueue& queue, TradingMode mode)
    : event_queue_(queue),
    state_persistence_(std::make_unique<StatePersistence>("./state.json")),
    trading_logger_(std::make_unique<TradingLogger>("./logs")),
    order_manager_(queue, mode, trading_logger_.get()),
    running_(false) {
    LOG_INFO("StrategyEngine initialized");
}

StrategyEngine::~StrategyEngine() {
    stop();
}

void StrategyEngine::start() {
    if (running_.load()) {
        LOG_INFO("StrategyEngine already running");
        return;
    }
    
    running_ = true;
    strategy_thread_ = std::thread(&StrategyEngine::run, this);
    LOG_INFO("StrategyEngine started");
}

void StrategyEngine::stop() {
    if (!running_.load()) {
        return;
    }
    
    LOG_DEBUG("Stopping StrategyEngine...");
    running_.store(false);
    
    event_queue_.push(Event::shutdown("Strategy shutdown"));
    
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    
    LOG_INFO("StrategyEngine stopped");
}

void StrategyEngine::run() {
    LOG_DEBUG("StrategyEngine event loop started");
    
    auto last_snapshot = std::chrono::steady_clock::now();

    while (running_.load()) {
        Event event = event_queue_.pop();
  
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
                LOG_DEBUG("Received shutdown event");
                running_.store(false);
                break;
                
            default:
                LOG_WARN("Unknown event type");
                break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_snapshot > std::chrono::seconds(60)) {
            snapshotPositions();
            last_snapshot = now;
        }
    }
    
    LOG_INFO("StrategyEngine event loop exited");
}

void StrategyEngine::handleBookSnapshot(const Event& event) {
    auto& payload = std::get<BookSnapshotPayload>(event.payload);

    std::string market_name = payload.token_id;
    auto it = market_metadata_.find(payload.token_id);
    if (it != market_metadata_.end()) {
        market_name = it->second.title + " - " + it->second.outcome;
    }
    LOG_INFO("Processing book snapshot for {}", market_name);
    LOG_INFO("       Bids: {} levels | Asks: {} levels", payload.bids.size(), payload.asks.size());
        
    OrderBook& book = getOrCreateOrderBook(payload.token_id, market_name);
    book.clear();
    
    for (const auto& [price, size] : payload.bids) {
        book.updateBid(price, size);
    }
    for (const auto& [price, size] : payload.asks) {
        book.updateAsk(price, size);
    }
    
    LOG_DEBUG("Order book updated: {} - Best bid: {}, Best ask: {}, Spread: {}", market_name,
              book.getBestBid(),
              book.getBestAsk(),
              book.getSpread());
    
    
    order_manager_.updateOrderBook(payload.token_id, book);
    calculateQuotes(payload.token_id, market_name);
}

void StrategyEngine::handlePriceUpdate(const Event& event) {
    auto& payload = std::get<PriceLevelUpdatePayload>(event.payload);
    auto token_id = payload.token_id;
    auto market_name = market_metadata_.find(token_id) != market_metadata_.end() ?
                       market_metadata_[token_id].title + " - " + market_metadata_[token_id].outcome :
                       token_id;
    LOG_INFO("Processing price update for {}", market_name);
    LOG_INFO("       Bids: {} levels | Asks: {} levels", payload.bids.size(), payload.asks.size());

    OrderBook& book = getOrCreateOrderBook(token_id, market_name);
    
    for (const auto& [price, size] : payload.bids) {
        book.updateBid(price, size);
    }
    for (const auto& [price, size] : payload.asks) {
        book.updateAsk(price, size);
    }
    
    LOG_DEBUG("Price levels updated: {} - Best bid: {}, Best ask: {}", market_name,
              book.getBestBid(),
              book.getBestAsk());

    calculateQuotes(token_id, market_name);
}

void StrategyEngine::handleOrderFill(const Event& event) {
    auto& payload = std::get<OrderFillPayload>(event.payload);
    auto market_name = market_metadata_.find(payload.token_id) != market_metadata_.end() ?
                    market_metadata_[payload.token_id].title + " - " + market_metadata_[payload.token_id].outcome :
                    payload.token_id;
    
    LOG_INFO(">>> FILL EVENT: {}", payload.order_id);
    LOG_INFO("    Market: {}", market_name);
    LOG_INFO("    Side: {}", (payload.side == Side::BUY ? "BUY" : "SELL"));
    LOG_INFO("    Size: {} @ {}", payload.filled_size, payload.fill_price);

    updatePosition(payload.token_id, payload.filled_size, payload.fill_price, payload.side);
    
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto& pos = positions_[payload.token_id];
        LOG_INFO("    New position: {} @ avg {} | Realized PnL: ${}", pos.quantity, pos.avg_entry_price, pos.realized_pnl);
    }
    
    auto mm_it = market_makers_.find(payload.token_id);
    if (mm_it != market_makers_.end()) {
        mm_it->second.updateInventory(
            payload.side,
            payload.filled_size,
            payload.fill_price
        );
    }

    auto& book = order_books_[payload.token_id];
    double spread_bps = (book.getSpread() / book.getMid()) * 10000;
    double imbalance = book.getImbalance();

    LOG_INFO("  Market context: spread={:.1f}bps, imbalance={:.2f}, our_inventory={}", 
         spread_bps, imbalance, mm_it->second.getInventory());

    if (trading_logger_) {
        trading_logger_->logOrderFilled(
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
    LOG_ERROR("Order rejected: {} - Reason: {}", payload.order_id, payload.reason);
    
    // TODO: Handle rejection logic
}

void StrategyEngine::calculateQuotes(const TokenId& token_id, 
                                   const std::string& market_name) {
    auto it = order_books_.find(token_id);
    if (it == order_books_.end()) {
        LOG_ERROR("No order book found for token: {} , market: {}", token_id, market_name);
        return;
    }
    
    const OrderBook& book = it->second;
    
    if (!book.hasValidBBO()) {
        LOG_WARN("No valid BBO for {}, skipping quote calculation", token_id);
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
            LOG_INFO("  Orders already at target prices, no update needed");
            return;
        }
        LOG_INFO("\n  PLACING ORDERS:");
        LOG_INFO("  BID: {} x {}", quote.bid_price, quote.bid_size);
        LOG_INFO("  ASK: {} x {}", quote.ask_price, quote.ask_size);
        
        order_manager_.cancelAllOrders(token_id, market_name);
        
        order_manager_.placeOrder(token_id, Side::BUY, quote.bid_price, quote.bid_size, market_name);
        order_manager_.placeOrder(token_id, Side::SELL, quote.ask_price, quote.ask_size, market_name);
    }
}

OrderBook& StrategyEngine::getOrCreateOrderBook(const TokenId& token_id, const std::string& market_name) {
    auto it = order_books_.find(token_id);
    if (it == order_books_.end()) {
        LOG_DEBUG("Creating new order book for token: {}", market_name);
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
    LOG_DEBUG("Registered: {} - {}", title, outcome);
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
    if (trading_logger_) {
        trading_logger_->startSession(event_name);
    }
}

void StrategyEngine::snapshotPositions() {
    if (!state_persistence_ && !trading_logger_) return;
    
    std::lock_guard<std::mutex> lock(positions_mutex_);
    
    // Save state for recovery
    if (state_persistence_) {
        TradingState state;
        state.last_session_id = trading_logger_ ? trading_logger_->getSessionId() : "";
        state.last_updated = std::chrono::system_clock::now();
        
        for (const auto& [token_id, pos] : positions_) {
            PositionState ps;
            ps.quantity = pos.quantity;
            ps.avg_cost = pos.avg_entry_price;
            ps.realized_pnl = pos.realized_pnl;
            state.positions[token_id] = ps;
            state.total_realized_pnl += pos.realized_pnl;
        }
        
        // Would need to track these properly
        state.total_trades = 0;  // TODO: track in strategy
        state.total_volume = 0.0;  // TODO: track in strategy
        
        state_persistence_->saveState(state);
    }
    
    // Log positions for audit trail
    if (trading_logger_) {
        for (const auto& [token_id, pos] : positions_) {
            auto ob_it = order_books_.find(token_id);
            double market_value = 0.0;
            double unrealized_pnl = 0.0;
            
            if (ob_it != order_books_.end() && ob_it->second.getMid() > 0) {
                double mid = ob_it->second.getMid();
                market_value = pos.quantity * mid;
                unrealized_pnl = pos.quantity * (mid - pos.avg_entry_price);
            }
            
            std::string market_name = token_id;
            auto meta_it = market_metadata_.find(token_id);
            if (meta_it != market_metadata_.end()) {
                market_name = meta_it->second.title + " - " + meta_it->second.outcome;
            }
            
            trading_logger_->logPosition(market_name, token_id, pos.quantity, 
                                        pos.avg_entry_price, market_value, unrealized_pnl);
        }
    }
}

} // namespace pmm
