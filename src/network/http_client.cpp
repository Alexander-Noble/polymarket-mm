#include "network/http_client.hpp"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <iostream>

namespace pmm {

// Callback for CURL
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

PolymarketHttpClient::PolymarketHttpClient()
    : api_base_url_("https://gamma-api.polymarket.com") {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::cout << "PolymarketHttpClient initialized\n";
}

std::string PolymarketHttpClient::httpGet(const std::string& endpoint) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    if (curl) {
        std::string url = api_base_url_ + endpoint;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "CURL error: " << curl_easy_strerror(res) << "\n";
        }
        
        curl_easy_cleanup(curl);
    }
    
    return response;
}

std::vector<EventInfo> PolymarketHttpClient::parseBatch(const std::string& response) {
    std::vector<EventInfo> events;
    
    if (response.empty()) {
        return events;
    }
    
    try {
        auto json = nlohmann::json::parse(response);

        if (!json.is_array()) {
            std::cerr << "Expected array response from API\n";
            return events;
        }
        
        for (const auto& event_json : json) {
            EventInfo event;
            event.event_id = event_json.value("id", "");
            event.title = event_json.value("title", "");
            event.slug = event_json.value("slug", "");
            event.description = event_json.value("description", "");
            event.start_date = event_json.value("startDate", "");
            event.end_date = event_json.value("endDate", "");
            event.category = event_json.value("category", "");
            event.active = event_json.value("active", false);
            event.closed = event_json.value("closed", false);

            if (event_json.contains("volume")) {
                if (event_json["volume"].is_number()) {
                    event.volume = event_json["volume"].get<double>();
                } else if (event_json["volume"].is_string()) {
                    event.volume = std::stod(event_json["volume"].get<std::string>());
                }
            }
            
            if (event_json.contains("liquidity")) {
                if (event_json["liquidity"].is_number()) {
                    event.liquidity = event_json["liquidity"].get<double>();
                } else if (event_json["liquidity"].is_string()) {
                    event.liquidity = std::stod(event_json["liquidity"].get<std::string>());
                }
            }
            
            event.markets = std::vector<MarketInfo>();
            
            if (event_json.contains("markets") && event_json["markets"].is_array()) {
                for (const auto& market_json : event_json["markets"]) {
                    MarketInfo market;
                    market.market_id = market_json.value("id", "");
                    market.condition_id = market_json.value("conditionId", "");
                    market.question = market_json.value("question", "");
                    market.description = market_json.value("description", "");
                    market.slug = market_json.value("slug", "");
                    market.active = market_json.value("active", false);
                    market.volume = std::stod(market_json.value("volume", "0"));
                    market.liquidity = std::stod(market_json.value("liquidity", "0"));
                    
                    if (market_json.contains("clobTokenIds")) {
                        auto token_ids = nlohmann::json::parse(market_json["clobTokenIds"].get<std::string>());
                        if (token_ids.is_array()) {
                            for (const auto& token_id : token_ids) {
                                market.tokens.push_back(token_id.get<std::string>());
                            }
                        }
                    }

                    if (market_json.contains("outcomes")) {
                        auto outcomes = nlohmann::json::parse(market_json["outcomes"].get<std::string>());
                        if (outcomes.is_array()) {
                            for (const auto& outcome : outcomes) {
                                market.outcomes.push_back(outcome.get<std::string>());
                            }
                        }
                    }

                    event.markets.push_back(market);
                }
            }
            
            events.push_back(event);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error parsing batch: " << e.what() << "\n";
    }
    
    return events;
}

std::vector<EventInfo> PolymarketHttpClient::getActiveEvents(int limit) {
    std::cout << "Fetching active events from Polymarket...\n";
    
    std::vector<EventInfo> all_events;
    int offset = 0;

    auto now = std::chrono::system_clock::now();
    auto tomorrow = now + std::chrono::hours(24);
    auto week_from_now = now + std::chrono::hours(24 * 7);
    
    // Format dates as ISO 8601 (YYYY-MM-DDTHH:MM:SSZ)
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::time_t week_t = std::chrono::system_clock::to_time_t(week_from_now);
    
    char now_str[25];
    char week_str[25];
    std::strftime(now_str, sizeof(now_str), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_t));
    std::strftime(week_str, sizeof(week_str), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&week_t));

    std::string endpoint = "/events?limit=" + std::to_string(limit)
                        + "&active=true"
                        + "&closed=false"
                        + "&archived=false"
                        + "&end_date_min=" + std::string(now_str)      // Ends after now (not expired)
                        + "&start_date_max=" + std::string(week_str)   // Starts within 7 days
                        + "&order=volume"                               // Sort by volume
                        + "&ascending=false";   
    while (true) {
        auto paged_endpoint = endpoint+ "&offset=" + std::to_string(offset);   
        std::string response = httpGet(paged_endpoint);
        
        if (response.empty()) {
            break;
        }
        std::vector<EventInfo> batch = parseBatch(response);
        
        if (batch.empty()) {
            break;
        }
        
        all_events.insert(all_events.end(), batch.begin(), batch.end());
        
        std::cout << "Fetched " << batch.size() << " events (total: " << all_events.size() << ")\n";
        
        if (batch.size() < limit) {
            break;
        }
        
        offset += limit;
    }
    
    std::cout << "Total events fetched: " << all_events.size() << "\n";
    return all_events;
}

std::vector<EventInfo> PolymarketHttpClient::searchEvents(const std::string& query) {
    std::cout << "Searching for events: \"" << query << "\"\n";
    
    // Fetch a batch of active events
    auto events = getActiveEvents(100);
    
    // Filter by query (case-insensitive substring match)
    std::vector<EventInfo> filtered;
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);
    
    const double MIN_VOLUME = 500.0;
    const double MIN_LIQUIDITY = 1000.0;
    for (const auto& event : events) {

        if (event.volume < MIN_VOLUME || event.liquidity < MIN_LIQUIDITY) {
            continue;
        }
        std::string lower_title = event.title;
        std::string slug = event.slug;
        std::string description = event.description;
        std::transform(lower_title.begin(), lower_title.end(), 
                      lower_title.begin(), ::tolower);
        std::transform(slug.begin(), slug.end(), slug.begin(), ::tolower);
        std::transform(description.begin(), description.end(), description.begin(), ::tolower);

        if (
            // (
            // lower_question.find(lower_query) != std::string::npos) || 
            (slug.find(lower_query) != std::string::npos) ||
            (description.find(lower_query) != std::string::npos)) {
            
            if (lower_title.find("top 4") != std::string::npos ||
                lower_title.find("top goal scorer") != std::string::npos ||
                lower_title.find("finish in") != std::string::npos ||
                lower_title.find("last place") != std::string::npos ||
                lower_title.find("2nd place") != std::string::npos ||
                lower_title.find("3rd place") != std::string::npos ||
                lower_title.find("be promoted") != std::string::npos) {
                continue;  // Skip season-long markets
            }
            
            // Only keep match markets (has "vs." or "win on" or "end in a draw")
            if (lower_title.find(" vs. ") != std::string::npos ||
                lower_title.find(" vs ") != std::string::npos ||
                lower_title.find("win on 2025") != std::string::npos ||
                lower_title.find("end in a draw") != std::string::npos) {
                filtered.push_back(event);
            }
        }
    }
    
    std::cout << "Found " << filtered.size() << " matching markets\n";

    std::sort(filtered.begin(), filtered.end(), [](const EventInfo& a, const EventInfo& b) {
        if (a.volume != b.volume) {
            return a.volume > b.volume;
        }
        return a.liquidity > b.liquidity;
    });
    return filtered;
}

std::optional<EventInfo> PolymarketHttpClient::getEvent(const std::string& condition_id) {
    std::cout << "Fetching event: " << condition_id << "\n";
    
    std::string endpoint = "/events?condition_id=" + condition_id;
    std::string response = httpGet(endpoint);
    
    if (response.empty()) {
        return std::nullopt;
    }
    
    try {
        auto json = nlohmann::json::parse(response);
        
        if (json.is_array() && !json.empty()) {
            auto& event = json[0];
            
            EventInfo info;
            info.event_id = event.value("event_id", "");
            info.title = event.value("title", "");
            info.slug = event.value("slug", "");
            info.description = event.value("description", "");
            info.start_date = event.value("start_date", "");
            info.end_date = event.value("end_date", "");
            info.category = event.value("category", "");
            info.active = event.value("active", false);
            info.closed = event.value("closed", false);
            info.volume = std::stod(event.value("volume", "0"));
            info.liquidity = std::stod(event.value("liquidity", "0"));
            
            return info;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error parsing market: " << e.what() << "\n";
    }
    
    return std::nullopt;
}

} // namespace pmm