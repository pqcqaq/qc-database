cmake_minimum_required(VERSION 3.16)
project(DistributedCache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -g")
endif()

# Find required packages
find_package(Threads REQUIRED)

# Try to find Protobuf and gRPC using find_package first
find_package(Protobuf QUIET)
find_package(gRPC QUIET)

# If not found, try to use system libraries directly
if(NOT Protobuf_FOUND)
    set(Protobuf_LIBRARIES protobuf)
    set(Protobuf_INCLUDE_DIRS /usr/include)
endif()

if(NOT gRPC_FOUND)
    set(GRPC_LIBRARIES grpc grpc++)
    set(GRPC_INCLUDE_DIRS /usr/include)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src)

# Add subdirectories
add_subdirectory(src)

# Main executable
add_executable(cache_server
    src/main.cpp
)

target_link_libraries(cache_server
    cache_core
    Threads::Threads
    ${GRPC_LIBRARIES}
    ${Protobuf_LIBRARIES}
)

# Add include directories
target_include_directories(cache_server PRIVATE
    ${GRPC_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)

# Install targets
install(TARGETS cache_server DESTINATION bin)