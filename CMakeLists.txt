cmake_minimum_required (VERSION 3.5.1)
project (MQEXEC)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

add_subdirectory(third_party)
add_subdirectory(module)
#add_subdirectory(executor)
