cmake_minimum_required(VERSION 3.0)

project (json LANGUAGES CXX)

add_library(json INTERFACE)
#target_sources(json INTERFACE include/json.hpp)
target_include_directories(json INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
