
set(NAGIOS_INCLUDE "/usr/include/nagios" CACHE PATH "The include path for Nagios Headers")

set(CMAKE_CXX_STANDARD 11)

if (APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

add_library(mqexec-tng SHARED)
target_sources(mqexec-tng PRIVATE frontend.cpp backend.cpp)

target_link_libraries(mqexec-tng zmqpp-static json)
include_directories(${NAGIOS_INCLUDE})
