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
};

class MarketMaker {
public:
    MarketMaker(double spread_pct = 0.02, double max_position = 1000.0);
    
    std::optional<Quote> generateQuote(const OrderBook& book);
    
    void updateInventory(Side side, Size filled_size, Price fill_price);
    
    double getInventory() const { return inventory_; }
    double getInventoryDollars() const { return inventory_dollars_; }
    double getRealizedPnL() const { return realized_pnl_; }
    
    void updateVolatility(Price old_mid, Price new_mid, double time_elapsed_seconds);

private:
    double spread_pct_;      // Target spread (e.g., 0.02 = 2%)
    double max_position_;    // Max position size

    // AV params
    double volatility_ = 0.05; // Estimated volatility (standard deviation of mid price changes)
    double risk_aversion_ = 0.1;   // Risk aversion parameter

    // Position tracking
    double inventory_;       // Current inventory (positive = long, negative = short)
    double inventory_dollars_; // Dollar value of current inventory
    double realized_pnl_; // Realized profit and loss
    double avg_cost_;

    // For volatility calculation
    Price last_mid_;
    std::chrono::steady_clock::time_point last_update_time_;

    Price roundToCent(Price price);
};

} // namespace pmm