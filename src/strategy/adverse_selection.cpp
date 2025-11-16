#include "strategy/adverse_selection.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <algorithm>

namespace pmm {

AdverseSelectionManager::AdverseSelectionManager(double base_spread)
    : base_spread_(base_spread) {
    LOG_DEBUG("AdverseSelectionManager initialized with base spread: {:.2f}%", base_spread * 100);
}

void AdverseSelectionManager::recordFill(const TokenId& token_id, const OrderId& order_id, 
                                        Side side, Price fill_price, Price mid_at_fill, 
                                        double inventory_before) {
    FillQualityMetrics metrics;
    metrics.token_id = token_id;
    metrics.order_id = order_id;
    metrics.side = side;
    metrics.fill_price = fill_price;
    metrics.mid_at_fill = mid_at_fill;
    metrics.fill_time = std::chrono::steady_clock::now();
    metrics.inventory_before = inventory_before;
    
    fill_history_[token_id].push_back(metrics);
    
    // Limit history size
    if (fill_history_[token_id].size() > MAX_FILL_HISTORY) {
        fill_history_[token_id].pop_front();
    }
    
    // Update volume clock
    volume_clocks_[token_id].recordFill();
    
    LOG_DEBUG("Recorded fill for AS tracking: {} {} @ {}", 
             side == Side::BUY ? "BUY" : "SELL", fill_price, token_id);
}

void AdverseSelectionManager::updateMetrics(const TokenId& token_id, Price current_mid) {
    auto it = fill_history_.find(token_id);
    if (it == fill_history_.end()) return;
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& metrics : it->second) {
        if (metrics.metrics_captured) continue;
        
        auto time_since_fill = std::chrono::duration_cast<std::chrono::seconds>(
            now - metrics.fill_time
        ).count();
        
        // Capture at 5 seconds
        if (time_since_fill >= 5 && metrics.price_move_5s == 0.0) {
            double price_change = (current_mid - metrics.mid_at_fill) / metrics.mid_at_fill;
            
            // Adverse selection: price moved against our position
            if (metrics.side == Side::BUY) {
                metrics.price_move_5s = price_change;  // Negative = toxic
            } else {
                metrics.price_move_5s = -price_change; // Negative = toxic
            }
        }
        
        // Capture at 30 seconds and mark complete
        if (time_since_fill >= 30 && !metrics.metrics_captured) {
            double price_change = (current_mid - metrics.mid_at_fill) / metrics.mid_at_fill;
            
            if (metrics.side == Side::BUY) {
                metrics.price_move_30s = price_change;
            } else {
                metrics.price_move_30s = -price_change;
            }
            
            // Mark as toxic if significant adverse move
            metrics.is_toxic = (metrics.price_move_30s < TOXIC_THRESHOLD);
            metrics.metrics_captured = true;
            
            if (metrics.is_toxic) {
                // Increase spread multiplier for this token
                double& multiplier = spread_multipliers_[token_id];
                multiplier = std::min(MAX_MULTIPLIER, multiplier * 1.2 + 0.1);
                
                LOG_WARN("TOXIC FILL DETECTED: {} | {} @ {} | Price moved {:.2f}% against us | Spread multiplier: {:.2f}x",
                         token_id, metrics.side == Side::BUY ? "BUY" : "SELL", 
                         metrics.fill_price, metrics.price_move_30s * 100, multiplier);
            } else if (metrics.price_move_30s > 0.005) {
                // Good fill - gradually reduce multiplier
                double& multiplier = spread_multipliers_[token_id];
                multiplier = std::max(MIN_MULTIPLIER, multiplier * 0.95);
                
                LOG_DEBUG("Favorable fill: Price moved {:.2f}% in our favor", 
                         metrics.price_move_30s * 100);
            }
        }
    }
}

double AdverseSelectionManager::calculateToxicFlowScore(const TokenId& token_id) const {
    auto it = fill_history_.find(token_id);
    if (it == fill_history_.end() || it->second.empty()) {
        return 1.0;  // No data = baseline
    }
    
    // Look at recent completed fills
    int toxic_count = 0;
    int total_count = 0;
    double avg_adverse_move = 0.0;
    
    for (const auto& metrics : it->second) {
        if (metrics.metrics_captured) {
            total_count++;
            if (metrics.is_toxic) {
                toxic_count++;
            }
            avg_adverse_move += std::min(0.0, metrics.price_move_30s);
        }
    }
    
    if (total_count == 0) return 1.0;
    
    double toxic_rate = static_cast<double>(toxic_count) / total_count;
    
    // High toxic rate = higher spread needed
    // 0% toxic = 1.0x, 50% toxic = 1.5x, 100% toxic = 2.0x
    double toxic_score = 1.0 + toxic_rate;
    
    // Also consider magnitude of adverse moves
    double magnitude_score = 1.0 - (avg_adverse_move / total_count) * 10.0;  // Scale up the impact
    magnitude_score = std::max(1.0, std::min(2.0, magnitude_score));
    
    return std::max(toxic_score, magnitude_score);
}

double AdverseSelectionManager::calculateInventoryRiskScore(Side side, double inventory, 
                                                            double max_position) const {
    // Normalize inventory to [-1, 1]
    double normalized_inv = inventory / max_position;
    
    // When we have inventory, getting hit on the "unwinding" side is more toxic
    // Long position (inventory > 0):
    //   - Getting hit on ASK (selling to us) is MORE toxic (adds to position)
    //   - Getting hit on BID (buying from us) is LESS toxic (reduces position)
    // Short position (inventory < 0):
    //   - Getting hit on BID (buying from us) is MORE toxic (adds to short)
    //   - Getting hit on ASK (selling to us) is LESS toxic (reduces short)
    
    double inventory_risk = 1.0;
    
    if (inventory > 0 && side == Side::SELL) {
        // Long, getting hit on ask - more risky
        inventory_risk = 1.0 + std::abs(normalized_inv) * 0.5;
    } else if (inventory < 0 && side == Side::BUY) {
        // Short, getting hit on bid - more risky  
        inventory_risk = 1.0 + std::abs(normalized_inv) * 0.5;
    } else if (inventory > 0 && side == Side::BUY) {
        // Long, getting hit on bid - less risky (helping us unwind)
        inventory_risk = 1.0 - std::abs(normalized_inv) * 0.2;
    } else if (inventory < 0 && side == Side::SELL) {
        // Short, getting hit on ask - less risky (helping us unwind)
        inventory_risk = 1.0 - std::abs(normalized_inv) * 0.2;
    }
    
    return std::max(0.8, std::min(1.5, inventory_risk));
}

double AdverseSelectionManager::getSpreadMultiplier(const TokenId& token_id, Side side, 
                                                     double inventory) const {
    // 1. Base multiplier from toxic flow history
    double base_multiplier = 1.0;
    auto mult_it = spread_multipliers_.find(token_id);
    if (mult_it != spread_multipliers_.end()) {
        base_multiplier = mult_it->second;
    }
    
    // 2. Toxic flow score from recent fills
    double toxic_score = calculateToxicFlowScore(token_id);
    
    // 3. Inventory-based risk
    double inventory_score = calculateInventoryRiskScore(side, inventory);
    
    // 4. Volume clock adjustment
    double volume_score = 1.0;
    auto vol_it = volume_clocks_.find(token_id);
    if (vol_it != volume_clocks_.end()) {
        volume_score = vol_it->second.getVolumeClockMultiplier();
    }
    
    // Combine factors
    // Use product for base and toxic (they compound)
    // Add inventory adjustment
    // Multiply by volume clock
    double total_multiplier = base_multiplier * toxic_score * inventory_score * volume_score;
    
    // Clamp to reasonable range
    return std::max(MIN_MULTIPLIER, std::min(MAX_MULTIPLIER, total_multiplier));
}

AdverseSelectionManager::AdverseSelectionScores 
AdverseSelectionManager::getScores(const TokenId& token_id, Side side, double inventory) const {
    AdverseSelectionScores scores;
    
    scores.toxic_flow_score = calculateToxicFlowScore(token_id);
    scores.inventory_risk_score = calculateInventoryRiskScore(side, inventory);
    
    auto vol_it = volume_clocks_.find(token_id);
    scores.volume_clock_score = vol_it != volume_clocks_.end() 
        ? vol_it->second.getVolumeClockMultiplier() 
        : 1.0;
    
    scores.total_multiplier = getSpreadMultiplier(token_id, side, inventory);
    
    return scores;
}

void AdverseSelectionManager::decay() {
    // Gradually reduce spread multipliers back toward 1.0
    for (auto& [token_id, multiplier] : spread_multipliers_) {
        if (multiplier > MIN_MULTIPLIER) {
            multiplier = std::max(MIN_MULTIPLIER, 
                                 MIN_MULTIPLIER + (multiplier - MIN_MULTIPLIER) * DECAY_RATE);
            
            LOG_DEBUG("Decayed spread multiplier for {}: {:.2f}x", token_id, multiplier);
        }
    }
}

} // namespace pmm
