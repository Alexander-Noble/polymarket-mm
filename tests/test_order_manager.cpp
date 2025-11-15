#include <gtest/gtest.h>
#include "strategy/order_manager.hpp"
#include "core/event_queue.hpp"
#include "core/types.hpp"

using namespace pmm;

class OrderManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue = std::make_unique<EventQueue>();
        om = std::make_unique<OrderManager>(*queue, TradingMode::PAPER);
    }
    
    std::unique_ptr<EventQueue> queue;
    std::unique_ptr<OrderManager> om;
};

TEST_F(OrderManagerTest, PlaceOrder) {
    Order order;
    order.token_id = "test_token";
    order.side = Side::BUY;
    order.price = 0.50;
    order.size = 100;
    
    std::string order_id = om->placeOrder(order.token_id, order.side, order.price, order.size, "test_market");
    
    EXPECT_FALSE(order_id.empty());
}

TEST_F(OrderManagerTest, CancelOrder) {
    Order order;
    order.token_id = "test_token";
    order.side = Side::BUY;
    order.price = 0.50;
    order.size = 100;
    
    std::string order_id = om->placeOrder(order.token_id, order.side, order.price, order.size, "test_market");
    EXPECT_FALSE(order_id.empty());
    
    bool cancelled = om->cancelOrder(order_id, "test_market");
    EXPECT_TRUE(cancelled);
}

TEST_F(OrderManagerTest, GetActiveOrders) {
    Order order1;
    order1.token_id = "test_token";
    order1.side = Side::BUY;
    order1.price = 0.50;
    order1.size = 100;
    
    Order order2;
    order2.token_id = "test_token";
    order2.side = Side::SELL;
    order2.price = 0.52;
    order2.size = 100;
    
    om->placeOrder(order1.token_id, order1.side, order1.price, order1.size, "test_market");
    om->placeOrder(order2.token_id, order2.side, order2.price, order2.size, "test_market");
    
    auto active = om->getOpenOrders("test_token");
    EXPECT_GE(active.size(), 2);
}

TEST_F(OrderManagerTest, CancelAllOrders) {
    Order order1;
    order1.token_id = "test_token";
    order1.side = Side::BUY;
    order1.price = 0.50;
    order1.size = 100;
    
    Order order2;
    order2.token_id = "test_token";
    order2.side = Side::SELL;
    order2.price = 0.52;
    order2.size = 100;
    
    om->placeOrder(order1.token_id, order1.side, order1.price, order1.size, "test_market");
    om->placeOrder(order2.token_id, order2.side, order2.price, order2.size, "test_market");
    
    bool cancelled_all = om->cancelAllOrders("test_token", "test_market");
    EXPECT_TRUE(cancelled_all);

    auto active = om->getOpenOrders("test_token");
    EXPECT_EQ(active.size(), 0);
}