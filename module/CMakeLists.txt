
set(NAGIOS_INCLUDE "/usr/include/nagios" CACHE PATH "The include path for Nagios Headers")

set(CMAKE_CXX_STANDARD 11)

add_library(mqexec-tng SHARED)
target_sources(mqexec-tng PRIVATE frontend.cpp backend.cpp)

target_link_libraries(mqexec-tng zmqpp-static json)
include_directories(${NAGIOS_INCLUDE})
