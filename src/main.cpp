#include "core/event_queue.hpp"
#include "strategy/strategy_engine.hpp"
#include "network/http_client.hpp"
#include "network/websocket_client.hpp"
#include "strategy/order_manager.hpp"
#include "utils/logger.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <iomanip>

using namespace pmm;

std::atomic<bool> keep_running{true};

void signalHandler(int signal) {
    std::cout << "\n\nReceived signal " << signal << ", shutting down...\n";
    keep_running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    Logger::init("./logs", "polymarket_mm");
    
    std::cout << "Trading mode:\n";
    std::cout << "  1. Paper Trading (simulated)\n";
    std::cout << "  2. Live Trading (real money!)\n";
    std::cout << "Choice: ";

    int mode_choice;
    std::cin >> mode_choice;
    std::cin.ignore();
    
    TradingMode mode = (mode_choice == 2) ? TradingMode::LIVE : TradingMode::PAPER;
    
    if (mode == TradingMode::LIVE) {
        std::cout << "\nWARNING: LIVE TRADING MODE - REAL MONEY AT RISK! ⚠️\n";
        std::cout << "Type 'YES' to confirm: ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "YES") {
            std::cout << "Live trading cancelled. Switching to paper mode.\n";
            mode = TradingMode::PAPER;
        }
    }

    EventQueue queue;
    StrategyEngine strategy(queue, mode);
    PolymarketHttpClient http_client;

    std::cout << "What would you like to trade?\n";
    std::cout << "  1. Search for specific event (e.g., 'epl')\n";
    std::cout << "  2. Browse top active events\n";
    std::cout << "Choice (1 or 2): ";
    
    int choice;
    std::cin >> choice;
    std::cin.ignore();
    
    std::vector<EventInfo> events;
    
    if (choice == 1) {
        std::cout << "Enter search query: ";
        std::string query;
        std::getline(std::cin, query);
        events = http_client.searchEvents(query);
    } else {
        events = http_client.getActiveEvents(10);
    }
    
    if (events.empty()) {
        LOG_ERROR("No events found! Exiting.\n");
        return 1;
    }

    std::cout << "\nAvailable events:\n";
    for (size_t i = 0; i < events.size(); i++) {
        std::cout << "  [" << i << "] " << events[i].title 
                  << "\n      Volume: $" << static_cast<int>(events[i].volume)
                  << ", Liquidity: $" << static_cast<int>(events[i].liquidity)
                  << "\n      Markets: " << events[i].markets.size() << "\n";
        
        for (const auto& market : events[i].markets) {
            std::cout << "        - " << market.question << "\n";
        }
        std::cout << "\n";
    }
    
    std::cout << "Select event to trade (0-" << (events.size()-1) << "): ";
    size_t selection;
    std::cin >> selection;
    
    if (selection >= events.size()) {
        LOG_ERROR("Invalid selection\n");
        return 1;
    }
    
    const EventInfo& selected_event = events[selection];
    
    LOG_INFO("\n>>> Registering {} markets for event: {}", selected_event.markets.size(), selected_event.title);
    
    std::vector<TokenId> all_tokens;
    for (const auto& market : selected_event.markets) {
        for (size_t i = 0; i < market.tokens.size(); i++) {
            strategy.registerMarket(
                market.tokens[i],
                market.question,
                market.outcomes[i],
                market.market_id
            );
            all_tokens.push_back(market.tokens[i]);
        }
    }

    LOG_INFO("Markets registered: {} tokens total.", all_tokens.size());
    
    strategy.start();
    strategy.startLogging(selected_event.title);

    LOG_INFO("\n>>> Connecting to Polymarket WebSocket...\n");
    PolymarketWebSocketClient ws_client(queue);
    ws_client.connect();
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO(">>> Subscribing to {} tokens...", all_tokens.size());
    ws_client.subscribe(all_tokens);
    
    LOG_INFO("  PAPER TRADING ACTIVE");
    LOG_INFO("  Event: {}", selected_event.title);
    LOG_INFO("  Markets: {}", selected_event.markets.size());

    std::thread status_thread([&]() {
        int seconds = 0;
        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (keep_running) {
                seconds += 5;
                auto positions = strategy.getPositionCount();
                auto active_orders = strategy.getActiveOrderCount();
                auto pnl = strategy.getTotalPnL();
                auto unrealized_pnl = strategy.getUnrealizedPnL();
                
                LOG_INFO("\n[STATUS] Runtime: {}s | Queue: {} | Markets: {} | Positions: {} | Orders: {} | PnL: ${:.2f}",
                         seconds,
                         queue.size(),
                         all_tokens.size(),
                         positions,
                         active_orders,
                         pnl,
                         unrealized_pnl);
        
            }
        }
    });

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOG_INFO("Shutting down...");
    ws_client.disconnect();
    strategy.stop();
    
    std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("Logs saved to: ./logs/");
    LOG_INFO("Goodbye!");
    return 0;
}