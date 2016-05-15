cmake_minimum_required(VERSION 3.0)

project (json LANGUAGES CXX)

add_library(json INTERFACE)
target_include_directories(json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
