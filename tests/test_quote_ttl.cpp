#include <gtest/gtest.h>
#include "core/types.hpp"
#include "strategy/market_maker.hpp"
#include "data/order_book.hpp"
#include <thread>
#include <chrono>

using namespace pmm;

TEST(QuoteTTLTest, MarketPhaseDetection) {
    MarketMetadata metadata;
    metadata.has_end_time = true;
    
    auto now = std::chrono::system_clock::now();
    
    metadata.event_end_time = now + std::chrono::hours(3);
    EXPECT_EQ(metadata.getMarketPhase(), MarketPhase::PRE_MATCH_EARLY);
    EXPECT_EQ(metadata.getRecommendedTTL(), 90);
    EXPECT_EQ(metadata.getRequoteInterval(), 45);
    
    metadata.event_end_time = now + std::chrono::minutes(45);
    EXPECT_EQ(metadata.getMarketPhase(), MarketPhase::PRE_MATCH_LATE);
    EXPECT_EQ(metadata.getRecommendedTTL(), 45);
    EXPECT_EQ(metadata.getRequoteInterval(), 22);
    
    metadata.event_end_time = now + std::chrono::minutes(8);
    EXPECT_EQ(metadata.getMarketPhase(), MarketPhase::PRE_MATCH_CRITICAL);
    EXPECT_EQ(metadata.getRecommendedTTL(), 20);
    EXPECT_EQ(metadata.getRequoteInterval(), 7);
    
    metadata.event_end_time = now - std::chrono::minutes(5);
    EXPECT_EQ(metadata.getMarketPhase(), MarketPhase::IN_PLAY);
    EXPECT_EQ(metadata.getRecommendedTTL(), 3);
    EXPECT_EQ(metadata.getRequoteInterval(), 1);
}

TEST(QuoteTTLTest, QuoteCreationWithTTL) {
    MarketMaker mm(0.02, 1000.0);
    OrderBook book("test_token");
    
    book.updateBid(0.50, 100.0);
    book.updateAsk(0.52, 100.0);
    
    MarketMetadata metadata;
    metadata.has_end_time = true;
    metadata.event_end_time = std::chrono::system_clock::now() + std::chrono::hours(3);
    
    auto quote_opt = mm.generateQuote(book, &metadata, 1.0);
    
    ASSERT_TRUE(quote_opt.has_value());
    const Quote& quote = quote_opt.value();
    
    EXPECT_EQ(quote.ttl_seconds, 90);
    
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - quote.created_at);
    EXPECT_LT(age.count(), 1);
}

TEST(QuoteTTLTest, QuoteTTLChangesWithPhase) {
    MarketMaker mm(0.02, 1000.0);
    OrderBook book("test_token");
    
    book.updateBid(0.50, 100.0);
    book.updateAsk(0.52, 100.0);
    
    MarketMetadata metadata;
    metadata.has_end_time = true;
    
    auto now = std::chrono::system_clock::now();
    
    struct TestCase {
        std::chrono::system_clock::time_point end_time;
        int expected_ttl;
    };
    
    std::vector<TestCase> test_cases = {
        {now + std::chrono::hours(3), 90},
        {now + std::chrono::minutes(45), 45},
        {now + std::chrono::minutes(8), 20}, 
        {now - std::chrono::minutes(5), 3}
    };
    
    for (const auto& test_case : test_cases) {
        metadata.event_end_time = test_case.end_time;
        auto quote_opt = mm.generateQuote(book, &metadata, 1.0);
        
        ASSERT_TRUE(quote_opt.has_value());
        EXPECT_EQ(quote_opt->ttl_seconds, test_case.expected_ttl);
    }
}

TEST(QuoteTTLTest, QuoteWithoutMetadata) {
    MarketMaker mm(0.02, 1000.0);
    OrderBook book("test_token");
    
    book.updateBid(0.50, 100.0);
    book.updateAsk(0.52, 100.0);
    
    auto quote_opt = mm.generateQuote(book, nullptr, 1.0);
    
    ASSERT_TRUE(quote_opt.has_value());
    const Quote& quote = quote_opt.value();
    
    EXPECT_EQ(quote.ttl_seconds, 90);
}

TEST(QuoteTTLTest, QuoteExpiration) {
    Quote quote{0.50, 100.0, 0.52, 100.0, 2, std::chrono::steady_clock::now()};
    
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - quote.created_at);
    EXPECT_LT(age.count(), quote.ttl_seconds);
    
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    now = std::chrono::steady_clock::now();
    age = std::chrono::duration_cast<std::chrono::seconds>(now - quote.created_at);
    EXPECT_GE(age.count(), quote.ttl_seconds);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
