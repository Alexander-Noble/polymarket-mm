#include "network/websocket_client.hpp"
#include "utils/logger.hpp"
#include <iostream>
#include <stdexcept>

namespace pmm {

PolymarketWebSocketClient::PolymarketWebSocketClient(
    EventQueue& queue,
    const std::string& url
) : event_queue_(queue),
    url_(url) {
    parseUrl(url_);
    LOG_INFO("WebSocket Client initialized with URL: {}", url_);
    LOG_INFO("Host: {}, Port: {}, Path: {}", host_, port_, path_);
}

PolymarketWebSocketClient::~PolymarketWebSocketClient() {
    disconnect();
}

void PolymarketWebSocketClient::parseUrl(const std::string& url) {
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

void PolymarketWebSocketClient::connect() {
    if (running_.load()) {
        LOG_INFO("WebSocket Client is already connected.");
        return;
    }

    running_ = true;
    ws_thread_ = std::thread(&PolymarketWebSocketClient::run, this);
    LOG_INFO("WebSocket Client connecting to {}", url_);

    int attempts = 0;
    while (!connected_.load() && attempts < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        attempts++;
    }
    
    if (connected_.load()) {
        LOG_INFO("WebSocket connected successfully");
    } else {
        LOG_WARN("WebSocket connection delayed (may still be connecting)");
    }
}

void PolymarketWebSocketClient::disconnect() {
    if (!running_.load()) {
        return;
    }
    
    LOG_INFO("Disconnecting WebSocket...");
    
    running_.store(false);
    connected_.store(false);

    if (ioc_) {
        ioc_->stop();
    }

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
    
    LOG_INFO("WebSocket disconnected");
}

void PolymarketWebSocketClient::run() {
    try {
        LOG_INFO("Connecting to {}:{}{}", host_, port_, path_);
        
        ioc_ = std::make_shared<net::io_context>();
        ssl::context ctx{ssl::context::tlsv12_client};
        
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_none);
        
        tcp::resolver resolver{*ioc_};
        auto const results = resolver.resolve(host_, port_);
        
        ws_ = std::make_shared<websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, ctx);

        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
            throw beast::system_error{
                static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()
            };
        }
        
        // Connect
        auto ep = net::connect(beast::get_lowest_layer(*ws_), results);
        LOG_DEBUG("TCP connected");
        
        // SSL handshake
        ws_->next_layer().handshake(ssl::stream_base::client);
        LOG_DEBUG("SSL handshake complete");
        
        // WebSocket handshake
        ws_->handshake(host_, path_);
        LOG_DEBUG("WebSocket connected");
        
        // Set options
        ws_->read_message_max(64 * 1024 * 1024);
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        
        connected_.store(true);
        if (!subscribed_assets_.empty()) {
            sendSubscription();
        }

        // Create buffer for async reads
        buffer_ = std::make_shared<beast::flat_buffer>();
        
        // Start async read
        startAsyncRead();
        
        // Start ping timer
        startPingTimer();
        
        // Run the io_context
        LOG_DEBUG("Starting io_context event loop");
        ioc_->run();
        
        LOG_DEBUG("WebSocket read loop exited");
        
        // Clean close
        if (ws_ && ws_->is_open()) {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
            if (ec) {
                LOG_ERROR("Error closing WebSocket: {}", ec.message());
            }
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("WebSocket exception: {}", e.what());
    }
    
    connected_.store(false);
    running_.store(false);
    LOG_INFO("WebSocket thread finished");
}

void PolymarketWebSocketClient::startAsyncRead() {
    if (!running_.load() || !ws_ || !buffer_) return;
    
    ws_->async_read(*buffer_,
        [this](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                if (ec == websocket::error::closed) {
                    handleDisconnection("Connection closed by remote");
                } else if (ec != net::error::operation_aborted) {
                    handleDisconnection(ec.message());
                } else {
                    ioc_->stop();
                }
                return;
            }
            
            std::string message = beast::buffers_to_string(buffer_->data());
            handleMessage(message);
            buffer_->consume(buffer_->size());
            
            // Continue reading
            startAsyncRead();
        });
}

void PolymarketWebSocketClient::startPingTimer() {
    if (!running_.load() || !ioc_) return;
    
    auto timer = std::make_shared<net::steady_timer>(*ioc_, std::chrono::seconds(5));
    
    timer->async_wait([this, timer](beast::error_code ec) {
        if (ec || !running_.load() || !ws_ || !ws_->is_open()) {
            return;
        }
        
        // Send ping
        ws_->async_ping({}, [](beast::error_code ec) {
            if (ec) {
                LOG_ERROR("Ping error: {}", ec.message());
            }
        });
        
        // Schedule next ping
        startPingTimer();
    });
}


void PolymarketWebSocketClient::subscribe(const std::vector<std::string>& asset_ids) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    
    subscribed_assets_ = asset_ids;
    
    LOG_INFO("Subscribing to {} tokens", asset_ids.size());
    LOG_DEBUG("=== SUBSCRIPTION REQUEST ===");
    for (size_t i = 0; i < asset_ids.size(); i++) {
        LOG_DEBUG("  [{}] Token: {}", i, asset_ids[i]);
    }
    LOG_DEBUG("===========================");
    
    LOG_DEBUG("Connected: {}", connected_.load());
    LOG_DEBUG("WebSocket is {}", ws_->is_open() ? "open" : "closed");
    LOG_DEBUG("IO context is {}", ioc_ ? "valid" : "null");
    // if (connected_.load() && ws_) {
    //     if (ioc_) {
    //         net::post(*ioc_, [this]() {
    //             sendSubscription();
    //         });
    //     }
    // }
    if (connected_.load() && ws_ && ws_->is_open()) {
        LOG_DEBUG("WebSocket is connected, sending subscription now");
        sendSubscription();
    } else {
        LOG_DEBUG("WebSocket not ready, subscription will be sent when connected");
    }
}

void PolymarketWebSocketClient::sendSubscription() {
    LOG_INFO("Sending subscription for {} assets...", subscribed_assets_.size());
    if (subscribed_assets_.empty()) {
        LOG_DEBUG("No assets to subscribe to");
        return;
    }
    
    if (!ws_ || !ws_->is_open()) {
        LOG_DEBUG("WebSocket not ready for subscription");
        return;
    }
    
    try {       
        nlohmann::json sub_msg;
        sub_msg["type"] = "market";
        sub_msg["assets_ids"] = subscribed_assets_;
        
        std::string msg_str = sub_msg.dump();
        LOG_DEBUG("Sending subscription: {}", msg_str);
        
        ws_->write(net::buffer(msg_str));
        LOG_DEBUG("Subscription sent for {} assets", subscribed_assets_.size());
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error sending subscription: {}", e.what());
    }
}

void PolymarketWebSocketClient::handleMessage(const std::string& message) {
    try {
        auto json_msg = nlohmann::json::parse(message);
        if (json_msg.is_array()) {
            for (const auto& item : json_msg) {
                parseMessage(item);
            }
            return;
        } else {
            parseMessage(json_msg);
            return;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing message: {}", e.what());
    }
}

void PolymarketWebSocketClient::parseMessage(const nlohmann::json& json_msg) {
    if (json_msg.find("event_type") == json_msg.end()) {
        LOG_DEBUG("Unknown message format, missing event_type\n{}", json_msg.dump());
        return;
    }
    std::string event_type = json_msg["event_type"];
    
    if (event_type == "book") {
        parseBookMessage(json_msg);
    } else if (event_type == "price_change") {
        parsePriceChangeMessage(json_msg);
    } else {
        LOG_DEBUG("Received {} message", event_type);
    }
}

void PolymarketWebSocketClient::parseBookMessage(const nlohmann::json& msg) {
    std::string asset_id = msg["asset_id"];
    
    // Check if this token was in our subscription
    bool is_subscribed = false;
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        is_subscribed = std::find(subscribed_assets_.begin(), subscribed_assets_.end(), asset_id) != subscribed_assets_.end();
    }
    
    if (!is_subscribed) {
        LOG_DEBUG("[WS RECV] Book message for unsubscribed token: {}... (Polymarket sends both sides)", asset_id.substr(0, 16));
    } else {
        LOG_DEBUG("[WS RECV] Book message for subscribed token: {}...{}", asset_id.substr(0, 8), asset_id.substr(asset_id.length()-8));
    }
    
    std::vector<std::pair<Price, Size>> bids;
    std::vector<std::pair<Price, Size>> asks;
    
    for (const auto& bid : msg["bids"]) {
        Price price = std::stod(bid["price"].get<std::string>());
        Size size = std::stod(bid["size"].get<std::string>());
        bids.push_back({price, size});
    }
    
    for (const auto& ask : msg["asks"]) {
        Price price = std::stod(ask["price"].get<std::string>());
        Size size = std::stod(ask["size"].get<std::string>());
        asks.push_back({price, size});
    }

    LOG_DEBUG("Pushed book event for {} (bids: {}, asks: {})", asset_id.substr(0, 8), bids.size(), asks.size());
    LOG_DEBUG("[WS RECV] Book snapshot for token: {}", asset_id);

    auto event = Event::bookSnapshot(asset_id, std::move(bids), std::move(asks));
    event_queue_.push(std::move(event));
}

void PolymarketWebSocketClient::parsePriceChangeMessage(const nlohmann::json& msg) {
    auto price_changes = msg["price_changes"]; 
    LOG_DEBUG("[WS RECV] Price change message with {} changes.", price_changes.size());
    
    for (const auto& change : price_changes) {
        std::string asset_id = change["asset_id"];
        
        // Check if this token was in our subscription (Polymarket sends both Yes/No even if we only subscribed to one)
        bool is_subscribed = false;
        {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            is_subscribed = std::find(subscribed_assets_.begin(), subscribed_assets_.end(), asset_id) != subscribed_assets_.end();
        }
        
        if (!is_subscribed) {
            LOG_DEBUG("[WS RECV] Price change for unsubscribed token (other side): {}...", asset_id.substr(0, 16));
        }

        Price price = std::stod(change["price"].get<std::string>());
        Size size = std::stod(change["size"].get<std::string>());

        std::string side_str = change["side"];
        Side side = (side_str == "BUY") ? Side::BUY : Side::SELL;

        LOG_DEBUG("  -> {} x {} ({})", price, size, side_str);

        std::vector<std::pair<Price, Size>> bids;
        std::vector<std::pair<Price, Size>> asks;
        
        if (side == Side::BUY) {
            bids.push_back({price, size});
        } else {
            asks.push_back({price, size});
        }
        
        auto event = Event::priceLevelUpdate(asset_id, std::move(bids), std::move(asks));
        event_queue_.push(std::move(event));
    }   
}

void PolymarketWebSocketClient::setReconnectConfig(int max_attempts, std::chrono::seconds backoff) {
    max_reconnect_attempts_ = max_attempts;
    reconnect_backoff_ = backoff;
}

void PolymarketWebSocketClient::handleDisconnection(const std::string& reason) {
    LOG_WARN("WebSocket disconnected: {}", reason);
    
    // Reset connection state
    connected_.store(false);
    
    if (!running_.load()) {
        LOG_INFO("Shutdown requested, not reconnecting");
        return;
    }
    
    attemptReconnect();
}


void PolymarketWebSocketClient::attemptReconnect() {
    reconnect_attempt_++;
    
    if (reconnect_attempt_ > max_reconnect_attempts_) {
        LOG_ERROR("Max reconnection attempts ({}) exceeded", max_reconnect_attempts_);
        event_queue_.push(Event::shutdown("WebSocket reconnection failed"));
        return;
    }
    
    auto delay = reconnect_backoff_ * reconnect_attempt_;
    LOG_INFO("Reconnecting in {}s (attempt {}/{})", delay.count(), reconnect_attempt_, max_reconnect_attempts_);
    
    std::this_thread::sleep_for(delay);
    
    try {
        // Properly clean up the old connection first
        running_.store(false);
        connected_.store(false);
        
        // Stop the io_context to exit the run loop
        if (ioc_) {
            ioc_->stop();
        }
        
        // Wait for the thread to finish
        if (ws_thread_.joinable()) {
            ws_thread_.join();
        }
        
        // Clean up old resources
        buffer_.reset();
        ws_.reset();
        ioc_.reset();
        
        // Now attempt to reconnect
        connect();
        reconnect_attempt_ = 0;
        
        LOG_INFO("Reconnection successful");
        
        if (!subscribed_assets_.empty()) {
            LOG_INFO("Re-subscribing to {} markets after reconnection", subscribed_assets_.size());
            sendSubscription();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Reconnection failed: {}", e.what());
        attemptReconnect();
    }
}

} // namespace pmm