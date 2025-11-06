cmake_minimum_required(VERSION 3.10)
project(polymarket_mm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(pmm_core
    src/core/event_queue.cpp
)

target_link_libraries(pmm_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

# Build the test executable
add_executable(test_event_queue
    tests/test_event_queue.cpp
)

# Link our library to the test
target_link_libraries(test_event_queue
    PRIVATE
        pmm_core
)

add_executable(test_order_book
    tests/test_order_book.cpp
    src/data/order_book.cpp
)

target_link_libraries(test_order_book
    PRIVATE pmm_core
)