#pragma once

#include "core/types.hpp"
#include "data/order_book.hpp"
#include <optional>

namespace pmm {

struct Quote {
    Price bid_price;
    Size bid_size;
    Price ask_price;
    Size ask_size;
    int ttl_seconds;
    std::chrono::steady_clock::time_point created_at;
};

class MarketMaker {
public:
    MarketMaker(double spread_pct = 0.02, double max_position = 1000.0);
    
    std::optional<Quote> generateQuote(const OrderBook& book, 
                                      const MarketMetadata* metadata = nullptr,
                                      double spread_multiplier = 1.0);
    
    void updateInventory(Side side, Size filled_size, Price fill_price);
    
    void restoreState(double inventory, double avg_cost, double realized_pnl);
    
    double getInventory() const { return inventory_; }
    double getInventoryDollars() const { return inventory_dollars_; }
    double getRealizedPnL() const { return realized_pnl_; }
    double getUnrealizedPnL(Price current_mid) const;
    
    void updateVolatility(Price old_mid, Price new_mid, double time_elapsed_seconds);
    
    void setMarketCloseTime(std::chrono::system_clock::time_point close_time);
    double getTimeUrgency() const;

private:
    double spread_pct_;
    double max_position_;

    double volatility_ = 0.05;
    double risk_aversion_ = 0.1;

    double inventory_;
    double inventory_dollars_;
    double realized_pnl_;
    double avg_cost_;

    Price last_mid_;
    std::chrono::steady_clock::time_point last_update_time_;
    
    std::chrono::system_clock::time_point market_close_time_;
    bool has_close_time_ = false;

    Price roundToCent(Price price);
};

} // namespace pmm