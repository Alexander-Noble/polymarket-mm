#include <gtest/gtest.h>
#include "utils/trading_logger.hpp"
#include "core/types.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace pmm;

class TradingLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = "./test_logs";
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
        logger = std::make_unique<TradingLogger>(test_dir);
    }
    
    void TearDown() override {
        logger.reset();  // Close files before cleanup
        if (std::filesystem::exists(test_dir)) {
            std::filesystem::remove_all(test_dir);
        }
    }
    
    std::string test_dir;
    std::unique_ptr<TradingLogger> logger;
    
    bool fileContainsString(const std::filesystem::path& file, const std::string& str) {
        if (!std::filesystem::exists(file)) return false;
        std::ifstream f(file);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find(str) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    
    int countLinesInFile(const std::filesystem::path& file) {
        if (!std::filesystem::exists(file)) return 0;
        std::ifstream f(file);
        int count = 0;
        std::string line;
        while (std::getline(f, line)) {
            count++;
        }
        return count;
    }
};

TEST_F(TradingLoggerTest, SessionCreatesDirectory) {
    logger->startSession("Test Event");
    
    EXPECT_TRUE(std::filesystem::exists(test_dir));
    
    std::string session_id = logger->getSessionId();
    EXPECT_FALSE(session_id.empty());
    EXPECT_TRUE(session_id.find("session_") != std::string::npos);
}

TEST_F(TradingLoggerTest, SessionCreatesLogFiles) {
    logger->startSession("Test Event");
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path session_dir = std::filesystem::path(test_dir) / session_id;
    
    EXPECT_TRUE(std::filesystem::exists(session_dir));
    EXPECT_TRUE(std::filesystem::exists(session_dir / "orders.csv"));
    EXPECT_TRUE(std::filesystem::exists(session_dir / "fills.csv"));
    EXPECT_TRUE(std::filesystem::exists(session_dir / "positions.csv"));
}

TEST_F(TradingLoggerTest, LogOrderPlaced) {
    logger->startSession("Test Event");
    
    Order order;
    order.order_id = "ORDER_123";
    order.token_id = "TOKEN_XYZ";
    order.side = Side::BUY;
    order.price = 0.55;
    order.size = 100.0;
    
    logger->logOrderPlaced(order, "MARKET_001");
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path orders_file = std::filesystem::path(test_dir) / session_id / "orders.csv";
    
    EXPECT_TRUE(fileContainsString(orders_file, "ORDER_123"));
    EXPECT_TRUE(fileContainsString(orders_file, "TOKEN_XYZ"));
    EXPECT_TRUE(fileContainsString(orders_file, "BUY"));
    EXPECT_TRUE(fileContainsString(orders_file, "0.55"));
    EXPECT_TRUE(fileContainsString(orders_file, "100"));
    EXPECT_TRUE(fileContainsString(orders_file, "OPEN"));
}

TEST_F(TradingLoggerTest, LogOrderCancelled) {
    logger->startSession("Test Event");
    
    Order order;
    order.order_id = "ORDER_456";
    order.token_id = "TOKEN_ABC";
    order.side = Side::SELL;
    order.price = 0.45;
    order.size = 200.0;
    
    logger->logOrderCancelled("ORDER_456", order, "MARKET_002");
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path orders_file = std::filesystem::path(test_dir) / session_id / "orders.csv";
    
    EXPECT_TRUE(fileContainsString(orders_file, "ORDER_456"));
    EXPECT_TRUE(fileContainsString(orders_file, "CANCELLED"));
}

TEST_F(TradingLoggerTest, LogOrderFilled) {
    logger->startSession("Test Event");
    
    logger->logOrderFilled("MARKET_003", "ORDER_789", "TOKEN_DEF", 0.60, 150.0, Side::BUY, 25.50);
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path fills_file = std::filesystem::path(test_dir) / session_id / "fills.csv";
    
    EXPECT_TRUE(fileContainsString(fills_file, "ORDER_789"));
    EXPECT_TRUE(fileContainsString(fills_file, "TOKEN_DEF"));
    EXPECT_TRUE(fileContainsString(fills_file, "BUY"));
    EXPECT_TRUE(fileContainsString(fills_file, "0.6"));
    EXPECT_TRUE(fileContainsString(fills_file, "150"));
    EXPECT_TRUE(fileContainsString(fills_file, "25.5"));
}

TEST_F(TradingLoggerTest, LogPosition) {
    logger->startSession("Test Event");
    
    logger->logPosition("MARKET_004", "TOKEN_GHI", 500.0, 0.52, 260.0, 10.0);
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path positions_file = std::filesystem::path(test_dir) / session_id / "positions.csv";
    
    EXPECT_TRUE(fileContainsString(positions_file, "TOKEN_GHI"));
    EXPECT_TRUE(fileContainsString(positions_file, "500"));
    EXPECT_TRUE(fileContainsString(positions_file, "0.52"));
    EXPECT_TRUE(fileContainsString(positions_file, "260"));
    EXPECT_TRUE(fileContainsString(positions_file, "10"));
}

TEST_F(TradingLoggerTest, MultipleOrders) {
    logger->startSession("Test Event");
    
    Order order1;
    order1.order_id = "ORDER_1";
    order1.token_id = "TOKEN_1";
    order1.side = Side::BUY;
    order1.price = 0.50;
    order1.size = 100.0;
    
    Order order2;
    order2.order_id = "ORDER_2";
    order2.token_id = "TOKEN_2";
    order2.side = Side::SELL;
    order2.price = 0.60;
    order2.size = 200.0;
    
    logger->logOrderPlaced(order1, "MARKET_A");
    logger->logOrderPlaced(order2, "MARKET_B");
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path orders_file = std::filesystem::path(test_dir) / session_id / "orders.csv";
    
    // Should have header + 2 orders = 3 lines
    EXPECT_EQ(countLinesInFile(orders_file), 3);
}

TEST_F(TradingLoggerTest, EndSessionClosesFiles) {
    logger->startSession("Test Event");
    
    Order order;
    order.order_id = "ORDER_END";
    order.token_id = "TOKEN_END";
    order.side = Side::BUY;
    order.price = 0.55;
    order.size = 50.0;
    
    logger->logOrderPlaced(order, "MARKET_END");
    logger->endSession();
    
    // After end, files should be closed but data should still be there
    std::string session_id = logger->getSessionId();
    std::filesystem::path orders_file = std::filesystem::path(test_dir) / session_id / "orders.csv";
    
    EXPECT_TRUE(std::filesystem::exists(orders_file));
    EXPECT_TRUE(fileContainsString(orders_file, "ORDER_END"));
}

TEST_F(TradingLoggerTest, NoSessionNoLogging) {
    // Don't start session
    Order order;
    order.order_id = "ORDER_NOSESSION";
    order.token_id = "TOKEN_X";
    order.side = Side::BUY;
    order.price = 0.50;
    order.size = 100.0;
    
    // Should not crash, just silently not log
    logger->logOrderPlaced(order, "MARKET_X");
    
    // No session directory should exist
    EXPECT_TRUE(std::filesystem::is_empty(test_dir) || 
                !std::filesystem::exists(test_dir));
}

TEST_F(TradingLoggerTest, MultipleSessionsCreateSeparateDirectories) {
    logger->startSession("Event 1");
    std::string session1_id = logger->getSessionId();
    logger->endSession();
    
    // Small delay to ensure different timestamp
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    
    logger->startSession("Event 2");
    std::string session2_id = logger->getSessionId();
    
    EXPECT_NE(session1_id, session2_id);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(test_dir) / session1_id));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(test_dir) / session2_id));
}

TEST_F(TradingLoggerTest, CSVHeadersAreCorrect) {
    logger->startSession("Test Event");
    
    std::string session_id = logger->getSessionId();
    std::filesystem::path session_dir = std::filesystem::path(test_dir) / session_id;
    
    // End session to ensure files are flushed and closed
    logger->endSession();
    
    std::filesystem::path orders_file = session_dir / "orders.csv";
    std::filesystem::path fills_file = session_dir / "fills.csv";
    std::filesystem::path positions_file = session_dir / "positions.csv";
    
    EXPECT_TRUE(fileContainsString(orders_file, "timestamp,market_id,order_id,token_id,side,price,size,status"));
    EXPECT_TRUE(fileContainsString(fills_file, "timestamp,market_id,order_id,token_id,side,fill_price,fill_size,pnl"));
    EXPECT_TRUE(fileContainsString(positions_file, "timestamp,market_id,token_id,position,avg_cost,market_value,unrealized_pnl"));
}
