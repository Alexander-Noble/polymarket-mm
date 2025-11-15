#include <gtest/gtest.h>
#include "strategy/market_maker.hpp"
#include "data/order_book.hpp"

using namespace pmm;

class MarketMakerTest : public ::testing::Test {
protected:
    void SetUp() override {
        book = std::make_unique<OrderBook>("test_token");
        mm = std::make_unique<MarketMaker>(0.02, 100000.0);  
    }
    
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<MarketMaker> mm;
    
    bool approxEqual(double a, double b, double epsilon = 1e-6) {
        return std::abs(a - b) < epsilon;
    }
};

TEST_F(MarketMakerTest, InitialState) {
    EXPECT_NE(mm, nullptr);
}

TEST_F(MarketMakerTest, QuoteGeneration) {
    book->updateBid(0.48, 1000);
    book->updateAsk(0.54, 800);
    
    auto quote = mm->generateQuote(*book);
    
    EXPECT_TRUE(quote.has_value());
    
    auto quote_value = *std::move(quote);
    EXPECT_LT(quote_value.ask_price, 0.54);
    EXPECT_GT(quote_value.bid_price, 0.48);
}

TEST_F(MarketMakerTest, QuotesRespectInventory) {
    book->updateBid(0.48, 1000);
    book->updateAsk(0.54, 800);
    
    mm->updateInventory(Side::BUY, 1000, 0.50);
    auto quote = mm->generateQuote(*book);
    EXPECT_TRUE(quote.has_value());
    
    EXPECT_LT(quote->bid_price, 0.51);
    EXPECT_LT(quote->ask_price, 0.53);
}

TEST_F(MarketMakerTest, QuoteSizing) {
    book->updateBid(0.48, 1000);
    book->updateAsk(0.54, 800);
    
    auto quote = mm->generateQuote(*book);
    
    EXPECT_GT(quote->bid_size, 0);
    EXPECT_LT(quote->ask_size, 10000); 
}

TEST_F(MarketMakerTest, PreventsSellAtLoss) {
    mm->updateInventory(Side::BUY, 1000, 0.55);
    book->updateBid(0.48, 1000);
    book->updateAsk(0.52, 800);
    
    auto quote = mm->generateQuote(*book);
    EXPECT_TRUE(quote.has_value());
    EXPECT_GE(quote->ask_price, 0.55);
}

TEST_F(MarketMakerTest, AllowsLossWhenHighRisk) {
    mm = std::make_unique<MarketMaker>(0.02, 1000.0);  
    
    mm->updateInventory(Side::BUY, 1500, 0.55);
    book->updateBid(0.48, 1000);
    book->updateAsk(0.52, 800);
    
    auto quote = mm->generateQuote(*book);
    EXPECT_FALSE(quote.has_value());
}

TEST_F(MarketMakerTest, AcceptsBreakevenAtHighRisk) {
    mm->updateInventory(Side::BUY, 1500, 0.51);

    book->updateBid(0.48, 1000);
    book->updateAsk(0.52, 800);
    
    auto quote = mm->generateQuote(*book);
    EXPECT_TRUE(quote.has_value());
    EXPECT_GE(quote->ask_price, 0.51);
}

TEST_F(MarketMakerTest, TimeUrgencyWithNoCloseTime) {
    EXPECT_DOUBLE_EQ(mm->getTimeUrgency(), 0.0);
}

TEST_F(MarketMakerTest, TimeUrgencyCalculation) {
    auto now = std::chrono::system_clock::now();
    
    mm->setMarketCloseTime(now + std::chrono::hours(48));
    EXPECT_DOUBLE_EQ(mm->getTimeUrgency(), 0.0);
    
    mm->setMarketCloseTime(now + std::chrono::hours(12));
    EXPECT_NEAR(mm->getTimeUrgency(), 0.5, 0.05);  // Allow 5% tolerance for timing
    
    mm->setMarketCloseTime(now + std::chrono::hours(1));
    EXPECT_GT(mm->getTimeUrgency(), 0.9);  // Should be very high
    
    mm->setMarketCloseTime(now - std::chrono::hours(1));
    EXPECT_DOUBLE_EQ(mm->getTimeUrgency(), 1.0);
}

TEST_F(MarketMakerTest, AcceptsLossNearMarketClose) {
    auto now = std::chrono::system_clock::now();
    mm->setMarketCloseTime(now + std::chrono::minutes(30));
    
    mm->updateInventory(Side::BUY, 1000, 0.55);
    book->updateBid(0.48, 1000);
    book->updateAsk(0.52, 800);
    
    auto quote = mm->generateQuote(*book);
    EXPECT_TRUE(quote.has_value());
    
    EXPECT_GE(quote->ask_price, 0.54);
    EXPECT_LT(quote->ask_price, 0.56);
}

TEST_F(MarketMakerTest, ReducedProfitRequirementWithTimeUrgency) {
    auto now = std::chrono::system_clock::now();
    mm->setMarketCloseTime(now + std::chrono::hours(12));
    
    mm->updateInventory(Side::BUY, 1000, 0.50);
    book->updateBid(0.48, 1000);
    book->updateAsk(0.52, 800);
    
    auto quote = mm->generateQuote(*book);
    EXPECT_TRUE(quote.has_value());
    
    EXPECT_GE(quote->ask_price, 0.50);
    EXPECT_LT(quote->ask_price, 0.52);
}