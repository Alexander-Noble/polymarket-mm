#include "strategy/market_maker.hpp"
#include "utils/logger.hpp"
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
    
    LOG_INFO("MarketMaker initialized: spread={}, max_pos={}, gamma={}, sigma={}", spread_pct, max_position, risk_aversion_, volatility_);
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
        LOG_INFO("Market spread too tight ({}), not quoting", market_spread);
        return std::nullopt;
    }
    
    double target_spread_dollars = mid * spread_pct_;

    double q = inventory_ / 100.0; // Normalize inventory
    double gamma = risk_aversion_;
    double sigma_sq = volatility_ * volatility_; // Assume some volatility estimate
    
    double reservation_bid = mid - (q + 1.0) * gamma * sigma_sq;
    double reservation_ask = mid + (q - 1.0) * gamma * sigma_sq;

    LOG_DEBUG("  AV calculation:");
    LOG_DEBUG("    q (normalized inventory): {}", q);
    LOG_DEBUG("    gamma: {}, sigma: {}", gamma, volatility_);
    LOG_DEBUG("    reservation_bid: {}", reservation_bid);
    LOG_DEBUG("    reservation_ask: {}", reservation_ask);
    LOG_DEBUG("    target_spread: {}", target_spread_dollars);
    Price our_bid = reservation_bid - target_spread_dollars / 2.0;
    Price our_ask = reservation_ask + target_spread_dollars / 2.0;
        
    double imbalance = book.getImbalance();
    double imbalance_adjustment = imbalance * 0.005;  // Max 0.5% adjustment
    
    our_bid += imbalance_adjustment;
    our_ask += imbalance_adjustment;
    
    LOG_DEBUG("    imbalance: {}  adjustment: {}", imbalance, imbalance_adjustment);
    
    our_bid = roundToCent(our_bid);
    our_ask = roundToCent(our_ask);
    
    // Time-aware risk-adjusted cost floor
    if (inventory_ > 0 && avg_cost_ > 0) {
        double inventory_risk = std::abs(inventory_dollars_) / max_position_;
        double time_urgency = getTimeUrgency();
        
        // Base profit requirement: 1.5% when no urgency
        double base_min_profit = 0.015;
        
        // Reduce profit requirement based on time urgency and inventory risk
        double urgency_factor = std::max(time_urgency, inventory_risk);
        double min_profit_pct = base_min_profit * (1.0 - urgency_factor);
        
        // At very high urgency (>90%), accept small losses to exit
        if (urgency_factor > 0.9) {
            min_profit_pct = -0.01;  // Accept up to 1% loss
        }
        
        double min_ask = avg_cost_ * (1.0 + min_profit_pct);
        
        if (our_ask < min_ask) {
            LOG_DEBUG("    Adjusting ask from {} to {} (avg_cost: {}, urgency: {:.1f}%, inv_risk: {:.1f}%, min_profit: {:.2f}%)", 
                     our_ask, min_ask, avg_cost_, time_urgency * 100, inventory_risk * 100, min_profit_pct * 100);
            our_ask = min_ask;
        }
    }
    
    // Clip to valid price range [0.01, 0.99] for binary markets
    our_bid = std::max(0.01, std::min(0.99, our_bid));
    our_ask = std::max(0.01, std::min(0.99, our_ask));
    

    if (our_ask <= our_bid) {
        LOG_INFO("Quotes collapsed after clipping (bid={}, ask={}), not quoting", our_bid, our_ask);
        return std::nullopt;
    }

    if (our_bid >= book.getBestAsk() || our_ask <= book.getBestBid()) {
        LOG_INFO("Our quotes would cross the market, not quoting");
        return std::nullopt;
    }
    
    // Size based on remaining capacity
    double remaining_capacity = max_position_ - std::abs(inventory_);
    Size quote_size = std::min(100.0, remaining_capacity / mid);

    if (quote_size < 10.0) {
        LOG_INFO("Near max position (remaining: ${}), not quoting", remaining_capacity);
        return std::nullopt;
    }

    Quote quote{our_bid, quote_size, our_ask, quote_size};
    
    LOG_INFO("Generated quote: Bid {} x {} / Ask {} x {} (inventory: {})", our_bid, quote_size, our_ask, quote_size, inventory_);
    
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
        
        LOG_INFO("  Bought {} @ {}", filled_size, fill_price);
        
    } else {
        double old_inventory_for_pnl = old_inventory;
        
        inventory_ -= filled_size;
        
        // Calculate P&L if closing long position
        if (old_inventory_for_pnl > 0) {
            double closing_size = std::min(filled_size, old_inventory_for_pnl);
            double pnl = closing_size * (fill_price - avg_cost_);
            realized_pnl_ += pnl;
            
            LOG_INFO("  Sold {} @ {} (closed long @ {}, PnL: ${})", closing_size, fill_price, avg_cost_, pnl);
            
            // If oversold (went short)
            if (filled_size > closing_size) {
                double opening_short = filled_size - closing_size;
                LOG_INFO("  Opened short: {} @ {}", opening_short, fill_price);
            }
        } else {
            LOG_INFO("  Sold {} @ {} (opening/adding to short)", filled_size, fill_price);
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
    
    LOG_INFO("  Inventory: {} shares (${}), Realized P&L: ${}", inventory_, inventory_dollars_, realized_pnl_);
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
        LOG_DEBUG("  Volatility updated: {}", volatility_);
        last_logged_vol = volatility_;
    }
}

Price MarketMaker::roundToCent(Price price) {
    return std::round(price * 100.0) / 100.0;
}

void MarketMaker::setMarketCloseTime(std::chrono::system_clock::time_point close_time) {
    market_close_time_ = close_time;
    has_close_time_ = true;
    LOG_DEBUG("Market close time set");
}

double MarketMaker::getTimeUrgency() const {
    if (!has_close_time_) {
        return 0.0;  // No urgency if we don't know close time
    }
    
    auto now = std::chrono::system_clock::now();
    auto time_to_close = std::chrono::duration_cast<std::chrono::hours>(market_close_time_ - now);
    double hours_remaining = time_to_close.count();
    
    if (hours_remaining < 0) {
        return 1.0;  // Market closed or past close
    }
    
    // Urgency ramps up as we approach close
    // 0-24 hours: linear urgency increase
    // >24 hours: minimal urgency
    if (hours_remaining > 24.0) {
        return 0.0;
    }
    
    // Linear ramp: 24h = 0.0, 0h = 1.0
    return 1.0 - (hours_remaining / 24.0);
}

} // namespace pmm