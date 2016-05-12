
set(CMAKE_CXX_STANDARD 11)

add_executable(mqexec-tng executor.cpp)
target_link_libraries(mqexec-tng zmqpp-static cereal shared json)

add_executable(harness harness.cpp)
target_link_libraries(harness zmqpp-static cereal shared json)
