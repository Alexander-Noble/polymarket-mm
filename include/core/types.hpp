#pragma once 

#include <string>
#include <vector>
#include <variant>
#include <chrono>

namespace pmm {

using Price = double;
using Size = double;
using Volume = double;
using OrderId = std::string;
using TokenId = std::string;
using MarketId = std::string;

enum class Side {
    BUY,
    SELL
};

enum class MarketPhase {
    PRE_MATCH_EARLY,    // 2+ hours before
    PRE_MATCH_LATE,     // Last hour
    PRE_MATCH_CRITICAL, // Last 10 minutes
    IN_PLAY             // Event started
};

enum class EventType {
    BOOK_SNAPSHOT,
    PRICE_LEVEL_UPDATE,
    TRADE,
    ORDER_FILL,
    ORDER_REJECTED,
    TIMER_TICK,
    SHUTDOWN
};

struct BookSnapshotPayload {
    TokenId token_id;
    std::vector<std::pair<Price, Size>> bids;
    std::vector<std::pair<Price, Size>> asks;
};

struct PriceLevelUpdatePayload {
    TokenId token_id;
    std::vector<std::pair<Price, Size>> bids;
    std::vector<std::pair<Price, Size>> asks;
};

struct OrderFillPayload {
    OrderId order_id;
    TokenId token_id;
    Price fill_price;
    Size filled_size;
    Side side;
    
};

struct OrderRejectedPayload {
    OrderId order_id;
    std::string reason;
};

struct TimerTickPayload {};

struct ShutdownPayload {
    std::string reason;
};

struct Event {
    EventType type;
    std::chrono::system_clock::time_point timestamp;

    std::variant<
        BookSnapshotPayload,
        PriceLevelUpdatePayload,
        OrderFillPayload,
        OrderRejectedPayload,
        TimerTickPayload,
        ShutdownPayload
    > payload;

    static Event bookSnapshot(TokenId token_id,
                                 std::vector<std::pair<Price, Size>> bids,
                                 std::vector<std::pair<Price, Size>> asks) {
        return Event{
            EventType::BOOK_SNAPSHOT,
            std::chrono::system_clock::now(),
            BookSnapshotPayload{std::move(token_id), std::move(bids), std::move(asks)}
        };
    }

    static Event priceLevelUpdate(TokenId token_id,
                                   std::vector<std::pair<Price, Size>> bids,
                                   std::vector<std::pair<Price, Size>> asks) {
        return Event{
            EventType::PRICE_LEVEL_UPDATE,
            std::chrono::system_clock::now(),
            PriceLevelUpdatePayload{std::move(token_id), std::move(bids), std::move(asks)}
        };
    }

    static Event orderFill(OrderId order_id,
                           TokenId token_id,
                           Price fill_price,
                           Size filled_size,
                           Side side) {
        return Event{
            EventType::ORDER_FILL,
            std::chrono::system_clock::now(),
            OrderFillPayload{std::move(order_id), std::move(token_id), fill_price, filled_size, side}
        };
    }

    static Event orderRejected(OrderId order_id,
                               std::string reason) {
        return Event{
            EventType::ORDER_REJECTED,
            std::chrono::system_clock::now(),
            OrderRejectedPayload{std::move(order_id), std::move(reason)}
        };
    }

    static Event timerTick() {
        return Event{
            EventType::TIMER_TICK,
            std::chrono::system_clock::now(),
            TimerTickPayload{}
        };
    }

    static Event shutdown(std::string reason) {
        return Event{
            EventType::SHUTDOWN,
            std::chrono::system_clock::now(),
            ShutdownPayload{std::move(reason)}
        };
    }

};

enum class OrderStatus {
    OPEN,
    FILLED,
    CANCELLED
};


struct Order {
    OrderId order_id;
    TokenId token_id;
    Side side;
    Price price;
    Size size;
    Size filled_size;
    OrderStatus status;
    std::chrono::steady_clock::time_point created_at;
};

struct MarketMetadata {
    std::string title;        // e.g., "Aston Villa vs Bournemouth"
    std::string outcome;      // e.g., "Villa Win", "Draw", "Bournemouth Win"
    std::string market_id;    // Polymarket's market ID for this specific market
    std::string condition_id; // Polymarket condition ID (groups related outcome markets)
    std::chrono::system_clock::time_point event_end_time;  // When the event ends
    bool has_end_time = false;
    
    // Get market phase based on time to event
    MarketPhase getMarketPhase() const {
        if (!has_end_time) {
            return MarketPhase::PRE_MATCH_EARLY;  // Default to conservative
        }
        
        auto now = std::chrono::system_clock::now();
        auto time_to_match = std::chrono::duration_cast<std::chrono::minutes>(event_end_time - now);
        
        if (time_to_match.count() < 0) {
            return MarketPhase::IN_PLAY;  // Event started
        } else if (time_to_match.count() < 10) {
            return MarketPhase::PRE_MATCH_CRITICAL;
        } else if (time_to_match.count() < 60) {
            return MarketPhase::PRE_MATCH_LATE;
        } else {
            return MarketPhase::PRE_MATCH_EARLY;
        }
    }
    
    // Get recommended TTL in seconds based on market phase
    int getRecommendedTTL() const {
        switch (getMarketPhase()) {
            case MarketPhase::PRE_MATCH_EARLY:
                return 90;  // 60-120 seconds
            case MarketPhase::PRE_MATCH_LATE:
                return 45;  // 30-60 seconds
            case MarketPhase::PRE_MATCH_CRITICAL:
                return 20;  // 10-30 seconds
            case MarketPhase::IN_PLAY:
                return 3;   // 1-5 seconds
        }
        return 90;  // Default
    }
    
    // Get recommended requote interval in seconds
    int getRequoteInterval() const {
        switch (getMarketPhase()) {
            case MarketPhase::PRE_MATCH_EARLY:
                return 45;  // Every 30-60 seconds
            case MarketPhase::PRE_MATCH_LATE:
                return 22;  // Every 15-30 seconds
            case MarketPhase::PRE_MATCH_CRITICAL:
                return 7;   // Every 5-10 seconds
            case MarketPhase::IN_PLAY:
                return 1;   // Constantly
        }
        return 45;  // Default
    }
};

// Polymarket market info from API
// NOTE: Polymarket's naming is confusing:
//   - market_id: Unique ID for this specific market (e.g., "Will X win?")
//   - condition_id: Groups related markets together (e.g., all outcomes for an event)
//   - token_id: ERC-1155 token ID for a specific outcome (Yes/No)
struct MarketInfo {
    std::string event_title;
    std::string market_id;      // Polymarket's market ID for this specific market
    std::string condition_id;   // Groups related outcome markets (use this for event tracking)
    std::string question;
    std::string description; 
    std::vector<TokenId> tokens;
    std::vector<std::string> outcomes;
    std::string tags;
    std::string slug;
    bool active;
    double volume;
    double liquidity;
    MarketMetadata metadata;
};

struct EventInfo {
    std::string event_id;
    std::string title;
    std::string slug;
    std::string description;
    std::string start_date;
    std::string end_date;
    std::string category;
    bool active;
    bool closed;
    double volume;
    double liquidity;
    std::vector<MarketInfo> markets;
};


} // namespace pmm