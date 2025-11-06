#include "core/types.hpp"
#include "core/event_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace pmm;

int main() {
    EventQueue queue;
    
    // Producer thread
    std::thread producer([&queue]() {
        for (int i = 0; i < 5; i++) {
            auto event = Event::timerTick();
            std::cout << "Producer: Pushed event " << i << std::endl;
            queue.push(std::move(event));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Send shutdown
        queue.push(Event::shutdown("Test complete"));
    });
    
    // Consumer thread
    std::thread consumer([&queue]() {
        while (true) {
            auto event = queue.pop();
            
            if (event.type == EventType::SHUTDOWN) {
                std::cout << "Consumer: Received shutdown" << std::endl;
                break;
            }
            
            std::cout << "Consumer: Processed event" << std::endl;
        }
    });
    
    producer.join();
    consumer.join();
    
    std::cout << "Test passed!" << std::endl;
    return 0;
}