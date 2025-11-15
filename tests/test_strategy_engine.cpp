#include <gtest/gtest.h>
#include "strategy/strategy_engine.hpp"
#include "core/event_queue.hpp"
#include <thread>
#include <chrono>

using namespace pmm;

class StrategyEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue = std::make_unique<EventQueue>();
        strategy = std::make_unique<StrategyEngine>(*queue, TradingMode::PAPER);
    }
    
    void TearDown() override {
        if (strategy) {
            strategy->stop();
        }
    }
    
    std::unique_ptr<EventQueue> queue;
    std::unique_ptr<StrategyEngine> strategy;
};

TEST_F(StrategyEngineTest, RegisterMarket) {
    std::string token = "test_token_123";
    strategy->registerMarket(token, "Test Event", "Test Market", "12345");
    
    SUCCEED();
}

TEST_F(StrategyEngineTest, StartAndStop) {
    strategy->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    strategy->stop();
    
    SUCCEED();
}

TEST_F(StrategyEngineTest, ProcessBookSnapshot) {
    std::string token = "test_token_123";
    strategy->registerMarket(token, "Test Event", "Test Market", "12345");
    strategy->start();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::vector<std::pair<Price, Size>> bids = {{0.50, 1000.0}, {0.49, 500.0}};
    std::vector<std::pair<Price, Size>> asks = {{0.51, 800.0}, {0.52, 1200.0}};
    
    queue->push(Event::bookSnapshot(token, bids, asks));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    SUCCEED();
}

TEST_F(StrategyEngineTest, PaperTradingSimulation) {
    std::string villa_token = "44623110248227182263524920709598432835467185438698898378400926229226251167932";
    
    strategy->registerMarket(villa_token, "Aston Villa vs Bournemouth", "Villa Win", "651006");
    strategy->start();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Round 1: Initial book
    std::vector<std::pair<Price, Size>> bids1 = {{0.41, 7000.0}, {0.40, 6000.0}};
    std::vector<std::pair<Price, Size>> asks1 = {{0.42, 1700.0}, {0.43, 3700.0}};
    queue->push(Event::bookSnapshot(villa_token, bids1, asks1));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Round 2: Market moves up
    std::vector<std::pair<Price, Size>> bids2 = {{0.43, 5000.0}, {0.42, 7000.0}};
    std::vector<std::pair<Price, Size>> asks2 = {{0.44, 3700.0}, {0.45, 4000.0}};
    queue->push(Event::bookSnapshot(villa_token, bids2, asks2));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Round 3: Market moves down
    std::vector<std::pair<Price, Size>> bids3 = {{0.40, 6000.0}, {0.39, 5000.0}};
    std::vector<std::pair<Price, Size>> asks3 = {{0.41, 2000.0}, {0.42, 3000.0}};
    queue->push(Event::bookSnapshot(villa_token, bids3, asks3));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    SUCCEED();
}