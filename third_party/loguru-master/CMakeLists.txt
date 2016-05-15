cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 11)

project (loguru LANGUAGES CXX)
add_library(loguru STATIC loguru-impl.cpp)
target_include_directories(loguru INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(loguru dl)
