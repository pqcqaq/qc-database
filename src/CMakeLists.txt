# Core libraries
add_library(cache_core
    cache_server.cpp
    crc32c.cpp
    storage/lsm_tree.cpp
    storage/memtable.cpp
    storage/sstable.cpp
    storage/storage_engine_factory.cpp
    storage/wal.cpp
    cluster/split_brain_detector.cpp
    transaction/transaction_manager.cpp
    time/time_sync.cpp
    hotspot/hotspot_manager.cpp
)

target_include_directories(cache_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Link dependencies for cache_core
target_link_libraries(cache_core
    ${GRPC_LIBRARIES}
    ${Protobuf_LIBRARIES}
    Threads::Threads
)

# Add include directories for gRPC and protobuf
target_include_directories(cache_core PRIVATE
    ${GRPC_INCLUDE_DIRS}
    ${Protobuf_INCLUDE_DIRS}
)