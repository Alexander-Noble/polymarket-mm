#include "utils/market_summary_logger.hpp"
#include "utils/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <sstream>

namespace pmm {

void RollingWindow::add(double value, std::chrono::steady_clock::time_point timestamp) {
    values.push_back(value);
    timestamps.push_back(timestamp);
    cleanup(timestamp);
}

void RollingWindow::cleanup(std::chrono::steady_clock::time_point now) {
    while (!timestamps.empty() && (now - timestamps.front()) > window_size) {
        values.pop_front();
        timestamps.pop_front();
    }
}

double RollingWindow::mean() const {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double RollingWindow::stddev() const {
    if (values.size() < 2) return 0.0;
    double m = mean();
    double sq_sum = 0.0;
    for (double v : values) {
        sq_sum += (v - m) * (v - m);
    }
    return std::sqrt(sq_sum / values.size());
}

double RollingWindow::max() const {
    if (values.empty()) return 0.0;
    return *std::max_element(values.begin(), values.end());
}

double RollingWindow::min() const {
    if (values.empty()) return 0.0;
    return *std::min_element(values.begin(), values.end());
}

MarketSummaryLogger::MarketSummaryLogger(const std::filesystem::path& session_dir)
    : session_dir_(session_dir),
      start_time_(std::chrono::steady_clock::now()),
      last_summary_time_(std::chrono::steady_clock::now() - std::chrono::seconds(9999)) {
    initializeFile();
}

MarketSummaryLogger::~MarketSummaryLogger() {
    if (summary_file_.is_open()) {
        summary_file_.close();
    }
}

void MarketSummaryLogger::initializeFile() {
    summary_file_.open(session_dir_ / "market_summary.csv");
    summary_file_ << "timestamp,market_name,market_id,token_id,"
                  << "mid_price,spread_bps,best_bid,best_ask,"
                  << "mid_price_volatility,price_trend,max_price_move,"
                  << "quote_change_rate,bid_stability_score,ask_stability_score,"
                  << "avg_spread_bps,liquidity_score,depth_score,"
                  << "update_frequency,volume_trend,"
                  << "hours_to_event,is_tradeable,trading_quality_score\n";
}

void MarketSummaryLogger::updateMarket(const std::string& market_name, const std::string& market_id,
                                       const std::string& condition_id, const TokenId& token_id,
                                       Price mid_price, double spread_bps,
                                       Price best_bid, Price best_ask,
                                       double bid_volume, double ask_volume,
                                       int bid_levels, int ask_levels) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto& state = market_states_[token_id];

    if (state.update_count == 0) {
        state.token_id = token_id;
        state.market_name = market_name;
        state.market_id = market_id;
        state.condition_id = condition_id;
        state.first_update = now;
        state.last_best_bid = best_bid;
        state.last_best_ask = best_ask;

        auto end_it = event_end_times_.find(condition_id);
        if (end_it != event_end_times_.end()) {
            state.event_end_time = end_it->second;
        }
    }
    
    if (best_bid != state.last_best_bid) {
        state.bid_changes++;
        state.last_best_bid = best_bid;
    }
    if (best_ask != state.last_best_ask) {
        state.ask_changes++;
        state.last_best_ask = best_ask;
    }
    
    state.current_mid = mid_price;
    state.current_spread = best_ask - best_bid;
    state.current_spread_bps = spread_bps;
    state.current_best_bid = best_bid;
    state.current_best_ask = best_ask;
    state.current_bid_volume = bid_volume;
    state.current_ask_volume = ask_volume;
    state.current_bid_levels = bid_levels;
    state.current_ask_levels = ask_levels;

    if (mid_price > 0) {
        state.mid_prices.add(mid_price, now);
    }
    if (spread_bps > 0) {
        state.spreads_bps.add(spread_bps, now);
    }
    state.bid_volumes.add(bid_volume, now);
    state.ask_volumes.add(ask_volume, now);
    
    state.update_count++;
    state.last_update = now;
}

void MarketSummaryLogger::setEventEndTime(const std::string& condition_id,
                                          std::chrono::system_clock::time_point end_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_end_times_[condition_id] = end_time;
    
    for (auto& [token_id, state] : market_states_) {
        if (state.condition_id == condition_id) {
            state.event_end_time = end_time;
        }
    }
}

bool MarketSummaryLogger::shouldLogSummary() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_summary_time_);
    return elapsed >= getUpdateInterval();
}

std::chrono::seconds MarketSummaryLogger::getUpdateInterval() const {
    double min_hours = getMinHoursToEvent();
    return getAdaptiveInterval(min_hours);
}

std::chrono::seconds MarketSummaryLogger::getAdaptiveInterval(double hours_to_event) const {
    if (hours_to_event < 0) {
        return std::chrono::seconds(300);
    } else if (hours_to_event < 3.0) {
        return std::chrono::seconds(30);
    } else if (hours_to_event < 6.0) {
        return std::chrono::seconds(60);
    } else if (hours_to_event < 24.0) {
        return std::chrono::seconds(300);
    } else if (hours_to_event < 48.0) {
        return std::chrono::seconds(600);
    } else {
        return std::chrono::seconds(1800);
    }
}

double MarketSummaryLogger::getMinHoursToEvent() const {
    auto now = std::chrono::system_clock::now();
    double min_hours = -1.0;
    
    for (const auto& [token_id, state] : market_states_) {
        if (state.event_end_time != std::chrono::system_clock::time_point{}) {
            auto duration = state.event_end_time - now;
            double hours = std::chrono::duration_cast<std::chrono::hours>(duration).count();
            if (min_hours < 0 || hours < min_hours) {
                min_hours = hours;
            }
        }
    }
    
    return min_hours;
}

void MarketSummaryLogger::logSummaries() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!summary_file_.is_open()) return;
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [token_id, state] : market_states_) {
        if (state.update_count == 0) continue;
        
        state.mid_prices.cleanup(now);
        state.spreads_bps.cleanup(now);
        state.bid_volumes.cleanup(now);
        state.ask_volumes.cleanup(now);
        
        MarketSummary summary = computeSummary(state);

        auto time_now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(time_now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
        
        summary_file_ << ss.str() << ","
                     << summary.market_name << ","
                     << summary.market_id << ","
                     << summary.token_id << ","
                     << summary.mid_price << ","
                     << summary.spread_bps << ","
                     << summary.best_bid << ","
                     << summary.best_ask << ","
                     << summary.mid_price_volatility << ","
                     << summary.price_trend << ","
                     << summary.max_price_move << ","
                     << summary.quote_change_rate << ","
                     << summary.bid_stability_score << ","
                     << summary.ask_stability_score << ","
                     << summary.avg_spread_bps << ","
                     << summary.liquidity_score << ","
                     << summary.depth_score << ","
                     << summary.update_frequency << ","
                     << summary.volume_trend << ","
                     << summary.hours_to_event << ","
                     << (summary.is_tradeable ? "1" : "0") << ","
                     << summary.trading_quality_score << "\n";
    }
    
    summary_file_.flush();
    last_summary_time_ = now;
    
    LOG_DEBUG("Logged market summaries for {} markets (interval: {}s)", 
              market_states_.size(), getUpdateInterval().count());
}

MarketSummary MarketSummaryLogger::computeSummary(const MarketState& state) {
    MarketSummary summary;
    
    summary.market_name = state.market_name;
    summary.market_id = state.market_id;
    summary.token_id = state.token_id;
    
    summary.mid_price = state.current_mid;
    summary.spread_bps = state.current_spread_bps;
    summary.best_bid = state.current_best_bid;
    summary.best_ask = state.current_best_ask;
    
    summary.mid_price_volatility = computeVolatility(state.mid_prices);
    summary.price_trend = computeTrend(state.mid_prices);
    
    double price_range = state.mid_prices.max() - state.mid_prices.min();
    double mid = state.mid_prices.mean();
    summary.max_price_move = (mid > 0) ? (price_range / mid) : 0.0;

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - state.first_update);
    double minutes = std::max(1.0, static_cast<double>(duration.count()));
    
    summary.quote_change_rate = (state.bid_changes + state.ask_changes) / minutes;
    
    // Stability scores: 1.0 = stable, 0.0 = very unstable
    // Use exponential decay: e^(-changes/updates)
    double bid_change_ratio = state.update_count > 0 ? 
        static_cast<double>(state.bid_changes) / state.update_count : 0.0;
    double ask_change_ratio = state.update_count > 0 ?
        static_cast<double>(state.ask_changes) / state.update_count : 0.0;
    
    summary.bid_stability_score = std::exp(-5.0 * bid_change_ratio);
    summary.ask_stability_score = std::exp(-5.0 * ask_change_ratio);
    
    summary.avg_spread_bps = state.spreads_bps.mean();
    
    double total_volume = state.current_bid_volume + state.current_ask_volume;
    summary.liquidity_score = (summary.avg_spread_bps > 0) ? 
        (total_volume / summary.avg_spread_bps) : 0.0;
    
    summary.depth_score = (state.bid_volumes.mean() + state.ask_volumes.mean());
    
    summary.update_frequency = state.update_count / minutes;
    
    double recent_vol = state.bid_volumes.size() > 0 ? 
        (state.bid_volumes.values.back() + state.ask_volumes.values.back()) : 0.0;
    double early_vol = state.bid_volumes.size() > 5 ?
        (state.bid_volumes.values.front() + state.ask_volumes.values.front()) : recent_vol;
    
    summary.volume_trend = (early_vol > 0) ? ((recent_vol - early_vol) / early_vol) : 0.0;

    if (state.event_end_time != std::chrono::system_clock::time_point{}) {
        auto time_now = std::chrono::system_clock::now();
        auto duration = state.event_end_time - time_now;
        summary.hours_to_event = std::chrono::duration<double, std::ratio<3600>>(duration).count();
    } else {
        summary.hours_to_event = -1.0;
    }

    summary.trading_quality_score = computeQualityScore(summary);
    summary.is_tradeable = (summary.trading_quality_score >= 50);
    
    return summary;
}

double MarketSummaryLogger::computeVolatility(const RollingWindow& window) {
    if (window.size() < 2) return 0.0;
    
    double m = window.mean();
    if (m <= 0.0) return 0.0;
    
    return window.stddev() / m;
}

double MarketSummaryLogger::computeTrend(const RollingWindow& window) {
    if (window.size() < 2) return 0.0;
    
    size_t n = window.size();
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    
    for (size_t i = 0; i < n; i++) {
        double x = static_cast<double>(i);
        double y = window.values[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    
    double denominator = (n * sum_x2 - sum_x * sum_x);
    if (std::abs(denominator) < 1e-10) return 0.0;
    
    double slope = (n * sum_xy - sum_x * sum_y) / denominator;
    
    double mean_price = sum_y / n;
    return (mean_price > 0) ? (slope / mean_price) : 0.0;
}

int MarketSummaryLogger::computeQualityScore(const MarketSummary& summary) {
    int score = 0;
    
    // Liquidity component (0-40 points)
    // Good liquidity: score > 1000, excellent: > 5000
    if (summary.liquidity_score > 5000) {
        score += 40;
    } else if (summary.liquidity_score > 1000) {
        score += static_cast<int>(20 + (summary.liquidity_score - 1000) / 4000.0 * 20);
    } else if (summary.liquidity_score > 100) {
        score += static_cast<int>((summary.liquidity_score / 1000.0) * 20);
    }
    
    // Spread component (0-25 points)
    // Tight spread: < 100bps = 25 pts, < 300bps = 15 pts
    if (summary.avg_spread_bps < 100) {
        score += 25;
    } else if (summary.avg_spread_bps < 300) {
        score += static_cast<int>(25 - (summary.avg_spread_bps - 100) / 200.0 * 10);
    } else if (summary.avg_spread_bps < 500) {
        score += static_cast<int>(15 - (summary.avg_spread_bps - 300) / 200.0 * 10);
    }
    
    // Stability component (0-20 points)
    // High stability = good for market making
    double avg_stability = (summary.bid_stability_score + summary.ask_stability_score) / 2.0;
    score += static_cast<int>(avg_stability * 20);
    
    // Activity component (0-15 points)
    // Good update frequency: > 1/min = 15 pts
    if (summary.update_frequency > 1.0) {
        score += 15;
    } else {
        score += static_cast<int>(summary.update_frequency * 15);
    }
    
    return std::min(100, std::max(0, score));
}

} // namespace pmm
