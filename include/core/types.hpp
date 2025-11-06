#pragma once  // Include guard (modern alternative to #ifndef)

#include <string>
#include <vector>
#include <variant>
#include <chrono>

namespace pmm {  // "Polymarket Market Maker" namespace

using Price = double;
using Size = double;
using OrderId = std::string;
using TokenId = std::string;

enum class Side {
    BUY,
    SELL
};

enum class EventType {
    MARKET_DATA_UPDATE,
    ORDER_FILL,
    ORDER_REJECTED,
    TIMER_TICK,
    SHUTDOWN
};

struct MarketDataPayload {
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
        MarketDataPayload,
        OrderFillPayload,
        OrderRejectedPayload,
        TimerTickPayload,
        ShutdownPayload
    > payload;

    static Event marketData(TokenId token_id,
                            std::vector<std::pair<Price, Size>> bids,
                            std::vector<std::pair<Price, Size>> asks) {
        return Event{
            EventType::MARKET_DATA_UPDATE,
            std::chrono::system_clock::now(),
            MarketDataPayload{std::move(token_id), std::move(bids), std::move(asks)}
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


} // namespace pmm