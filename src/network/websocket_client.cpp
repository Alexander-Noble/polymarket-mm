#include "network/websocket_client.hpp"
#include <iostream>
#include <stdexcept>

namespace pmm {

PolymarketWebSocketClient::PolymarketWebSocketClient(
    EventQueue& queue,
    const std::string& url
) : event_queue_(queue),
    url_(url) {
    parseUrl(url_);
    std::cout << "WebSocket Client initialized with URL: " << url_ << std::endl;
    std::cout << "Host: " << host_ << ", Port: " << port_ << ", Path: " << path_ << std::endl;
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
        std::cout << "WebSocket Client is already connected." << std::endl;
        return;
    }

    running_ = true;
    ws_thread_ = std::thread(&PolymarketWebSocketClient::run, this);
    std::cout << "WebSocket Client connecting to " << url_ << std::endl;

    int attempts = 0;
    while (!connected_.load() && attempts < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        attempts++;
    }
    
    if (connected_.load()) {
        std::cout << "WebSocket connected successfully\n";
    } else {
        std::cout << "WebSocket connection delayed (may still be connecting)\n";
    }
}

void PolymarketWebSocketClient::disconnect() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "Disconnecting WebSocket...\n";
    
    running_.store(false);
    connected_.store(false);

    if (ioc_) {
        ioc_->stop();
    }

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
    
    std::cout << "WebSocket disconnected\n";
}

void PolymarketWebSocketClient::run() {
    try {
        std::cout << "Connecting to " << host_ << ":" << port_ << path_ << "\n";
        
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
        std::cout << "TCP connected to " << ep << "\n";
        
        // SSL handshake
        ws_->next_layer().handshake(ssl::stream_base::client);
        std::cout << "SSL handshake complete\n";
        
        // WebSocket handshake
        ws_->handshake(host_, path_);
        std::cout << "WebSocket connected!\n";
        
        // Set options
        ws_->read_message_max(64 * 1024 * 1024);
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        
        connected_.store(true);
        if (!subscribed_assets_.empty()) {
            sendSubscription();
        }

        // Start async read
        beast::flat_buffer buffer;
        startAsyncRead(buffer);
        
        // Start ping timer
        startPingTimer();
        
        // Run the io_context
        std::cout << "Starting io_context event loop\n";
        ioc_->run();
        
        std::cout << "WebSocket read loop exited\n";
        
        // Clean close
        if (ws_ && ws_->is_open()) {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
            if (ec) {
                std::cerr << "Error closing WebSocket: " << ec.message() << "\n";
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "WebSocket exception: " << e.what() << "\n";
    }
    
    connected_.store(false);
    running_.store(false);
    std::cout << "WebSocket thread finished\n";
}

void PolymarketWebSocketClient::startAsyncRead(beast::flat_buffer& buffer) {
    if (!running_.load() || !ws_) return;
    
    ws_->async_read(buffer,
        [this, &buffer](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                if (ec != websocket::error::closed && 
                    ec != net::error::operation_aborted) {
                    std::cerr << "WebSocket read error: " << ec.message() << "\n";
                }
                ioc_->stop();
                return;
            }
            
            std::string message = beast::buffers_to_string(buffer.data());
            handleMessage(message);
            buffer.consume(buffer.size());
            
            // Continue reading
            startAsyncRead(buffer);
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
                std::cerr << "Ping error: " << ec.message() << "\n";
            }
        });
        
        // Schedule next ping
        startPingTimer();
    });
}


void PolymarketWebSocketClient::subscribe(const std::vector<std::string>& asset_ids) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    
    subscribed_assets_ = asset_ids;
    
    std::cout << "Queued subscription for " << asset_ids.size() << " assets\n";
    std::cout << connected_.load() << "\n";
    std::cout << (ws_->is_open() ? "ws_ is valid\n" : "ws_ is null\n");
    std::cout << (ioc_ ? "ioc_ is valid\n" : "ioc_ is null\n");
    // if (connected_.load() && ws_) {
    //     if (ioc_) {
    //         net::post(*ioc_, [this]() {
    //             sendSubscription();
    //         });
    //     }
    // }
    if (connected_.load() && ws_ && ws_->is_open()) {
        std::cout << "WebSocket is connected, sending subscription now\n";
        sendSubscription();
    } else {
        std::cout << "WebSocket not ready, subscription will be sent when connected\n";
    }
}

void PolymarketWebSocketClient::sendSubscription() {
    std::cout << "Sending subscription for " << subscribed_assets_.size() << " assets...\n";
    if (subscribed_assets_.empty()) {
        std::cout << "No assets to subscribe to\n";
        return;
    }
    
    if (!ws_ || !ws_->is_open()) {
        std::cout << "WebSocket not ready for subscription\n";
        return;
    }
    
    try {       
        nlohmann::json sub_msg;
        sub_msg["type"] = "market";
        sub_msg["assets_ids"] = subscribed_assets_;
        
        std::string msg_str = sub_msg.dump();
        std::cout << "Sending subscription: " << msg_str << "\n";
        
        ws_->write(net::buffer(msg_str));
        std::cout << "Subscription sent for " << subscribed_assets_.size() << " assets\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error sending subscription: " << e.what() << "\n";
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
        std::cerr << "Error parsing message: " << e.what() << "\n";
    }
}

void PolymarketWebSocketClient::parseMessage(const nlohmann::json& json_msg) {
    if (json_msg.find("event_type") == json_msg.end()) {
        std::cout << "Unknown message format, missing event_type\n" << json_msg.dump();
        return;
    }
    std::string event_type = json_msg["event_type"];
    
    if (event_type == "book") {
        parseBookMessage(json_msg);
    } else if (event_type == "price_change") {
        parsePriceChangeMessage(json_msg);
    } else {
        std::cout << "Received " << event_type << " message\n";
    }
}

void PolymarketWebSocketClient::parseBookMessage(const nlohmann::json& msg) {
    std::string asset_id = msg["asset_id"];
    
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

    std::cout << "Pushed book event for " << asset_id.substr(0, 8)
              << " (bids: " << bids.size() << ", asks: " << asks.size() << ")\n";

    auto event = Event::bookSnapshot(asset_id, std::move(bids), std::move(asks));
    event_queue_.push(std::move(event));
}

void PolymarketWebSocketClient::parsePriceChangeMessage(const nlohmann::json& msg) {
    auto price_changes = msg["price_changes"]; 
    std::cout << "Received price change message with " << price_changes.size() << " changes.\n";
    
    for (const auto& change : price_changes) {
        std::string asset_id = change["asset_id"];

        Price price = std::stod(change["price"].get<std::string>());
        Size size = std::stod(change["size"].get<std::string>());

        std::string side_str = change["side"];
        Side side = (side_str == "BUY") ? Side::BUY : Side::SELL;

        std::cout << "Price change for asset " << asset_id << ": " << price
                  << " x " << size << " (" << side_str << ")\n";

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

} // namespace pmm