cmake_minimum_required(VERSION 3.16)
project(DistributedCache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O3 -g")
endif()

# Find required packages
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Third-party libraries
find_package(Protobuf REQUIRED)

# Find gRPC using pkg-config since we installed system packages
pkg_check_modules(GRPC REQUIRED grpc)
pkg_check_modules(GRPCPP REQUIRED grpc++)

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
    ${GRPCPP_LIBRARIES}
    ${Protobuf_LIBRARIES}
)

# Add include directories for gRPC
target_include_directories(cache_server PRIVATE
    ${GRPC_INCLUDE_DIRS}
    ${GRPCPP_INCLUDE_DIRS}
)

# Install targets
install(TARGETS cache_server DESTINATION bin)