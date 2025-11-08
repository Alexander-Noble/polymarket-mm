#include "network/websocket_client.hpp"
#include <iostream>
#include <stdexcept>

namespace pmm {

PolymarketWebSocketClient::PolymarketWebSocketClient(
    EventQueue& queue,
    const std::string& url
) : event_queue_(queue),
    url_(url),
    running_(false) {
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
}

void PolymarketWebSocketClient::disconnect() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "Disconnecting WebSocket...\n";
    
    running_.store(false);
    
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
        
        // Local WebSocket (not shared)
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{*ioc_, ctx};
        
        // SNI
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host_.c_str())) {
            throw beast::system_error{
                static_cast<int>(::ERR_get_error()),
                net::error::get_ssl_category()
            };
        }
        
        // Connect
        auto ep = net::connect(beast::get_lowest_layer(ws), results);
        std::cout << "TCP connected to " << ep << "\n";
        
        // SSL handshake
        ws.next_layer().handshake(ssl::stream_base::client);
        std::cout << "SSL handshake complete\n";
        
        // WebSocket handshake
        ws.handshake(host_, path_);
        std::cout << "WebSocket connected!\n";
        
        // Set options
        ws.read_message_max(64 * 1024 * 1024); // 64MB max message size
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        
        if (!subscribed_assets_.empty()) {
            // Build the subscription message
            nlohmann::json sub_msg;
            sub_msg["type"] = "MARKET";
            sub_msg["assets_ids"] = subscribed_assets_;
            
            // Convert to string
            std::string msg_str = sub_msg.dump();
            std::cout << "Sending subscription: " << msg_str << "\n";
            
            // Send via WebSocket
            ws.write(net::buffer(msg_str));
            std::cout << "Subscription sent!\n";
        }

        // Main read loop with polling
        while (running_.load()) {
            beast::flat_buffer buffer;
            beast::error_code ec;
            
            // Non-blocking read with timeout
            ws.read(buffer, ec);
            
            if (ec) {
                // Check if it's a real error or just a graceful shutdown
                if (ec != websocket::error::closed && 
                    ec != net::error::eof &&
                    ec != net::error::operation_aborted) {
                    std::cerr << "WebSocket read error: " << ec.message() << "\n";
                }
                break;
            }
            
            std::string message = beast::buffers_to_string(buffer.data());
            handleMessage(message);
        }
        
        std::cout << "WebSocket read loop exited\n";
        
        // Clean close
        if (ws.is_open()) {
            beast::error_code ec;
            ws.close(websocket::close_code::normal, ec);
            if (ec) {
                std::cerr << "Error closing WebSocket: " << ec.message() << "\n";
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "WebSocket exception: " << e.what() << "\n";
    }
    
    ioc_.reset();
    running_.store(false);
    std::cout << "WebSocket thread finished\n";
}

void PolymarketWebSocketClient::subscribe(const std::vector<std::string>& asset_ids) {
    // TODO: Send subscribe message
    subscribed_assets_ = asset_ids;
    std::cout << "Subscribed to " << asset_ids.size() << " assets\n";
}

void PolymarketWebSocketClient::handleMessage(const std::string& message) {
    try {
        auto json_msg = nlohmann::json::parse(message);
        std::cout << "Received message: " << message << "\n";

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

    std::cout << "Pushed book event for " << asset_id 
              << " (bids: " << bids.size() << ", asks: " << asks.size() << ")\n";

    auto event = Event::bookSnapshot(asset_id, std::move(bids), std::move(asks));
    event_queue_.push(std::move(event));
}

void PolymarketWebSocketClient::parsePriceChangeMessage(const nlohmann::json& msg) {
    // Implementation for parsing price change messages
    auto price_changes = msg["price_changes"]; 
    std::cout << "Received price change message with " << price_changes.size() << " changes.\n";
    for (const auto& change : price_changes) {
        std::string asset_id = change["asset_id"];
        Price price = change["price"];
        Side side = change["side"];
        Size size = change["size"];

        std::cout << "Price change for asset " << asset_id << ": " << price
                  << " (" << (side == Side::BUY ? "buy" : "sell") << ")\n";
        
        auto event = Event::priceLevelUpdate(
            asset_id,
            side == Side::BUY ? std::vector<std::pair<Price, Size>>{{price, size}} : std::vector<std::pair<Price, Size>>{},
            side == Side::SELL ? std::vector<std::pair<Price, Size>>{{price, size}} : std::vector<std::pair<Price, Size>>{}
        );
    }   
}

} // namespace pmm