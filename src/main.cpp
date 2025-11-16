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
#include <sstream>
#include <map>
#include <set>
#include <unistd.h>  // for isatty()

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
    std::cout << "  1. Paper Trading\n";
    std::cout << "  2. Live Trading\n";
    std::cout << "Choice: ";

    int mode_choice;
    std::cin >> mode_choice;
    std::cin.ignore();
    
    TradingMode mode = (mode_choice == 2) ? TradingMode::LIVE : TradingMode::PAPER;
    
    if (mode == TradingMode::LIVE) {
        std::cout << "WARNING: LIVE TRADING MODE\n";
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
                  << ", Markets: " << events[i].markets.size() << "\n";
    }
    
    std::cout << "\n=== STAGE 1: SELECT EVENTS ===\n";
    std::cout << "Select events to trade:\n";
    std::cout << "  - Enter event numbers (e.g., '0,2,5')\n";
    std::cout << "  - Enter 'all' to trade all events\n";
    std::cout << "  - Enter 'top N' to select top N by volume (e.g., 'top 3')\n";
    std::cout << "Selection: ";
    
    std::string event_selection;
    std::getline(std::cin, event_selection);
    
    std::vector<size_t> selected_event_indices;
    
    if (event_selection == "all") {
        for (size_t i = 0; i < events.size(); i++) {
            selected_event_indices.push_back(i);
        }
    } else if (event_selection.find("top") == 0) {
        // Extract number after "top"
        std::stringstream ss(event_selection.substr(3));
        int top_n;
        ss >> top_n;
        
        // Events are already sorted by volume from API
        for (size_t i = 0; i < std::min(static_cast<size_t>(top_n), events.size()); i++) {
            selected_event_indices.push_back(i);
        }
        
        LOG_INFO("Auto-selected top {} events by volume", selected_event_indices.size());
    } else {
        // Parse comma-separated numbers
        std::stringstream ss(event_selection);
        std::string token;
        while (std::getline(ss, token, ',')) {
            token.erase(0, token.find_first_not_of(" \t\n\r"));
            token.erase(token.find_last_not_of(" \t\n\r") + 1);
            
            try {
                size_t idx = std::stoul(token);
                if (idx >= events.size()) {
                    LOG_ERROR("Invalid event index: {}\n", idx);
                    return 1;
                }
                selected_event_indices.push_back(idx);
            } catch (...) {
                LOG_ERROR("Invalid input: {}\n", token);
                return 1;
            }
        }
    }
    
    if (selected_event_indices.empty()) {
        LOG_ERROR("No events selected\n");
        return 1;
    }
    
    LOG_INFO("Selected {} event(s)", selected_event_indices.size());
    
    // Stage 2: Select markets for each event
    std::cout << "\n=== STAGE 2: SELECT MARKETS ===\n";
    
    // Track which event.market combinations to include
    std::map<size_t, std::set<size_t>> selected_markets;
    
    for (size_t event_idx : selected_event_indices) {
        const EventInfo& event = events[event_idx];
        
        std::cout << "\n--- " << event.title << " ---\n";
        std::cout << "Markets (" << event.markets.size() << " total):\n";
        
        // Sort markets by volume for display
        std::vector<std::pair<size_t, const MarketInfo*>> sorted_markets;
        for (size_t i = 0; i < event.markets.size(); i++) {
            sorted_markets.push_back({i, &event.markets[i]});
        }
        std::sort(sorted_markets.begin(), sorted_markets.end(),
                 [](const auto& a, const auto& b) { return a.second->volume > b.second->volume; });
        
        for (const auto& [idx, market] : sorted_markets) {
            std::cout << "  [" << idx << "] " << market->question 
                     << " ($" << static_cast<int>(market->volume/1000) << "K vol, "
                     << "$" << static_cast<int>(market->liquidity/1000) << "K liq)\n";
        }
        
        std::cout << "\nMarket selection options:\n";
        std::cout << "  all      - Trade all markets\n";
        std::cout << "  top N    - Top N by volume (e.g., 'top 3')\n";
        std::cout << "  liquid N - Top N by liquidity (e.g., 'liquid 3')\n";
        std::cout << "  vol>N    - Markets with volume > $N (e.g., 'vol>50000')\n";
        std::cout << "  0,2,5    - Specific market numbers\n";
        std::cout << "  skip     - Skip this event\n";
        std::cout << "Selection [all]: ";
        
        std::string market_selection;
        std::getline(std::cin, market_selection);
        
        if (market_selection.empty()) {
            market_selection = "all";
        }
        
        // Trim whitespace
        market_selection.erase(0, market_selection.find_first_not_of(" \t\n\r"));
        market_selection.erase(market_selection.find_last_not_of(" \t\n\r") + 1);
        
        if (market_selection == "skip") {
            LOG_INFO("Skipping event: {}", event.title);
            continue;
        }
        
        std::set<size_t> markets_for_event;
        
        if (market_selection == "all") {
            // All markets
            for (size_t i = 0; i < event.markets.size(); i++) {
                markets_for_event.insert(i);
            }
        } else if (market_selection.find("top") == 0) {
            // Top N by volume
            std::stringstream ss(market_selection.substr(3));
            int top_n;
            ss >> top_n;
            
            for (size_t i = 0; i < std::min(static_cast<size_t>(top_n), sorted_markets.size()); i++) {
                markets_for_event.insert(sorted_markets[i].first);
            }
        } else if (market_selection.find("liquid") == 0) {
            // Top N by liquidity
            std::stringstream ss(market_selection.substr(6));
            int top_n;
            ss >> top_n;
            
            auto liq_sorted = sorted_markets;
            std::sort(liq_sorted.begin(), liq_sorted.end(),
                     [](const auto& a, const auto& b) { return a.second->liquidity > b.second->liquidity; });
            
            for (size_t i = 0; i < std::min(static_cast<size_t>(top_n), liq_sorted.size()); i++) {
                markets_for_event.insert(liq_sorted[i].first);
            }
        } else if (market_selection.find("vol>") == 0) {
            // Volume threshold
            double min_vol = std::stod(market_selection.substr(4));
            
            for (size_t i = 0; i < event.markets.size(); i++) {
                if (event.markets[i].volume >= min_vol) {
                    markets_for_event.insert(i);
                }
            }
        } else {
            // Parse comma-separated numbers
            std::stringstream ss(market_selection);
            std::string token;
            while (std::getline(ss, token, ',')) {
                token.erase(0, token.find_first_not_of(" \t\n\r"));
                token.erase(token.find_last_not_of(" \t\n\r") + 1);
                
                try {
                    size_t idx = std::stoul(token);
                    if (idx >= event.markets.size()) {
                        LOG_ERROR("Invalid market index: {}\n", idx);
                        return 1;
                    }
                    markets_for_event.insert(idx);
                } catch (...) {
                    LOG_ERROR("Invalid input: {}\n", token);
                    return 1;
                }
            }
        }
        
        if (!markets_for_event.empty()) {
            selected_markets[event_idx] = markets_for_event;
            LOG_INFO("Selected {} markets from: {}", markets_for_event.size(), event.title);
        }
    }
    
    if (selected_markets.empty()) {
        LOG_ERROR("No markets selected\n");
        return 1;
    }
    
    // Stage 3: Register selected markets
    std::cout << "\n=== SUMMARY ===\n";
    std::vector<TokenId> all_tokens;
    int total_markets = 0;
    
    for (const auto& [event_idx, market_indices] : selected_markets) {
        const EventInfo& event = events[event_idx];
        
        LOG_INFO("Event: {} ({} markets)", event.title, market_indices.size());
        
        for (size_t market_idx : market_indices) {
            const auto& market = event.markets[market_idx];
            LOG_DEBUG("  - {}", market.question);
            
            for (size_t i = 0; i < market.tokens.size(); i++) {
                strategy.registerMarket(
                    market.tokens[i],
                    market.question,
                    market.outcomes[i],
                    market.market_id
                );
                all_tokens.push_back(market.tokens[i]);
            }
            total_markets++;
        }
    }

    LOG_INFO("Total markets registered: {} ({} tokens total)", total_markets, all_tokens.size());
    
    strategy.start();
    
    std::string session_title = selected_markets.size() == 1 && selected_markets.begin()->second.size() <= 1
        ? events[selected_markets.begin()->first].title 
        : "Multi-Market Trading";
    strategy.startLogging(session_title);

    LOG_INFO("Connecting to Polymarket WebSocket...");
    PolymarketWebSocketClient ws_client(queue);
    ws_client.connect();
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    LOG_INFO("Subscribing to {} tokens...", all_tokens.size());
    ws_client.subscribe(all_tokens);
    
    LOG_INFO("PAPER TRADING ACTIVE");
    LOG_INFO("Events: {}", selected_markets.size());
    LOG_INFO("Total Markets: {}", total_markets);

    bool is_tty = isatty(fileno(stdout));
    
    std::thread status_thread([&, is_tty]() {
        int seconds = 0;
        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (keep_running) {
                seconds += 5;
                auto positions = strategy.getPositionCount();
                auto active_orders = strategy.getActiveOrderCount();
                auto bid_count = strategy.getBidCount();
                auto ask_count = strategy.getAskCount();
                auto active_markets = strategy.getActiveMarketCount();
                auto realized_pnl = strategy.getTotalPnL();
                auto unrealized_pnl = strategy.getUnrealizedPnL();
                auto total_pnl = realized_pnl + unrealized_pnl;
                auto total_inventory = strategy.getTotalInventory();
                auto avg_spread = strategy.getAverageSpread();
                auto fill_count = strategy.getFillCount();
                
                // Format runtime nicely
                int minutes = seconds / 60;
                int secs = seconds % 60;
                std::string runtime_str = (minutes > 0) 
                    ? std::to_string(minutes) + "m" + std::to_string(secs) + "s"
                    : std::to_string(secs) + "s";
                
                // Build status message with detailed order breakdown
                std::string orders_str = std::to_string(bid_count) + "b/" + std::to_string(ask_count) + "a";
                
                // Format PnL
                std::string pnl_str;
                if (std::abs(total_pnl) < 0.01) {
                    pnl_str = "$0.00";
                } else if (std::abs(unrealized_pnl) < 0.01) {
                    // Only realized PnL
                    char pnl_buf[32];
                    snprintf(pnl_buf, sizeof(pnl_buf), "$%.2f", realized_pnl);
                    pnl_str = pnl_buf;
                } else {
                    // Both realized and unrealized
                    char pnl_buf[64];
                    snprintf(pnl_buf, sizeof(pnl_buf), "$%.2f (R:%.2f/U:%.2f)", total_pnl, realized_pnl, unrealized_pnl);
                    pnl_str = pnl_buf;
                }
                
                // Format spread in basis points
                double avg_spread_bps = (avg_spread > 0) ? (avg_spread * 10000) : 0.0;
                char spread_buf[16];
                snprintf(spread_buf, sizeof(spread_buf), "%.1fbps", avg_spread_bps);
                
                // Build enhanced status line
                if (is_tty) {
                    // Dashboard mode: overwrite previous line
                    // Move up 1 line, clear line, then print
                    std::string status_line = "\033[1A\033[2K\r[STATUS] " + runtime_str + 
                                  " | Mkts:" + std::to_string(active_markets) + "/" + std::to_string(total_markets) +
                                  " | Orders:" + orders_str +
                                  " | Fills:" + std::to_string(fill_count);
                    
                    if (positions > 0) {
                        status_line += " | Pos:" + std::to_string(positions) + "/" + 
                                      std::to_string(static_cast<int>(total_inventory)) + "u";
                    } else {
                        status_line += " | Pos:0";
                    }
                    
                    if (avg_spread_bps > 0) {
                        status_line += " | Spd:" + std::string(spread_buf);
                    }
                    
                    status_line += " | PnL:" + pnl_str;
                    
                    // Print to stdout directly (bypassing logger for dashboard)
                    std::cout << status_line << std::endl;
                    std::cout.flush();
                } else {
                    // Non-TTY mode: use regular logging
                    LOG_INFO("[STATUS] {} | Mkts:{}/{} | Orders:{} | Fills:{} | Pos:{} | Spd:{} | PnL:{}",
                             runtime_str,
                             active_markets,
                             total_markets,
                             orders_str,
                             fill_count,
                             positions,
                             spread_buf,
                             pnl_str);
                }
            }
        }
    });

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Wait for status thread to complete and print final newline for dashboard mode
    if (status_thread.joinable()) {
        status_thread.join();
    }
    
    LOG_INFO("Shutting down...");
    ws_client.disconnect();
    strategy.stop();
    
    std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("Logs saved to: ./logs/");
    LOG_INFO("Goodbye!");
    return 0;
}