#pragma once

#include "core/types.hpp"
#include "core/event_queue.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace pmm {

class PolymarketWebSocketClient {
public:
    explicit PolymarketWebSocketClient(
        EventQueue& queue,
        const std::string& url = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
    );
    
    ~PolymarketWebSocketClient();
    
    void connect();
    
    void subscribe(const std::vector<std::string>& asset_ids);
    
    void disconnect();

    bool isConnected() const {
        return running_.load();
    }
    
private:
    EventQueue& event_queue_;
    
    std::string url_;
    std::string host_;
    std::string port_;
    std::string path_;
    
    std::atomic<bool> running_;
    std::thread ws_thread_;
    
    std::vector<std::string> subscribed_assets_;
    
    // For async operations
    std::shared_ptr<net::io_context> ioc_;

    void run();
    
    void parseUrl(const std::string& url);

    void handleMessage(const std::string& message);

    void parseMessage(const nlohmann::json& json_msg);

    void parseBookMessage(const nlohmann::json& msg);
    
    void parsePriceChangeMessage(const nlohmann::json& msg);
};

} // namespace pmm