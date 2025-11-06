#include "data/order_book.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace pmm;

// Helper for floating-point comparison
bool approxEqual(double a, double b, double epsilon = 1e-9) {
    return std::abs(a - b) < epsilon;
}

int main() {
    OrderBook book("test_token_123");
    
    // Initially empty
    assert(!book.hasValidBBO());
    std::cout << "Empty book has no valid BBO\n";
    
    // Add some bids
    book.updateBid(0.50, 1000);
    book.updateBid(0.49, 500);
    book.updateBid(0.48, 200);
    
    // Add some asks
    book.updateAsk(0.51, 800);
    book.updateAsk(0.52, 1200);
    
    // Test best bid/ask
    assert(approxEqual(book.getBestBid(), 0.50));
    assert(approxEqual(book.getBestAsk(), 0.51));
    std::cout << "Best bid/ask correct\n";
    
    // Test mid and spread
    assert(approxEqual(book.getMid(), 0.505));
    assert(approxEqual(book.getSpread(), 0.01));
    std::cout << "Mid and spread correct\n";
    
    // Test volume
    assert(approxEqual(book.getTotalBidVolume(2), 1500));  // 1000 + 500
    assert(approxEqual(book.getTotalAskVolume(), 2000));    // 800 + 1200
    std::cout << "Volume calculations correct\n";
    
    // Test update (modify level)
    book.updateBid(0.50, 2000);
    assert(approxEqual(book.getTotalBidVolume(1), 2000));
    std::cout << "Level update works\n";
    
    // Test removal
    book.updateBid(0.50, 0);
    assert(approxEqual(book.getBestBid(), 0.49));
    std::cout << "Level removal works\n";
    
    // Test imbalance
    book.clear();
    book.updateBid(0.50, 1000);
    book.updateAsk(0.51, 500);
    double imbalance = book.getImbalance();
    assert(approxEqual(imbalance, (1000.0 - 500.0) / (1000.0 + 500.0)));
    std::cout << "Imbalance calculation correct\n";
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}