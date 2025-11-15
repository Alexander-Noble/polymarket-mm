#include <gtest/gtest.h>
#include "core/types.hpp"
#include "core/event_queue.hpp"
#include <thread>
#include <chrono>

using namespace pmm;

class EventQueueTest : public ::testing::Test {
protected:
    EventQueue queue;
};

TEST_F(EventQueueTest, PushAndPop) {
    auto event = Event::timerTick();
    queue.push(std::move(event));
    
    auto popped = queue.pop();
    EXPECT_EQ(popped.type, EventType::TIMER_TICK);
}

TEST_F(EventQueueTest, MultipleEvents) {
    for (int i = 0; i < 5; i++) {
        queue.push(Event::timerTick());
    }
    
    for (int i = 0; i < 5; i++) {
        auto event = queue.pop();
        EXPECT_EQ(event.type, EventType::TIMER_TICK);
    }
}

TEST_F(EventQueueTest, ProducerConsumerThreadSafety) {
    std::atomic<int> consumed{0};
    const int NUM_EVENTS = 100;
    
    std::thread producer([&]() {
        for (int i = 0; i < NUM_EVENTS; i++) {
            queue.push(Event::timerTick());
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        queue.push(Event::shutdown("Test complete"));
    });
    
    std::thread consumer([&]() {
        while (true) {
            auto event = queue.pop();
            if (event.type == EventType::SHUTDOWN) {
                break;
            }
            consumed++;
        }
    });
    
    producer.join();
    consumer.join();
    
    EXPECT_EQ(consumed, NUM_EVENTS);
}

TEST_F(EventQueueTest, BookSnapshotEvent) {
    std::vector<std::pair<Price, Size>> bids = {{0.50, 1000.0}, {0.49, 500.0}};
    std::vector<std::pair<Price, Size>> asks = {{0.51, 800.0}, {0.52, 1200.0}};
    
    queue.push(Event::bookSnapshot("test_token", bids, asks));
    
    auto event = queue.pop();
    EXPECT_EQ(event.type, EventType::BOOK_SNAPSHOT);
}

TEST_F(EventQueueTest, ShutdownEvent) {
    std::string reason = "Test shutdown";
    queue.push(Event::shutdown(reason));
    
    auto event = queue.pop();
    EXPECT_EQ(event.type, EventType::SHUTDOWN);
}