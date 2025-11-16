#include "utils/state_persistence.hpp"
#include "utils/logger.hpp"
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace pmm {

StatePersistence::StatePersistence(const std::filesystem::path& state_file)
    : state_file_(state_file) {
    ensureStateDir();
    LOG_DEBUG("StatePersistence initialized - State file: {}", state_file_.string());
}

void StatePersistence::ensureStateDir() {
    auto parent_dir = state_file_.parent_path();
    if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
        std::filesystem::create_directories(parent_dir);
        LOG_DEBUG("Created state directory: {}", parent_dir.string());
    }
}

void StatePersistence::saveState(const TradingState& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    current_state_ = state;
    
    LOG_DEBUG("Saving state to {}", state_file_.string());
    LOG_DEBUG("  Positions to save: {}", state.positions.size());
    LOG_DEBUG("  Total trades: {}", state.total_trades);
    LOG_DEBUG("  Total realized PnL: ${:.2f}", state.total_realized_pnl);
    
    nlohmann::json j;
    j["last_session_id"] = state.last_session_id;
    j["last_updated"] = std::chrono::system_clock::to_time_t(state.last_updated);
    j["total_trades"] = state.total_trades;
    j["total_volume"] = state.total_volume;
    j["total_realized_pnl"] = state.total_realized_pnl;
    
    // Always create positions object (empty object if no positions)
    nlohmann::json positions_json = nlohmann::json::object();
    for (const auto& [token_id, pos] : state.positions) {
        LOG_DEBUG("  Saving position: {} | Qty: {:.2f} @ {:.3f} | PnL: ${:.2f}",
                  token_id, pos.quantity, pos.avg_cost, pos.realized_pnl);
        positions_json[token_id] = {
            {"quantity", pos.quantity},
            {"avg_cost", pos.avg_cost},
            {"realized_pnl", pos.realized_pnl}
        };
    }
    j["positions"] = positions_json;
    
    std::ofstream file(state_file_);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open state file for writing: {}", state_file_.string());
        return;
    }
    
    file << std::setw(2) << j << "\n";
    file.close();
    
    LOG_DEBUG("State saved successfully: {} positions, {} trades, ${:.2f} realized P&L",
              state.positions.size(), state.total_trades, state.total_realized_pnl);
}

TradingState StatePersistence::loadState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    TradingState state;
    
    LOG_DEBUG("Checking for state file: {}", state_file_.string());
    
    if (!std::filesystem::exists(state_file_)) {
        LOG_INFO("No previous state file found at: {}", state_file_.string());
        LOG_INFO("Starting with fresh state");
        return state;
    }
    
    LOG_INFO("Found state file: {}", state_file_.string());
    
    try {
        std::ifstream file(state_file_);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open state file: {}", state_file_.string());
            return state;
        }
        
        nlohmann::json j;
        file >> j;
        file.close();
        
        LOG_DEBUG("State file loaded, parsing JSON...");
        
        state.last_session_id = j.value("last_session_id", "");
        state.total_trades = j.value("total_trades", 0);
        state.total_volume = j.value("total_volume", 0.0);
        state.total_realized_pnl = j.value("total_realized_pnl", 0.0);
        
        LOG_DEBUG("  Session ID: {}", state.last_session_id);
        LOG_DEBUG("  Total trades: {}", state.total_trades);
        LOG_DEBUG("  Total volume: ${:.2f}", state.total_volume);
        LOG_DEBUG("  Total realized PnL: ${:.2f}", state.total_realized_pnl);
        
        if (j.contains("last_updated")) {
            auto timestamp = j["last_updated"].get<std::time_t>();
            state.last_updated = std::chrono::system_clock::from_time_t(timestamp);
            LOG_DEBUG("  Last updated: {}", timestamp);
        }
        
        if (j.contains("positions") && !j["positions"].is_null()) {
            LOG_DEBUG("  Processing positions...");
            
            if (j["positions"].is_object()) {
                for (auto& [token_id, pos_json] : j["positions"].items()) {
                    PositionState pos;
                    pos.quantity = pos_json.value("quantity", 0.0);
                    pos.avg_cost = pos_json.value("avg_cost", 0.0);
                    pos.realized_pnl = pos_json.value("realized_pnl", 0.0);
                    state.positions[token_id] = pos;
                    
                    LOG_DEBUG("    Loaded position: {} | Qty: {:.2f} @ {:.3f} | PnL: ${:.2f}",
                              token_id, pos.quantity, pos.avg_cost, pos.realized_pnl);
                }
            } else {
                LOG_WARN("  Positions field exists but is not an object (type: {})", j["positions"].type_name());
            }
        } else {
            LOG_DEBUG("  No positions in state file");
        }
        
        LOG_INFO("Successfully loaded previous state:");
        LOG_INFO("  Positions: {}", state.positions.size());
        LOG_INFO("  Total trades: {}", state.total_trades);
        LOG_INFO("  Total volume: ${:.2f}", state.total_volume);
        LOG_INFO("  Realized P&L: ${:.2f}", state.total_realized_pnl);
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error loading state from {}: {}", state_file_.string(), e.what());
        LOG_ERROR("Starting with fresh state");
    }
    
    return state;
}

void StatePersistence::updatePosition(const TokenId& token_id, const PositionState& position) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    current_state_.positions[token_id] = position;
    current_state_.last_updated = std::chrono::system_clock::now();
}

void StatePersistence::updateGlobalStats(int total_trades, double total_volume, double total_realized_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    current_state_.total_trades = total_trades;
    current_state_.total_volume = total_volume;
    current_state_.total_realized_pnl = total_realized_pnl;
    current_state_.last_updated = std::chrono::system_clock::now();
}

} // namespace pmm