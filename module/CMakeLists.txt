
set(NAGIOS_INCLUDE "/usr/include/nagios" CACHE PATH "The include path for Nagios Headers")

set(CMAKE_CXX_STANDARD 11)

if (APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

add_library(modmqexec-tng SHARED)
target_sources(modmqexec-tng PRIVATE frontend.cpp backend.cpp)

target_link_libraries(modmqexec-tng zmqpp-static cereal shared json ${CMAKE_THREAD_LIBS_INIT})
include_directories(${NAGIOS_INCLUDE})
