#include "strategy/strategy_engine.hpp"
#include "core/event_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace pmm;

int main() {
    std::cout << "========================================\n";
    std::cout << "  PAPER TRADING TEST - AV MODEL\n";
    std::cout << "========================================\n\n";
    
    EventQueue queue;
    StrategyEngine strategy(queue, TradingMode::PAPER);
    
    std::string villa_token = "44623110248227182263524920709598432835467185438698898378400926229226251167932";
    
    strategy.registerMarket(villa_token, "Aston Villa vs Bournemouth", "Villa Win", "651006");
    strategy.start();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // =========================================================================
    std::cout << "\n=== ROUND 1: Initial book ===\n";
    
    std::vector<std::pair<Price, Size>> bids = {{0.41, 7000.0}, {0.40, 6000.0}};
    std::vector<std::pair<Price, Size>> asks = {{0.42, 1700.0}, {0.43, 3700.0}};
    
    queue.push(Event::bookSnapshot(villa_token, bids, asks));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // =========================================================================
    std::cout << "\n=== ROUND 2: Market moves up (our ask should fill) ===\n";
    
    std::vector<std::pair<Price, Size>> bids2 = {{0.43, 5000.0}, {0.42, 7000.0}};
    std::vector<std::pair<Price, Size>> asks2 = {{0.44, 3700.0}, {0.45, 4000.0}};
    
    queue.push(Event::bookSnapshot(villa_token, bids2, asks2));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // =========================================================================
    std::cout << "\n=== ROUND 3: Market moves down (our bid should fill) ===\n";
    
    std::vector<std::pair<Price, Size>> bids3 = {{0.40, 6000.0}, {0.39, 5000.0}};
    std::vector<std::pair<Price, Size>> asks3 = {{0.41, 2000.0}, {0.42, 3000.0}};
    
    queue.push(Event::bookSnapshot(villa_token, bids3, asks3));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // =========================================================================
    std::cout << "\n=== ROUND 4: One more cycle ===\n";
    
    queue.push(Event::bookSnapshot(villa_token, bids, asks));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // =========================================================================
    std::cout << "\n========================================\n";
    std::cout << "  TEST COMPLETE\n";
    std::cout << "========================================\n";
    
    strategy.stop();
    return 0;
}