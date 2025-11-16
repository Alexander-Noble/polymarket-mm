#pragma once

#include "core/types.hpp"
#include <deque>
#include <chrono>
#include <unordered_map>
#include <cmath>

namespace pmm {

// Tracks fill quality for toxic flow detection
struct FillQualityMetrics {
    TokenId token_id;
    OrderId order_id;
    Side side;
    Price fill_price;
    Price mid_at_fill;
    std::chrono::steady_clock::time_point fill_time;
    double inventory_before;
    double inventory_after;
    
    // Measured outcomes
    double price_move_5s = 0.0;
    double price_move_30s = 0.0;
    bool is_toxic = false;
    bool metrics_captured = false;
};

// Volume-based time tracking
struct VolumeClockTracker {
    std::deque<std::chrono::steady_clock::time_point> recent_fills;
    std::chrono::seconds window = std::chrono::seconds(60);
    
    void recordFill() {
        auto now = std::chrono::steady_clock::now();
        recent_fills.push_back(now);
        
        // Remove old fills outside window
        auto cutoff = now - window;
        while (!recent_fills.empty() && recent_fills.front() < cutoff) {
            recent_fills.pop_front();
        }
    }
    
    double getFillRate() const {
        if (recent_fills.empty()) return 0.0;
        return static_cast<double>(recent_fills.size()) / window.count();
    }
    
    double getVolumeClockMultiplier(double baseline_rate = 0.05) const {
        // baseline_rate = expected fills per second in normal conditions
        double current_rate = getFillRate();
        if (current_rate < baseline_rate * 0.1) return 0.8; // Very quiet, lower risk
        
        // More volume = more information = higher risk
        // Use sqrt to dampen the effect
        return std::sqrt(current_rate / baseline_rate);
    }
};

class AdverseSelectionManager {
public:
    AdverseSelectionManager(double base_spread = 0.02);
    
    // Called when we get filled
    void recordFill(const TokenId& token_id, const OrderId& order_id, Side side, 
                   Price fill_price, Price mid_at_fill, double inventory_before);
    
    // Update with current market state (called periodically)
    void updateMetrics(const TokenId& token_id, Price current_mid);
    
    // Get spread adjustment multiplier for a market
    double getSpreadMultiplier(const TokenId& token_id, Side side, double inventory) const;
    
    // Get detailed AS scores for monitoring
    struct AdverseSelectionScores {
        double toxic_flow_score;      // Based on recent fill quality
        double inventory_risk_score;  // Based on position
        double volume_clock_score;    // Based on fill rate
        double total_multiplier;      // Combined adjustment
    };
    
    AdverseSelectionScores getScores(const TokenId& token_id, Side side, double inventory) const;
    
    // Reset/decay parameters
    void decay();  // Called periodically to reduce adjustments over time
    
private:
    double base_spread_;
    
    // Per-token tracking
    std::unordered_map<TokenId, std::deque<FillQualityMetrics>> fill_history_;
    std::unordered_map<TokenId, VolumeClockTracker> volume_clocks_;
    std::unordered_map<TokenId, double> spread_multipliers_;  // Per-token spread adjustment
    
    // Analyze recent fill quality
    double calculateToxicFlowScore(const TokenId& token_id) const;
    
    // Inventory-based risk assessment
    double calculateInventoryRiskScore(Side side, double inventory, double max_position = 1000.0) const;
    
    // Parameters
    static constexpr size_t MAX_FILL_HISTORY = 50;
    static constexpr double TOXIC_THRESHOLD = -0.005;  // Price moved against us by 0.5%
    static constexpr double DECAY_RATE = 0.95;  // Multiplier decay per period
    static constexpr double MIN_MULTIPLIER = 1.0;
    static constexpr double MAX_MULTIPLIER = 3.0;
};

} // namespace pmm
