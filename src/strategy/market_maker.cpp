#include "strategy/market_maker.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

namespace pmm {

MarketMaker::MarketMaker(double spread_pct, double max_position)
    : spread_pct_(spread_pct),
      max_position_(max_position),
      inventory_(0.0),
      inventory_dollars_(0.0),
      avg_cost_(0.0),
      realized_pnl_(0.0),
      last_mid_(0.0),
      last_update_time_(std::chrono::steady_clock::now()) {
    
    std::cout << "MarketMaker initialized: spread=" << spread_pct 
              << ", max_pos=" << max_position 
              << ", gamma=" << risk_aversion_ 
              << ", sigma=" << volatility_ << "\n";
}

std::optional<Quote> MarketMaker::generateQuote(const OrderBook& book) {
    Price mid = book.getMid();
    Price market_spread = book.getSpread();
    
    // Update volatility estimate
    if (last_mid_ > 0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - last_update_time_).count();
        if (elapsed > 0.1) {  // Update if > 0.1 seconds elapsed
            updateVolatility(last_mid_, mid, elapsed);
        }
    }
    last_mid_ = mid;
    last_update_time_ = std::chrono::steady_clock::now();
    
    if (market_spread < 0.01) {
        std::cout << "Market spread too tight (" << market_spread << "), not quoting\n";
        return std::nullopt;
    }
    
    double target_spread_dollars = mid * spread_pct_;
    // double q = inventory_ / 100.0; // Normalize inventory
    // double gamma = risk_aversion_;
    // double sigma_sq = volatility_ * volatility_;
    
    // // Single reservation price with inventory skew
    // double reservation_price = mid - q * gamma * sigma_sq;
    
    // // Optimal spread from AV (simplified)
    // double optimal_spread = gamma * sigma_sq + (2.0 / gamma) * std::log(1.0 + gamma / spread_pct_);
    
    // // Use the larger of target spread or optimal spread
    // double actual_spread = std::max(target_spread_dollars, optimal_spread);
    
    // std::cout << "  AV calculation:\n";
    // std::cout << "    q (normalized inventory): " << q << "\n";
    // std::cout << "    gamma: " << gamma << ", sigma: " << volatility_ << "\n";
    // std::cout << "    reservation_price: " << reservation_price << "\n";
    // std::cout << "    optimal_spread: " << optimal_spread << ", target_spread: " << target_spread_dollars << "\n";

    // // Apply spread symmetrically around reservation price
    // Price our_bid = reservation_price - actual_spread / 2.0;
    // Price our_ask = reservation_price + actual_spread / 2.0;
        
    // double imbalance = book.getImbalance();
    // double imbalance_adjustment = imbalance * 0.005;  // Max 0.5% adjustment
    
    // our_bid += imbalance_adjustment;
    // our_ask += imbalance_adjustment;
    
    // std::cout << "    imbalance: " << imbalance << "  adjustment: " << imbalance_adjustment << "\n";
    
    double q = inventory_ / 100.0; // Normalize inventory
    double gamma = risk_aversion_;
    double sigma_sq = volatility_ * volatility_; // Assume some volatility estimate
    
    double reservation_bid = mid - (q + 1.0) * gamma * sigma_sq;
    double reservation_ask = mid + (q - 1.0) * gamma * sigma_sq;

    std::cout << "  AV calculation:\n";
    std::cout << "    q (normalized inventory): " << q << "\n";
    std::cout << "    gamma: " << gamma << ", sigma: " << volatility_ << "\n";
    std::cout << "    reservation_bid: " << reservation_bid << "\n";
    std::cout << "    reservation_ask: " << reservation_ask << "\n";

    Price our_bid = reservation_bid - target_spread_dollars / 2.0;
    Price our_ask = reservation_ask + target_spread_dollars / 2.0;
        
    double imbalance = book.getImbalance();
    double imbalance_adjustment = imbalance * 0.005;  // Max 0.5% adjustment
    
    our_bid += imbalance_adjustment;
    our_ask += imbalance_adjustment;
    
    std::cout << "    imbalance: " << imbalance << "  adjustment: " << imbalance_adjustment << "\n";
    
    our_bid = roundToCent(our_bid);
    our_ask = roundToCent(our_ask);
    // Clip to valid price range [0.01, 0.99] for binary markets
    our_bid = std::max(0.01, std::min(0.99, our_bid));
    our_ask = std::max(0.01, std::min(0.99, our_ask));
    

    if (our_ask <= our_bid) {
        std::cout << "Quotes collapsed after clipping (bid=" << our_bid << ", ask=" << our_ask << "), not quoting\n";
        return std::nullopt;
    }

    if (our_bid >= book.getBestAsk() || our_ask <= book.getBestBid()) {
        std::cout << "Our quotes would cross the market, not quoting\n";
        return std::nullopt;
    }
    
    // Size based on remaining capacity
    double remaining_capacity = max_position_ - std::abs(inventory_);
    Size quote_size = std::min(100.0, remaining_capacity / mid);

    if (quote_size < 10.0) {
        std::cout << "Near max position (remaining: $" << remaining_capacity << "), not quoting\n";
        return std::nullopt;
    }

    Quote quote{our_bid, quote_size, our_ask, quote_size};
    
    std::cout << "Generated quote: Bid " << our_bid << " x " << quote_size
              << " / Ask " << our_ask << " x " << quote_size 
              << " (inventory: " << inventory_ << ")\n";
    
    return quote;
}

void MarketMaker::updateInventory(Side side, Size filled_size, Price fill_price) {
    double old_inventory = inventory_;
    double old_inventory_dollars = inventory_dollars_;
    
    if (side == Side::BUY) {
        inventory_ += filled_size;
        inventory_dollars_ += filled_size * fill_price;
        
        if (inventory_ > 0) {
            avg_cost_ = inventory_dollars_ / inventory_;
        }
        
        std::cout << "  Bought " << filled_size << " @ " << fill_price << "\n";
        
    } else {
        double old_inventory_for_pnl = old_inventory;
        
        inventory_ -= filled_size;
        
        // Calculate P&L if closing long position
        if (old_inventory_for_pnl > 0) {
            double closing_size = std::min(filled_size, old_inventory_for_pnl);
            double pnl = closing_size * (fill_price - avg_cost_);
            realized_pnl_ += pnl;
            
            std::cout << "  Sold " << closing_size << " @ " << fill_price 
                      << " (closed long @ " << avg_cost_ << ", PnL: $" << pnl << ")\n";
            
            // If oversold (went short)
            if (filled_size > closing_size) {
                double opening_short = filled_size - closing_size;
                std::cout << "  Opened short: " << opening_short << " @ " << fill_price << "\n";
            }
        } else {
            std::cout << "  Sold " << filled_size << " @ " << fill_price 
                      << " (opening/adding to short)\n";
        }
        
        // Update inventory_dollars based on NEW position
        if (inventory_ > 0) {
            // Still long: value = remaining shares * avg cost
            inventory_dollars_ = inventory_ * avg_cost_;
        } else if (inventory_ < 0) {
            // Now short: value = short position * entry price
            inventory_dollars_ = inventory_ * fill_price;
            avg_cost_ = fill_price;  // Track short entry price
        } else {
            // Flat
            inventory_dollars_ = 0;
            avg_cost_ = 0;
        }
    }
    
    std::cout << "  Inventory: " << inventory_ << " shares ($" << inventory_dollars_ 
              << "), Realized P&L: $" << realized_pnl_ << "\n";
}

void MarketMaker::updateVolatility(Price old_mid, Price new_mid, double time_elapsed_seconds) {
    double return_pct = std::abs(new_mid - old_mid) / old_mid;
    
    // Annualize (assuming 252 trading days, 24 hours per day)
    double annual_factor = std::sqrt(252.0 * 24.0 * 3600.0 / time_elapsed_seconds);
    double observed_vol = return_pct * annual_factor;
    
    // EWMA with lambda = 0.94
    double lambda = 0.94;
    volatility_ = lambda * volatility_ + (1.0 - lambda) * observed_vol;
    
    // Clip to reasonable range
    volatility_ = std::max(0.01, std::min(0.50, volatility_));
    
    // Only log if significant change
    static double last_logged_vol = 0.0;
    if (std::abs(volatility_ - last_logged_vol) > 0.01) {
        std::cout << "  Volatility updated: " << volatility_ << "\n";
        last_logged_vol = volatility_;
    }
}

Price MarketMaker::roundToCent(Price price) {
    return std::round(price * 100.0) / 100.0;
}



} // namespace pmm