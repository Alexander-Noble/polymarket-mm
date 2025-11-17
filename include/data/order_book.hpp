#pragma once

#include "core/types.hpp"
#include <map>

namespace pmm {

class OrderBook {
private:
    TokenId token_id_;
    std::map<Price, Size, std::greater<Price>> bids_;
    std::map<Price, Size> asks_;

public:
    explicit OrderBook(TokenId token_id) : token_id_(std::move(token_id)) {}
    
    void updateBid(Price price, Size size);
    void updateAsk(Price price, Size size);
    
    void clear();

    Price getBestBid() const;
    Price getBestAsk() const;
    Price getSpread() const;
    Price getMid() const;

    bool hasValidBBO() const;
    
    Size getTotalBidVolume(int levels = 5) const;
    Size getTotalAskVolume(int levels = 5) const;

    double getImbalance() const;
    
    int getBidLevelCount() const;
    int getAskLevelCount() const;
};

} // namespace pmm