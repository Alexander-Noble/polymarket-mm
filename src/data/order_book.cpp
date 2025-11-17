#include "data/order_book.hpp"

namespace pmm {

    void OrderBook::updateBid(Price price, Size size) {
        if (size == 0) {
            bids_.erase(price);
        } else {
            bids_[price] = size;
        }
    }

    void OrderBook::updateAsk(Price price, Size size) {
        if (size == 0) {
            asks_.erase(price);
        } else {
            asks_[price] = size;
        }
    }

    void OrderBook::clear() {
        bids_.clear();
        asks_.clear();
    }

    Price OrderBook::getBestBid() const {
        if (!bids_.empty()) {
            return bids_.begin()->first;
        }
        return 0.0;
    }

    Price OrderBook::getBestAsk() const {
        if (!asks_.empty()) {
            return asks_.begin()->first;
        }
        return 0.0;
    }

    Price OrderBook::getSpread() const {
        if (!hasValidBBO()) {
            return 0.0;
        }
        return getBestAsk() - getBestBid();
    }

    Price OrderBook::getMid() const {
        if (!hasValidBBO()) {
            return 0.0;
        }
        return (getBestBid() + getBestAsk()) / 2.0;
    }

    bool OrderBook::hasValidBBO() const {
        return !bids_.empty() && !asks_.empty();
    }

    Size OrderBook::getTotalBidVolume(int levels) const {
        Size total = 0;
        int count = 0;
        for (const auto& [price, size] : bids_) {
            total += size;
            if (++count >= levels) break;
        }
        return total;
    }

    Size OrderBook::getTotalAskVolume(int levels) const {
        Size total = 0;
        int count = 0;
        for (const auto& [price, size] : asks_) {
            total += size;
            if (++count >= levels) break;
        }
        return total;
    }

    double OrderBook::getImbalance() const {
        double bid_vol = getTotalBidVolume();
        double ask_vol = getTotalAskVolume();
        double total = bid_vol + ask_vol;
        
        if (total == 0.0) {
            return 0.0;
        }
        
        return (bid_vol - ask_vol) / total;
    }

    int OrderBook::getBidLevelCount() const {
        return static_cast<int>(bids_.size());
    }

    int OrderBook::getAskLevelCount() const {
        return static_cast<int>(asks_.size());
    }

}