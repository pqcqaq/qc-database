# Core libraries
add_library(cache_core
    cache_server.cpp
)

target_include_directories(cache_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)