cmake_minimum_required(VERSION 3.10)
project(polymarket_mm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(pmm_core
    src/core/event_queue.cpp
    src/data/order_book.cpp
    src/strategy/strategy_engine.cpp
    src/strategy/market_maker.cpp
    src/strategy/order_manager.cpp  
    src/network/http_client.cpp
    src/network/websocket_client.cpp
    src/utils/trade_logger.cpp
)

target_link_libraries(pmm_core
    PUBLIC
        nlohmann_json::nlohmann_json
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl  
)

add_executable(polymarket_mm src/main.cpp)
target_link_libraries(polymarket_mm pmm_core)

add_executable(test_event_queue
    tests/test_event_queue.cpp
)

target_link_libraries(test_event_queue
    PRIVATE
        pmm_core
)

add_executable(test_order_book
    tests/test_order_book.cpp
)

target_link_libraries(test_order_book
    PRIVATE pmm_core
)


add_executable(test_websocket
    tests/test_websocket.cpp
    src/network/websocket_client.cpp
)

target_link_libraries(test_websocket
    PRIVATE pmm_core
)

add_executable(test_strategy
    tests/test_strategy_engine.cpp
)

target_link_libraries(test_strategy
    PRIVATE pmm_core
)
