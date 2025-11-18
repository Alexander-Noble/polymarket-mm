#include "strategy/strategy_engine.hpp"
#include "utils/logger.hpp"
#include <iostream>
#include <unordered_set>
#include <algorithm>

namespace pmm {

StrategyEngine::StrategyEngine(EventQueue& queue, TradingMode mode)
    : event_queue_(queue),
    state_persistence_(std::make_unique<StatePersistence>("./state.json")),
    trading_logger_(std::make_unique<TradingLogger>("./logs")),
    market_summary_logger_(nullptr),  // Initialized in startLogging
    as_manager_(std::make_unique<AdverseSelectionManager>(0.02)),
    order_manager_(queue, mode, trading_logger_.get()),
    running_(false) {
    LOG_INFO("StrategyEngine initialized");
    
    // Load previous state if available
    LOG_INFO("Attempting to load previous trading state...");
    TradingState loaded_state = state_persistence_->loadState();
    
    if (!loaded_state.positions.empty()) {
        LOG_INFO("Restoring {} positions from previous session", loaded_state.positions.size());
        std::lock_guard<std::mutex> lock(positions_mutex_);
        
        auto now = std::chrono::system_clock::now();
        
        for (const auto& [token_id, pos_state] : loaded_state.positions) {
            Position pos;
            pos.quantity = pos_state.quantity;
            pos.avg_entry_price = pos_state.avg_cost;
            pos.realized_pnl = pos_state.realized_pnl;
            // Initialize timestamps for restored positions (we don't persist these)
            pos.opened_at = now;
            pos.last_updated = now;
            pos.entry_side = (pos_state.quantity > 0) ? Side::BUY : Side::SELL;
            pos.num_fills = 0;  // Reset for restored positions
            positions_[token_id] = pos;
            
            LOG_INFO("  Restored position: {} | Qty: {:.2f} @ {:.3f} | Realized PnL: ${:.2f}",
                     token_id, pos.quantity, pos.avg_entry_price, pos.realized_pnl);
        }
        
        LOG_INFO("Total realized PnL from previous sessions: ${:.2f}", loaded_state.total_realized_pnl);
    } else {
        LOG_INFO("No previous positions to restore - starting fresh");
    }
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
    auto last_quote_check = std::chrono::steady_clock::now();
    auto last_summary_check = std::chrono::steady_clock::now();

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
                
            case EventType::TIMER_TICK:
                // Check for expired quotes on timer tick
                checkExpiredQuotes();
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
        
        // Check expired quotes every second
        if (now - last_quote_check > std::chrono::seconds(1)) {
            checkExpiredQuotes();
            last_quote_check = now;
        }
        
        // Check if we should log market summaries (adaptive interval)
        if (market_summary_logger_ && (now - last_summary_check > std::chrono::seconds(5))) {
            if (market_summary_logger_->shouldLogSummary()) {
                market_summary_logger_->logSummaries();
            }
            last_summary_check = now;
        }
        
        if (now - last_snapshot > std::chrono::seconds(60)) {
            snapshotPositions();
            checkPendingFillMetrics();
            logQuoteSummary();
            as_manager_->decay();  // Decay adverse selection adjustments
            last_snapshot = now;
        }
    }
    
    LOG_INFO("StrategyEngine event loop exited");
}

void StrategyEngine::handleBookSnapshot(const Event& event) {
    auto& payload = std::get<BookSnapshotPayload>(event.payload);

    // Check if this is a token we registered
    auto it = market_metadata_.find(payload.token_id);
    bool is_registered = (it != market_metadata_.end());
    
    std::string market_name = payload.token_id;
    if (is_registered) {
        market_name = it->second.title + " - " + it->second.outcome;
        LOG_DEBUG("[REGISTERED] Book snapshot for {}: {} bids, {} asks", market_name, payload.bids.size(), payload.asks.size());
    } else {
        LOG_DEBUG("[UNREGISTERED] Book snapshot for token {}: {} bids, {} asks", payload.token_id.substr(0, 16), payload.bids.size(), payload.asks.size());
    }
    LOG_DEBUG("Book snapshot for {}: {} bids, {} asks", market_name, payload.bids.size(), payload.asks.size());
        
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
    
    // Log initial positions once we have market data for at least one position
    if (!initial_positions_logged_.load() && !positions_.empty()) {
        bool has_position_with_book = false;
        {
            std::lock_guard<std::mutex> lock(positions_mutex_);
            for (const auto& [token_id, pos] : positions_) {
                auto ob_it = order_books_.find(token_id);
                if (ob_it != order_books_.end() && ob_it->second.hasValidBBO()) {
                    has_position_with_book = true;
                    break;
                }
            }
        }
        
        if (has_position_with_book) {
            initial_positions_logged_.store(true);
            logInitialPositions();
        }
    }
    
    order_manager_.updateOrderBook(payload.token_id, book);
    
    // Only calculate quotes for registered (tradable) tokens
    if (it != market_metadata_.end()) {
        calculateQuotes(payload.token_id, market_name);
    } else {
        LOG_DEBUG("Skipping quote calculation for unregistered token");
    }
}

void StrategyEngine::handlePriceUpdate(const Event& event) {
    auto& payload = std::get<PriceLevelUpdatePayload>(event.payload);
    auto token_id = payload.token_id;
    
    // Check if this is a token we registered
    auto metadata_it = market_metadata_.find(token_id);
    bool is_registered = (metadata_it != market_metadata_.end());
    
    std::string market_name = token_id;
    if (is_registered) {
        market_name = metadata_it->second.title + " - " + metadata_it->second.outcome;
        LOG_DEBUG("[REGISTERED] Price update for {}: {} bids, {} asks", market_name, payload.bids.size(), payload.asks.size());
    } else {
        LOG_DEBUG("[UNREGISTERED] Price update for token {}: {} bids, {} asks", token_id.substr(0, 16), payload.bids.size(), payload.asks.size());
    }
    LOG_DEBUG("Price update for {}: {} bids, {} asks", market_name, payload.bids.size(), payload.asks.size());

    OrderBook& book = getOrCreateOrderBook(token_id, market_name);
    
    // Get previous state before updating
    PriceUpdateHistory prev_state;
    {
        std::lock_guard<std::mutex> lock(price_history_mutex_);
        auto it = price_history_.find(token_id);
        if (it != price_history_.end()) {
            prev_state = it->second;
        }
    }
    
    for (const auto& [price, size] : payload.bids) {
        book.updateBid(price, size);
    }
    for (const auto& [price, size] : payload.asks) {
        book.updateAsk(price, size);
    }
    
    LOG_DEBUG("Price levels updated: {} - Best bid: {}, Best ask: {}", market_name,
              book.getBestBid(),
              book.getBestAsk());

    // Update adverse selection metrics with current price
    as_manager_->updateMetrics(token_id, book.getMid());

    // Calculate and log price update metrics
    if (book.hasValidBBO() && trading_logger_) {
        Price current_mid = book.getMid();
        double price_change_pct = 0.0;
        double price_change_abs = 0.0;
        double seconds_since_last = 0.0;
        
        auto now = std::chrono::steady_clock::now();
        
        if (prev_state.last_mid > 0.0) {
            price_change_abs = current_mid - prev_state.last_mid;
            price_change_pct = (price_change_abs / prev_state.last_mid) * 100.0;
            seconds_since_last = std::chrono::duration<double>(now - prev_state.last_update_time).count();
        }
        
        // Get current volumes
        double bid_volume = book.getTotalBidVolume(5);
        double ask_volume = book.getTotalAskVolume(5);
        double total_volume = bid_volume + ask_volume;
        double volume_imbalance = book.getImbalance();
        
        // Get spread metrics
        Price spread = book.getSpread();
        double spread_bps = (spread / current_mid) * 10000.0;
        
        // Get level counts
        int bid_levels = book.getBidLevelCount();
        int ask_levels = book.getAskLevelCount();
        
        // Get our inventory
        double our_inventory = 0.0;
        auto mm_it = market_makers_.find(token_id);
        if (mm_it != market_makers_.end()) {
            our_inventory = mm_it->second.getInventory();
        }
        
        // Calculate time to event from stored event end time
        double time_to_event_hours = -1.0;  // -1 indicates unknown
        auto metadata_it = market_metadata_.find(token_id);
        if (metadata_it != market_metadata_.end() && metadata_it->second.has_end_time) {
            auto time_remaining = metadata_it->second.event_end_time - std::chrono::system_clock::now();
            time_to_event_hours = std::chrono::duration<double, std::ratio<3600>>(time_remaining).count();
        }
        
        // Get market_id and condition_id from metadata, or "UNKNOWN" for unregistered tokens
        std::string market_id = "UNKNOWN";
        std::string condition_id = "UNKNOWN";
        if (metadata_it != market_metadata_.end()) {
            market_id = metadata_it->second.market_id;
            condition_id = metadata_it->second.condition_id;
        }
        
        // Log the price update
        trading_logger_->logPriceUpdate(
            market_name,
            market_id,
            condition_id,
            token_id,
            current_mid,
            price_change_pct,
            price_change_abs,
            book.getBestBid(),
            book.getBestAsk(),
            spread,
            spread_bps,
            bid_volume,
            ask_volume,
            total_volume,
            volume_imbalance,
            bid_levels,
            ask_levels,
            our_inventory,
            time_to_event_hours,
            seconds_since_last
        );
        
        // Update market summary logger if available
        if (market_summary_logger_ && metadata_it != market_metadata_.end()) {
            market_summary_logger_->updateMarket(
                market_name,
                market_id,
                condition_id,
                token_id,
                current_mid,
                spread_bps,
                book.getBestBid(),
                book.getBestAsk(),
                bid_volume,
                ask_volume,
                bid_levels,
                ask_levels
            );
        }
        
        // Update price history for next comparison
        {
            std::lock_guard<std::mutex> lock(price_history_mutex_);
            PriceUpdateHistory& history = price_history_[token_id];
            history.last_mid = current_mid;
            history.last_bid_volume = bid_volume;
            history.last_ask_volume = ask_volume;
            history.last_update_time = now;
        }
    }

    // Only calculate quotes for registered (tradable) tokens
    if (is_registered) {
        calculateQuotes(token_id, market_name);
    } else {
        LOG_DEBUG("Skipping quote calculation for unregistered token");
    }
}

void StrategyEngine::handleOrderFill(const Event& event) {
    auto& payload = std::get<OrderFillPayload>(event.payload);
    auto market_name = market_metadata_.find(payload.token_id) != market_metadata_.end() ?
                    market_metadata_[payload.token_id].title + " - " + market_metadata_[payload.token_id].outcome :
                    payload.token_id;
    
    LOG_INFO("FILL EVENT: {}", payload.order_id);
    LOG_INFO("Market: {}", market_name);
    LOG_INFO("Side: {}", (payload.side == Side::BUY ? "BUY" : "SELL"));
    LOG_INFO("Size: {} @ {}", payload.filled_size, payload.fill_price);
    
    // Capture market context at fill time
    auto ob_it = order_books_.find(payload.token_id);
    if (ob_it != order_books_.end()) {
        const auto& book = ob_it->second;
        double spread_bps = (book.getSpread() / book.getMid()) * 10000;
        double imbalance = book.getImbalance();
        
        LOG_INFO("Market context: spread={:.1f}bps, imbalance={:.2f}, mid={:.3f}", 
                 spread_bps, imbalance, book.getMid());
        
        // Store fill metrics for adverse selection analysis
        auto mm_it = market_makers_.find(payload.token_id);
        double inventory_before = mm_it != market_makers_.end() ? mm_it->second.getInventory() : 0.0;
        
        FillMetrics metrics;
        metrics.fill_time = std::chrono::system_clock::now();
        metrics.token_id = payload.token_id;
        metrics.order_id = payload.order_id;
        metrics.side = payload.side;
        metrics.fill_price = payload.fill_price;
        metrics.mid_at_fill = book.getMid();
        metrics.best_bid_at_fill = book.getBestBid();
        metrics.best_ask_at_fill = book.getBestAsk();
        metrics.spread_at_fill = book.getSpread();
        metrics.imbalance_at_fill = imbalance;
        metrics.inventory_before = inventory_before;
        
        std::lock_guard<std::mutex> lock(fill_metrics_mutex_);
        fill_history_.push_back(metrics);
    }

    total_fills_.fetch_add(1, std::memory_order_relaxed);
    
    updatePosition(payload.token_id, payload.filled_size, payload.fill_price, payload.side);
    
    {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto& pos = positions_[payload.token_id];
        LOG_INFO("New position: {} @ avg {} | Realized PnL: ${}", pos.quantity, pos.avg_entry_price, pos.realized_pnl);
    }
    
    auto mm_it = market_makers_.find(payload.token_id);
    if (mm_it != market_makers_.end()) {
        mm_it->second.updateInventory(
            payload.side,
            payload.filled_size,
            payload.fill_price
        );
        
        // Log PnL breakdown
        auto ob_it = order_books_.find(payload.token_id);
        if (ob_it != order_books_.end() && ob_it->second.getMid() > 0) {
            double realized = mm_it->second.getRealizedPnL();
            double unrealized = mm_it->second.getUnrealizedPnL(ob_it->second.getMid());
            LOG_INFO("  PnL: Realized: ${:.2f}, Unrealized: ${:.2f}, Total: ${:.2f}", 
                     realized, unrealized, realized + unrealized);
        }
        
        // Update fill metrics with inventory after
        std::lock_guard<std::mutex> lock(fill_metrics_mutex_);
        if (!fill_history_.empty()) {
            fill_history_.back().inventory_after = mm_it->second.getInventory();
        }
        
        // Record fill for adverse selection tracking
        auto book_it = order_books_.find(payload.token_id);
        if (book_it != order_books_.end()) {
            as_manager_->recordFill(
                payload.token_id,
                payload.order_id,
                payload.side,
                payload.fill_price,
                book_it->second.getMid(),
                fill_history_.back().inventory_before
            );
        }
    }
    
    if (trading_logger_) {
        // Get quoted price from active_quotes if available
        Price quoted_price = payload.fill_price;  // Default to fill price
        double seconds_to_fill = 0.0;
        
        {
            std::lock_guard<std::mutex> lock(quotes_mutex_);
            auto quote_it = active_quotes_.find(payload.token_id);
            if (quote_it != active_quotes_.end()) {
                // Determine quoted price based on side
                quoted_price = (payload.side == Side::BUY) ? 
                              quote_it->second.bid_price : quote_it->second.ask_price;
                
                // Calculate time to fill
                auto now = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(
                    now - quote_it->second.quote_created_at);
                seconds_to_fill = duration.count();
            }
        }
        
        // Get mid price at fill from order book
        Price mid_at_fill = 0.0;
        auto book_it = order_books_.find(payload.token_id);
        if (book_it != order_books_.end()) {
            mid_at_fill = book_it->second.getMid();
        }
        
        trading_logger_->logOrderFilled(
            market_name,
            payload.order_id,
            payload.token_id,
            payload.fill_price,
            payload.filled_size,
            payload.side,
            mm_it->second.getRealizedPnL(),
            quoted_price,
            mid_at_fill,
            seconds_to_fill
        );
        
        // Log position change after fill
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto& pos = positions_[payload.token_id];
        double total_cost = pos.quantity * pos.avg_entry_price;
        
        trading_logger_->logPosition(market_name, payload.token_id, pos.quantity,
                                    pos.avg_entry_price, pos.opened_at, pos.last_updated,
                                    pos.entry_side, pos.num_fills, total_cost);
    }

    calculateQuotes(payload.token_id, market_name);
}

void StrategyEngine::handleOrderRejected(const Event& event) {
    auto& payload = std::get<OrderRejectedPayload>(event.payload);
    LOG_ERROR("Order rejected: {} - Reason: {}", payload.order_id, payload.reason);
    
    // TODO: Handle rejection logic
}

void StrategyEngine::calculateQuotes(const TokenId& token_id, 
                                   const std::string& market_name,
                                   CancelReason cancel_reason) {
    auto it = order_books_.find(token_id);
    if (it == order_books_.end()) {
        LOG_ERROR("No order book found for token: {} , market: {}", token_id, market_name);
        return;
    }
    
    const OrderBook& book = it->second;
    
    // Check if this token has a market maker (i.e., it's tradable)
    // Don't auto-create market makers - only trade explicitly registered markets
    auto mm_it = market_makers_.find(token_id);
    bool is_tradable = (mm_it != market_makers_.end());
    
    if (!book.hasValidBBO()) {
        // Only warn for tradable tokens - observation-only tokens (No side) may have incomplete books
        if (is_tradable) {
            LOG_WARN("No valid BBO for {}, skipping quote calculation", token_id);
        } else {
            LOG_DEBUG("Incomplete BBO for observation-only token {} ({})", market_name, token_id);
        }
        return;
    }
    
    // Skip quoting if this is an observation-only token (no market maker registered)
    if (!is_tradable) {
        LOG_DEBUG("Skipping quotes for observation-only token {} ({})", market_name, token_id);
        return;
    }

    // Restore inventory from persisted state if available (only on first quote)
    static std::unordered_set<TokenId> restored_markets;
    if (restored_markets.find(token_id) == restored_markets.end()) {
        std::lock_guard<std::mutex> lock(positions_mutex_);
        auto pos_it = positions_.find(token_id);
        if (pos_it != positions_.end() && std::abs(pos_it->second.quantity) > 0.001) {
            mm_it->second.restoreState(
                pos_it->second.quantity,
                pos_it->second.avg_entry_price,
                pos_it->second.realized_pnl
            );
            LOG_DEBUG("Restored MarketMaker inventory for token: {}", token_id);
        }
        restored_markets.insert(token_id);
    }
    
    // Get adverse selection spread multiplier
    double inventory = mm_it->second.getInventory();
    double bid_multiplier = as_manager_->getSpreadMultiplier(token_id, Side::BUY, inventory);
    double ask_multiplier = as_manager_->getSpreadMultiplier(token_id, Side::SELL, inventory);
    double spread_multiplier = std::max(bid_multiplier, ask_multiplier);  // Use worst case
    
    // Get market metadata for TTL calculation
    const MarketMetadata* metadata = nullptr;
    auto metadata_it = market_metadata_.find(token_id);
    if (metadata_it != market_metadata_.end()) {
        metadata = &metadata_it->second;
    }
    
    auto quote_opt = mm_it->second.generateQuote(book, metadata, spread_multiplier);
    
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
        
        // Always update active_quotes_ with current state (prices, inventory, and TTL)
        {
            std::lock_guard<std::mutex> lock(quotes_mutex_);
            QuoteSummary summary;
            summary.market_name = market_name;
            summary.bid_price = quote.bid_price;
            summary.ask_price = quote.ask_price;
            summary.mid = book.getMid();
            summary.spread_bps = (quote.ask_price - quote.bid_price) / book.getMid() * 10000;
            summary.inventory = mm_it->second.getInventory();
            summary.last_update = std::chrono::steady_clock::now();
            summary.quote_created_at = quote.created_at;
            summary.ttl_seconds = quote.ttl_seconds;
            active_quotes_[token_id] = summary;
        }
        
        if (has_matching_bid && has_matching_ask) {
            return;
        }
        LOG_DEBUG("[{}] Bid {} x {} / Ask {} x {}", market_name, quote.bid_price, quote.bid_size, quote.ask_price, quote.ask_size);
        
        order_manager_.cancelAllOrders(token_id, market_name, cancel_reason);
        
        order_manager_.placeOrder(token_id, Side::BUY, quote.bid_price, quote.bid_size, market_name);
        order_manager_.placeOrder(token_id, Side::SELL, quote.ask_price, quote.ask_size, market_name);
    }
}

void StrategyEngine::checkExpiredQuotes() {
    std::vector<TokenId> expired_tokens;
    
    {
        std::lock_guard<std::mutex> lock(quotes_mutex_);
        for (const auto& [token_id, quote_summary] : active_quotes_) {
            if (quote_summary.isExpired()) {
                expired_tokens.push_back(token_id);
            }
        }
    }
    
    // Requote expired markets
    for (const auto& token_id : expired_tokens) {
        auto it = order_books_.find(token_id);
        if (it != order_books_.end() && it->second.hasValidBBO()) {
            std::string market_name = token_id;
            auto metadata_it = market_metadata_.find(token_id);
            if (metadata_it != market_metadata_.end()) {
                market_name = metadata_it->second.title + " - " + metadata_it->second.outcome;
            }
            
            LOG_DEBUG("Quote expired for {}, requoting...", market_name);
            calculateQuotes(token_id, market_name, CancelReason::TTL_EXPIRED);
        }
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
                                    const std::string& market_id,
                                    const std::string& condition_id) {
    // Store metadata
    registerMarketMetadata(token_id, title, outcome, market_id, condition_id);
    
    // Create market maker for this token (makes it tradable)
    auto it = market_makers_.find(token_id);
    if (it == market_makers_.end()) {
        market_makers_.emplace(token_id, MarketMaker());
        LOG_DEBUG("Created market maker for: {} - {}", title, outcome);
    }
}

void StrategyEngine::registerMarketMetadata(const TokenId& token_id,
                                           const std::string& title,
                                           const std::string& outcome,
                                           const std::string& market_id,
                                           const std::string& condition_id) {
    MarketMetadata metadata;
    metadata.title = title;
    metadata.outcome = outcome;
    metadata.market_id = market_id;
    metadata.condition_id = condition_id;
    metadata.has_end_time = false;
    market_metadata_[token_id] = metadata;
    LOG_DEBUG("Registered metadata: {} - {}", title, outcome);
}void StrategyEngine::setEventEndTime(const std::string& condition_id, 
                                    const std::chrono::system_clock::time_point& end_time) {
    // Update all tokens associated with this condition_id
    int updated_count = 0;
    for (auto& [token_id, metadata] : market_metadata_) {
        if (metadata.condition_id == condition_id) {
            metadata.event_end_time = end_time;
            metadata.has_end_time = true;
            updated_count++;
            
            // Also set it on the market maker for time-aware risk management
            auto mm_it = market_makers_.find(token_id);
            if (mm_it != market_makers_.end()) {
                mm_it->second.setMarketCloseTime(end_time);
            }
        }
    }
    
    // Update market summary logger
    if (market_summary_logger_) {
        market_summary_logger_->setEventEndTime(condition_id, end_time);
    }
    
    if (updated_count > 0) {
        LOG_DEBUG("Set event end time for condition {} ({} tokens)", condition_id, updated_count);
    }
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

size_t StrategyEngine::getBidCount() const {
    return order_manager_.getBidCount();
}

size_t StrategyEngine::getAskCount() const {
    return order_manager_.getAskCount();
}

size_t StrategyEngine::getActiveMarketCount() const {
    // Count unique markets (by market_id) that have orders on any token
    std::unordered_set<std::string> active_market_ids;
    
    for (const auto& [token_id, metadata] : market_metadata_) {
        auto orders = order_manager_.getOpenOrders(token_id);
        if (!orders.empty()) {
            active_market_ids.insert(metadata.market_id);
        }
    }
    
    return active_market_ids.size();
}

double StrategyEngine::getTotalInventory() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    double total = 0.0;
    for (const auto& [token_id, position] : positions_) {
        total += std::abs(position.quantity);
    }
    return total;
}

double StrategyEngine::getAverageSpread() const {
    double total_spread_pct = 0.0;
    int count = 0;
    
    for (const auto& [token_id, book] : order_books_) {
        if (book.hasValidBBO()) {
            double spread = book.getSpread();
            double mid = book.getMid();
            if (mid > 0) {
                total_spread_pct += (spread / mid);
                count++;
            }
        }
    }
    
    return (count > 0) ? (total_spread_pct / count) : 0.0;
}

size_t StrategyEngine::getFillCount() const {
    return total_fills_.load(std::memory_order_relaxed);
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
    bool was_flat = (pos.quantity == 0.0);
    
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
        
        // If we went from flat to a new position, record the opened_at time and entry side
        if (was_flat && pos.quantity != 0.0) {
            pos.opened_at = std::chrono::system_clock::now();
            pos.entry_side = side;
            pos.num_fills = 0;  // Reset fill count for new position
        }
    } else {
        // Partial close - realize proportional PnL
        double pnl = -signed_qty * (price - pos.avg_entry_price);
        pos.realized_pnl += pnl;
        pos.quantity += signed_qty;
    }
    
    // If this is a brand new position (opening from flat), set opened_at and entry_side
    if (was_flat && pos.quantity != 0.0 && pos.opened_at.time_since_epoch().count() == 0) {
        pos.opened_at = std::chrono::system_clock::now();
        pos.entry_side = side;
        pos.num_fills = 0;  // Reset fill count
    }
    
    // Always update last_updated and increment fill count
    pos.last_updated = std::chrono::system_clock::now();
    pos.num_fills++;
}

void StrategyEngine::startLogging(const std::string& event_name) {
    if (trading_logger_) {
        trading_logger_->startSession(event_name);
        
        // Initialize market summary logger with the session directory
        std::string session_id = trading_logger_->getSessionId();
        std::filesystem::path session_dir = std::filesystem::path("./logs") / session_id;
        market_summary_logger_ = std::make_unique<MarketSummaryLogger>(session_dir);
        
        LOG_INFO("Market summary logger initialized");
        
        // Don't log initial positions yet - wait until we have market data
    }
}

void StrategyEngine::logInitialPositions() {
    if (!trading_logger_) return;
    
    std::lock_guard<std::mutex> lock(positions_mutex_);
    
    if (positions_.empty()) {
        return;
    }
    
    LOG_INFO("Logging {} initial positions to session", positions_.size());
    
    for (const auto& [token_id, pos] : positions_) {
        double total_cost = pos.quantity * pos.avg_entry_price;
        
        std::string market_name = token_id;
        auto meta_it = market_metadata_.find(token_id);
        if (meta_it != market_metadata_.end()) {
            market_name = meta_it->second.title + " - " + meta_it->second.outcome;
        }
        
        trading_logger_->logPosition(market_name, token_id, pos.quantity,
                                    pos.avg_entry_price, pos.opened_at, pos.last_updated,
                                    pos.entry_side, pos.num_fills, total_cost);
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
}

void StrategyEngine::checkPendingFillMetrics() {
    std::lock_guard<std::mutex> lock(fill_metrics_mutex_);
    
    auto now = std::chrono::system_clock::now();
    
    for (auto& metrics : fill_history_) {
        if (metrics.metrics_complete) continue;
        
        auto time_since_fill = std::chrono::duration_cast<std::chrono::seconds>(now - metrics.fill_time).count();
        
        // Get current mid price
        auto ob_it = order_books_.find(metrics.token_id);
        if (ob_it == order_books_.end() || !ob_it->second.hasValidBBO()) continue;
        
        Price current_mid = ob_it->second.getMid();
        
        // Capture at 30s
        if (time_since_fill >= 30 && metrics.mid_30s_after == 0.0) {
            metrics.mid_30s_after = current_mid;
            
            double price_change_30s = (current_mid - metrics.mid_at_fill) / metrics.mid_at_fill * 100;
            double adverse_metric_30s = (metrics.side == Side::BUY) 
                ? (current_mid - metrics.fill_price)  // Positive = good, negative = adverse
                : (metrics.fill_price - current_mid); // Positive = good, negative = adverse
            
            LOG_INFO("[FILL ANALYSIS 30s] Order: {} | Side: {} | Fill: {:.3f} | Mid@Fill: {:.3f} | Mid@30s: {:.3f} | Change: {:.2f}% | Metric: {:.4f}",
                     metrics.order_id,
                     metrics.side == Side::BUY ? "BUY" : "SELL",
                     metrics.fill_price,
                     metrics.mid_at_fill,
                     current_mid,
                     price_change_30s,
                     adverse_metric_30s);
        }
        
        // Capture at 60s and mark complete
        if (time_since_fill >= 60 && metrics.mid_60s_after == 0.0) {
            metrics.mid_60s_after = current_mid;
            metrics.metrics_complete = true;
            
            double price_change_60s = (current_mid - metrics.mid_at_fill) / metrics.mid_at_fill * 100;
            double adverse_metric_60s = (metrics.side == Side::BUY) 
                ? (current_mid - metrics.fill_price)
                : (metrics.fill_price - current_mid);
            
            bool is_adverse = adverse_metric_60s < -0.01; // Lost more than 1 cent
            
            LOG_INFO("[FILL ANALYSIS 60s] Order: {} | Side: {} | Fill: {:.3f} | Mid@Fill: {:.3f} | Mid@60s: {:.3f} | Change: {:.2f}% | Metric: {:.4f} | Adverse: {}",
                     metrics.order_id,
                     metrics.side == Side::BUY ? "BUY" : "SELL",
                     metrics.fill_price,
                     metrics.mid_at_fill,
                     current_mid,
                     price_change_60s,
                     adverse_metric_60s,
                     is_adverse ? "YES" : "NO");
            
            // Log detailed context for adverse fills
            if (is_adverse) {
                LOG_WARN("ADVERSE SELECTION DETECTED!");
                LOG_WARN("Spread@Fill: {:.4f} ({:.1f}bps)", metrics.spread_at_fill, 
                         (metrics.spread_at_fill / metrics.mid_at_fill) * 10000);
                LOG_WARN("Imbalance@Fill: {:.2f}", metrics.imbalance_at_fill);
                LOG_WARN("Inventory: {:.1f} -> {:.1f}", metrics.inventory_before, metrics.inventory_after);
            }
        }
    }
    
    // Clean up old completed metrics (keep last 100)
    if (fill_history_.size() > 100) {
        auto first_incomplete = std::find_if(fill_history_.begin(), fill_history_.end(),
                                            [](const FillMetrics& m) { return !m.metrics_complete; });
        
        if (first_incomplete != fill_history_.begin() && 
            std::distance(fill_history_.begin(), first_incomplete) > 50) {
            fill_history_.erase(fill_history_.begin(), first_incomplete - 50);
        }
    }
}

void StrategyEngine::logQuoteSummary() {
    std::lock_guard<std::mutex> lock(quotes_mutex_);
        
    // Build list including both active quotes and markets with positions
    std::vector<std::pair<TokenId, QuoteSummary>> sorted_quotes;
    for (const auto& [token_id, summary] : active_quotes_) {
        sorted_quotes.push_back({token_id, summary});
    }
    
    // Add markets with positions that aren't actively quoting
    for (const auto& [token_id, mm] : market_makers_) {
        double inventory = mm.getInventory();
        if (std::abs(inventory) > 0.1 && active_quotes_.find(token_id) == active_quotes_.end()) {
            // This market has a position but isn't quoting - add it
            QuoteSummary summary;
            auto meta_it = market_metadata_.find(token_id);
            summary.market_name = (meta_it != market_metadata_.end()) ? 
                                  meta_it->second.title + " - " + meta_it->second.outcome : 
                                  token_id;
            
            auto ob_it = order_books_.find(token_id);
            if (ob_it != order_books_.end()) {
                summary.mid = ob_it->second.getMid();
                summary.bid_price = 0.0;
                summary.ask_price = 0.0;
                summary.spread_bps = 0.0;
            } else {
                summary.mid = 0.0;
                summary.bid_price = 0.0;
                summary.ask_price = 0.0;
                summary.spread_bps = 0.0;
            }
            summary.inventory = inventory;
            sorted_quotes.push_back({token_id, summary});
        }
    }
    
    if (sorted_quotes.empty()) {
        return;
    }
    
    std::sort(sorted_quotes.begin(), sorted_quotes.end(),
              [](const auto& a, const auto& b) {
                  return std::abs(a.second.inventory) > std::abs(b.second.inventory);
              });
    
    // Show top 5 by inventory risk
    LOG_INFO("\nTop markets by inventory:");
    size_t count = 0;
    for (const auto& [token_id, summary] : sorted_quotes) {
        if (count++ >= 5) break;
        if (summary.bid_price > 0 && summary.ask_price > 0) {
            int seconds_left = summary.getSecondsUntilExpiry();
            LOG_INFO("  {} | Mid: {:.3f} | Bid: {:.3f} / Ask: {:.3f} | Spread: {:.1f}bps | Inv: {:.1f} | TTL: {}s",
                     summary.market_name, summary.mid, summary.bid_price, summary.ask_price,
                     summary.spread_bps, summary.inventory, seconds_left);
        } else {
            LOG_INFO("  {} | Mid: {:.3f} | NOT QUOTING | Inv: {:.1f}",
                     summary.market_name, summary.mid, summary.inventory);
        }
    }
    
    // Calculate aggregate stats (only from active quotes)
    double avg_spread_bps = 0.0;
    if (!active_quotes_.empty()) {
        for (const auto& [token_id, summary] : active_quotes_) {
            avg_spread_bps += summary.spread_bps;
        }
        avg_spread_bps /= active_quotes_.size();
    }

    double total_short_inv = 0.0;
    double total_long_inv = 0.0;
    double total_inv = 0.0; 
    
    for (const auto& [token_id, mm] : market_makers_) {
        total_short_inv += std::min(0.0, mm.getInventory());
        total_long_inv += std::max(0.0, mm.getInventory());
        total_inv += std::abs(mm.getInventory());
    }
    
    LOG_INFO("Avg spread: {:.1f}bps | Total absolute inventory: {:.1f} | Total short inventory: {:.1f} | Total long inventory: {:.1f}",
             avg_spread_bps, total_inv, total_short_inv, total_long_inv);
}

} // namespace pmm
