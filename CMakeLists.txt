cmake_minimum_required(VERSION 3.10)
project(polymarket_mm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(spdlog REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_library(pmm_core
    src/core/event_queue.cpp
    src/data/order_book.cpp
    src/strategy/strategy_engine.cpp
    src/strategy/market_maker.cpp
    src/strategy/order_manager.cpp
    src/strategy/adverse_selection.cpp
    src/network/http_client.cpp
    src/network/websocket_client.cpp
    src/utils/state_persistence.cpp
    src/utils/trading_logger.cpp
    src/utils/market_summary_logger.cpp
    src/utils/logger.cpp
)

target_link_libraries(pmm_core
    PUBLIC
        nlohmann_json::nlohmann_json
        Boost::system
        Boost::thread
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl  
        spdlog::spdlog
)

add_executable(polymarket_mm src/main.cpp)
target_link_libraries(polymarket_mm pmm_core)

add_executable(test_event_queue tests/test_event_queue.cpp)
target_link_libraries(test_event_queue PRIVATE pmm_core GTest::gtest_main)
add_test(NAME EventQueueTest COMMAND test_event_queue)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE pmm_core GTest::gtest_main)
add_test(NAME OrderBookTest COMMAND test_order_book)

add_executable(test_strategy tests/test_strategy_engine.cpp)
target_link_libraries(test_strategy PRIVATE pmm_core GTest::gtest_main)
add_test(NAME StrategyTest COMMAND test_strategy)

add_executable(test_market_maker tests/test_market_maker.cpp)
target_link_libraries(test_market_maker PRIVATE pmm_core GTest::gtest_main)
add_test(NAME MarketMakerTest COMMAND test_market_maker)

add_executable(test_order_manager tests/test_order_manager.cpp)
target_link_libraries(test_order_manager PRIVATE pmm_core GTest::gtest_main)
add_test(NAME OrderManagerTest COMMAND test_order_manager)

add_executable(test_state_persistence tests/test_state_persistence.cpp)
target_link_libraries(test_state_persistence PRIVATE pmm_core GTest::gtest_main)
add_test(NAME StatePersistenceTest COMMAND test_state_persistence)

add_executable(test_trading_logger tests/test_trading_logger.cpp)
target_link_libraries(test_trading_logger PRIVATE pmm_core GTest::gtest_main)
add_test(NAME TradingLoggerTest COMMAND test_trading_logger)

add_executable(test_quote_ttl tests/test_quote_ttl.cpp)
target_link_libraries(test_quote_ttl PRIVATE pmm_core GTest::gtest_main)
add_test(NAME QuoteTTLTest COMMAND test_quote_ttl)

add_executable(test_websocket tests/test_websocket.cpp)
target_link_libraries(test_websocket PRIVATE pmm_core)
