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
    void disconnect();

    void subscribe(const std::vector<std::string>& asset_ids);

    bool isConnected() const {
        return running_.load();
    }
    
    void setReconnectConfig(int max_attempts = 5, std::chrono::seconds backoff = std::chrono::seconds(5));

private:
    EventQueue& event_queue_;
    
    std::string url_;
    std::string host_;
    std::string port_;
    std::string path_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread ws_thread_;
    
    std::shared_ptr<net::io_context> ioc_;
    std::shared_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    std::shared_ptr<beast::flat_buffer> buffer_;
    
    std::vector<std::string> subscribed_assets_;
    std::mutex subscription_mutex_;
    
    int max_reconnect_attempts_ = 5;
    std::chrono::seconds reconnect_backoff_{5};
    int reconnect_attempt_ = 0;

    void run();
    void sendSubscription();
    void parseUrl(const std::string& url);
    void handleMessage(const std::string& message);
    void parseMessage(const nlohmann::json& json_msg);
    void parseBookMessage(const nlohmann::json& msg);
    void parsePriceChangeMessage(const nlohmann::json& msg);

    void startAsyncRead();
    void startPingTimer();

    void attemptReconnect();
    void handleDisconnection(const std::string& reason);
};

} // namespace pmm