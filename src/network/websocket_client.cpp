#include "network/websocket_client.hpp"

namespace pmm {

    PolymarketWebSocketClient::PolymarketWebSocketClient(
        EventQueue& queue,
        const std::string& url
    ) : event_queue_(queue),
        url_(url),
        running_(false) {
        parseUrl(url_);
    }

    PolymarketWebSocketClient::~PolymarketWebSocketClient() {
        disconnect();
    }

    void parseUrl(const std::string& url) {
        url_ = url;
 
        size_t protocol_len = 0;
        if (url.find("wss://") == 0) {
            protocol_len = 6;
            port_ = "443";
        } else if (url.find("ws://") == 0) {
            protocol_len = 5;
            port_ = "80";
        } else {
            throw std::runtime_error("URL must start with ws:// or wss://");
        }
        
        size_t path_start = url.find('/', protocol_len);
        
        if (path_start == std::string::npos) {
            host_ = url.substr(protocol_len);
            path_ = "/";
        } else {
            host_ = url.substr(protocol_len, path_start - protocol_len);
            path_ = url.substr(path_start);
        }
    }

} // namespace pmm