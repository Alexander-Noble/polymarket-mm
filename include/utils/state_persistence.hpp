#pragma once

#include "core/types.hpp"
#include <filesystem>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace pmm {

struct PositionState {
    double quantity = 0.0;
    double avg_cost = 0.0;
    double realized_pnl = 0.0;
};

struct TradingState {
    std::unordered_map<TokenId, PositionState> positions;
    double total_realized_pnl = 0.0;
    int total_trades = 0;
    double total_volume = 0.0;
    std::string last_session_id;
    std::chrono::system_clock::time_point last_updated;
};

class StatePersistence {
public:
    explicit StatePersistence(const std::filesystem::path& state_file = "./state.json");
    
    void saveState(const TradingState& state);
    TradingState loadState() const;

    void updatePosition(const TokenId& token_id, const PositionState& position);
    
    void updateGlobalStats(int total_trades, double total_volume, double total_realized_pnl);
    
private:
    std::filesystem::path state_file_;
    mutable std::mutex mutex_;
    TradingState current_state_;
    
    void ensureStateDir();
};

} // namespace pmm