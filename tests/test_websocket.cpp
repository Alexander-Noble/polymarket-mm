#include "network/websocket_client.hpp"
#include "core/event_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace pmm;

int main() {
    std::cout << "Starting WebSocket test...\n";
    
    EventQueue queue;
    
    PolymarketWebSocketClient ws_client(queue);
    
    // Subscribe to Villa vs Bournemouth market
    std::vector<std::string> assets = {"44623110248227182263524920709598432835467185438698898378400926229226251167932", "74655874091254935900453596134895619498881794779751550714671032631365783284563", "98088767910008625418382803186104668675749871451088740835243801395922131532380"};
    ws_client.subscribe(assets);
    
    ws_client.connect();

    std::cout << "Connected! Letting it run for 5 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    std::cout << "Disconnecting...\n";
    ws_client.disconnect();
    
    std::cout << "Test complete!\n";
    return 0;
}