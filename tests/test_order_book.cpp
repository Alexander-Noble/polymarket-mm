#include <gtest/gtest.h>
#include "data/order_book.hpp"
#include <cmath>

using namespace pmm;

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book{"test_token_123"};
    
    bool approxEqual(double a, double b, double epsilon = 1e-9) {
        return std::abs(a - b) < epsilon;
    }
};

TEST_F(OrderBookTest, InitiallyEmpty) {
    EXPECT_FALSE(book.hasValidBBO());
}

TEST_F(OrderBookTest, AddBidsAndAsks) {
    book.updateBid(0.50, 1000);
    book.updateBid(0.49, 500);
    book.updateAsk(0.51, 800);
    book.updateAsk(0.52, 1200);
    
    EXPECT_TRUE(book.hasValidBBO());
    EXPECT_TRUE(approxEqual(book.getBestBid(), 0.50));
    EXPECT_TRUE(approxEqual(book.getBestAsk(), 0.51));
}

TEST_F(OrderBookTest, MidPriceCalculation) {
    book.updateBid(0.50, 1000);
    book.updateAsk(0.52, 800);
    
    EXPECT_TRUE(approxEqual(book.getMid(), 0.51));
}

TEST_F(OrderBookTest, SpreadCalculation) {
    book.updateBid(0.50, 1000);
    book.updateAsk(0.52, 800);
    
    EXPECT_TRUE(approxEqual(book.getSpread(), 0.02));
}

TEST_F(OrderBookTest, VolumeCalculations) {
    book.updateBid(0.50, 1000);
    book.updateBid(0.49, 500);
    book.updateBid(0.48, 200);
    book.updateAsk(0.51, 800);
    book.updateAsk(0.52, 1200);
    
    EXPECT_TRUE(approxEqual(book.getTotalBidVolume(2), 1500));  // Top 2 levels
    EXPECT_TRUE(approxEqual(book.getTotalAskVolume(), 2000));   // All levels
}

TEST_F(OrderBookTest, UpdateExistingLevel) {
    book.updateBid(0.50, 1000);
    EXPECT_TRUE(approxEqual(book.getBestBid(), 0.50));
    
    book.updateBid(0.50, 2000);
    EXPECT_TRUE(approxEqual(book.getBestBid(), 0.50));
}

TEST_F(OrderBookTest, RemoveLevel) {
    book.updateBid(0.50, 1000);
    book.updateBid(0.49, 500);
    
    book.updateBid(0.50, 0);  // Remove by setting to 0
    EXPECT_TRUE(approxEqual(book.getBestBid(), 0.49));
}

TEST_F(OrderBookTest, BestBidAskOrdering) {
    book.updateBid(0.48, 200);
    book.updateBid(0.50, 1000);
    book.updateBid(0.49, 500);
    
    EXPECT_TRUE(approxEqual(book.getBestBid(), 0.50));  // Highest bid
    
    book.updateAsk(0.53, 300);
    book.updateAsk(0.51, 800);
    book.updateAsk(0.52, 1200);
    
    EXPECT_TRUE(approxEqual(book.getBestAsk(), 0.51));  // Lowest ask
}