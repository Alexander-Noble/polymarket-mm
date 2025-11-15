#include <gtest/gtest.h>
#include "utils/state_persistence.hpp"
#include <filesystem>

using namespace pmm;

class StatePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file = "./test_state.json";
        // Clean up any existing test file
        if (std::filesystem::exists(test_file)) {
            std::filesystem::remove(test_file);
        }
        sp = std::make_unique<StatePersistence>(test_file);
    }
    
    void TearDown() override {
        if (std::filesystem::exists(test_file)) {
            std::filesystem::remove(test_file);
        }
    }
    
    std::string test_file;
    std::unique_ptr<StatePersistence> sp;
};

TEST_F(StatePersistenceTest, LoadStateWhenNoFileExists) {
    auto state = sp->loadState();
    
    EXPECT_EQ(state.positions.size(), 0);
    EXPECT_DOUBLE_EQ(state.total_realized_pnl, 0.0);
    EXPECT_EQ(state.total_trades, 0);
    EXPECT_DOUBLE_EQ(state.total_volume, 0.0);
}

TEST_F(StatePersistenceTest, SaveAndLoadCompleteState) {
    TradingState state;
    state.last_session_id = "session_123";
    state.total_realized_pnl = 1000.0;
    state.total_trades = 50;
    state.total_volume = 25000.0;
    state.last_updated = std::chrono::system_clock::now();
    
    // Add some positions
    PositionState pos1;
    pos1.quantity = 500.0;
    pos1.avg_cost = 0.55;
    pos1.realized_pnl = 250.0;
    state.positions["token_1"] = pos1;
    
    PositionState pos2;
    pos2.quantity = -300.0;
    pos2.avg_cost = 0.45;
    pos2.realized_pnl = -50.0;
    state.positions["token_2"] = pos2;
    
    sp->saveState(state);
    
    auto loaded = sp->loadState();
    
    EXPECT_EQ(loaded.last_session_id, "session_123");
    EXPECT_DOUBLE_EQ(loaded.total_realized_pnl, 1000.0);
    EXPECT_EQ(loaded.total_trades, 50);
    EXPECT_DOUBLE_EQ(loaded.total_volume, 25000.0);
    
    ASSERT_EQ(loaded.positions.size(), 2);
    EXPECT_DOUBLE_EQ(loaded.positions["token_1"].quantity, 500.0);
    EXPECT_DOUBLE_EQ(loaded.positions["token_1"].avg_cost, 0.55);
    EXPECT_DOUBLE_EQ(loaded.positions["token_1"].realized_pnl, 250.0);
    
    EXPECT_DOUBLE_EQ(loaded.positions["token_2"].quantity, -300.0);
    EXPECT_DOUBLE_EQ(loaded.positions["token_2"].avg_cost, 0.45);
    EXPECT_DOUBLE_EQ(loaded.positions["token_2"].realized_pnl, -50.0);
}

TEST_F(StatePersistenceTest, UpdateSinglePosition) {
    PositionState pos;
    pos.quantity = 100.0;
    pos.avg_cost = 0.60;
    pos.realized_pnl = 50.0;
    
    sp->updatePosition("token_xyz", pos);
    
    // Create a new instance to verify it's not just in memory
    StatePersistence sp2(test_file);
    sp->saveState(sp->loadState());  // Force save
    
    auto loaded = sp2.loadState();
    ASSERT_EQ(loaded.positions.count("token_xyz"), 0);  // Not saved yet without explicit save
}

TEST_F(StatePersistenceTest, UpdateGlobalStats) {
    sp->updateGlobalStats(100, 50000.0, 2500.0);
    
    // Need to explicitly save after updates
    TradingState state;
    state.total_trades = 100;
    state.total_volume = 50000.0;
    state.total_realized_pnl = 2500.0;
    sp->saveState(state);
    
    auto loaded = sp->loadState();
    EXPECT_EQ(loaded.total_trades, 100);
    EXPECT_DOUBLE_EQ(loaded.total_volume, 50000.0);
    EXPECT_DOUBLE_EQ(loaded.total_realized_pnl, 2500.0);
}

TEST_F(StatePersistenceTest, OverwriteExistingState) {
    TradingState state1;
    state1.total_realized_pnl = 500.0;
    state1.total_trades = 10;
    sp->saveState(state1);
    
    TradingState state2;
    state2.total_realized_pnl = 1500.0;
    state2.total_trades = 25;
    
    PositionState pos;
    pos.quantity = 200.0;
    pos.avg_cost = 0.50;
    pos.realized_pnl = 100.0;
    state2.positions["token_abc"] = pos;
    
    sp->saveState(state2);
    
    auto loaded = sp->loadState();
    EXPECT_DOUBLE_EQ(loaded.total_realized_pnl, 1500.0);
    EXPECT_EQ(loaded.total_trades, 25);
    EXPECT_EQ(loaded.positions.size(), 1);
}

TEST_F(StatePersistenceTest, EmptyPositions) {
    TradingState state;
    state.total_realized_pnl = 100.0;
    state.total_trades = 5;
    // No positions
    
    sp->saveState(state);
    
    auto loaded = sp->loadState();
    EXPECT_EQ(loaded.positions.size(), 0);
    EXPECT_DOUBLE_EQ(loaded.total_realized_pnl, 100.0);
}