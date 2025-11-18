#pragma once

#include "core/types.hpp"
#include <fstream>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <deque>
#include <unordered_map>

namespace pmm {

struct RollingWindow {
    std::deque<double> values;
    std::deque<std::chrono::steady_clock::time_point> timestamps;
    std::chrono::seconds window_size;
    
    explicit RollingWindow(std::chrono::seconds window = std::chrono::seconds(300))
        : window_size(window) {}
    
    void add(double value, std::chrono::steady_clock::time_point timestamp);
    void cleanup(std::chrono::steady_clock::time_point now);
    double mean() const;
    double stddev() const;
    double max() const;
    double min() const;
    size_t size() const { return values.size(); }
};

struct MarketState {
    TokenId token_id;
    std::string market_name;
    std::string market_id;
    std::string condition_id;
    
    Price current_mid = 0.0;
    Price current_spread = 0.0;
    double current_spread_bps = 0.0;
    Price current_best_bid = 0.0;
    Price current_best_ask = 0.0;
    double current_bid_volume = 0.0;
    double current_ask_volume = 0.0;
    int current_bid_levels = 0;
    int current_ask_levels = 0;
    
    RollingWindow mid_prices;
    RollingWindow spreads_bps;
    RollingWindow bid_volumes;
    RollingWindow ask_volumes;
    
    Price last_best_bid = 0.0;
    Price last_best_ask = 0.0;
    int bid_changes = 0;
    int ask_changes = 0;
    int update_count = 0;
    
    std::chrono::steady_clock::time_point first_update;
    std::chrono::steady_clock::time_point last_update;
    std::chrono::system_clock::time_point event_end_time;
    
    MarketState() 
        : mid_prices(std::chrono::seconds(300)),
          spreads_bps(std::chrono::seconds(300)),
          bid_volumes(std::chrono::seconds(300)),
          ask_volumes(std::chrono::seconds(300)) {}
};

struct MarketSummary {
    std::string timestamp;
    std::string market_name;
    std::string market_id;
    TokenId token_id;
    
    Price mid_price;
    double spread_bps;
    Price best_bid;
    Price best_ask;
    
    double mid_price_volatility;
    double price_trend;          
    double max_price_move;
    
    double quote_change_rate;   
    double bid_stability_score; 
    double ask_stability_score;

    double avg_spread_bps;
    double liquidity_score;
    double depth_score;
    
    double update_frequency;
    double volume_trend;
    
    double hours_to_event;
    
    bool is_tradeable;
    int trading_quality_score;
};

class MarketSummaryLogger {
public:
    explicit MarketSummaryLogger(const std::filesystem::path& session_dir);
    ~MarketSummaryLogger();
    
    void updateMarket(const std::string& market_name, const std::string& market_id,
                     const std::string& condition_id, const TokenId& token_id,
                     Price mid_price, double spread_bps,
                     Price best_bid, Price best_ask,
                     double bid_volume, double ask_volume,
                     int bid_levels, int ask_levels);
    
    void setEventEndTime(const std::string& condition_id, 
                        std::chrono::system_clock::time_point end_time);
    bool shouldLogSummary() const;
    void logSummaries();

    std::chrono::seconds getUpdateInterval() const;
    
private:
    std::filesystem::path session_dir_;
    std::ofstream summary_file_;
    std::mutex mutex_;
    
    std::unordered_map<TokenId, MarketState> market_states_;
    std::unordered_map<std::string, std::chrono::system_clock::time_point> event_end_times_;
    
    std::chrono::steady_clock::time_point last_summary_time_;
    std::chrono::steady_clock::time_point start_time_;
    
    void initializeFile();
    MarketSummary computeSummary(const MarketState& state);
    double computeVolatility(const RollingWindow& window);
    double computeTrend(const RollingWindow& window);
    int computeQualityScore(const MarketSummary& summary);
    std::chrono::seconds getAdaptiveInterval(double hours_to_event) const;
    double getMinHoursToEvent() const;
};

} // namespace pmm
