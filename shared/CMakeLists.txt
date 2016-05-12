add_library(shared INTERFACE)
target_include_directories(shared INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
