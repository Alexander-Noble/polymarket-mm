#pragma once

#include <string>
#include <vector>
#include <optional>
#include "core/types.hpp"

namespace pmm {

class PolymarketHttpClient {
public:
    PolymarketHttpClient();
    
    std::vector<EventInfo> getActiveEvents(int limit = 100);
    
    std::vector<EventInfo> searchEvents(const std::string& query);
    
    std::optional<EventInfo> getEvent(const std::string& condition_id);
    
private:
    std::string api_base_url_;
    
    std::string httpGet(const std::string& endpoint);

    std::vector<EventInfo> parseBatch(const std::string& response);
};

} // namespace pmm