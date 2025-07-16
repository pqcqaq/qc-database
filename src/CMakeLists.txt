# Core libraries
add_library(cache_core
    common/types.cpp
    common/utils.cpp
)

target_include_directories(cache_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Storage engine library
add_library(cache_storage
    storage/storage_engine_factory.cpp
    storage/lsm_tree.cpp
    storage/memtable.cpp
    storage/sstable.cpp
    storage/wal.cpp
)

target_link_libraries(cache_storage
    cache_core
    Threads::Threads
)

target_include_directories(cache_storage PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Cluster management library
add_library(cache_cluster
    cluster/cluster_manager.cpp
    cluster/raft.cpp
    cluster/shard_manager.cpp
    cluster/consistent_hash.cpp
)

target_link_libraries(cache_cluster
    cache_core
    Threads::Threads
)

target_include_directories(cache_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Network library
add_library(cache_network
    network/cache_service.cpp
    network/grpc_server.cpp
    network/grpc_client.cpp
    network/protocol.cpp
)

target_link_libraries(cache_network
    cache_core
    cache_storage
    cache_cluster
    cache_security
    gRPC::grpc++
    ${Protobuf_LIBRARIES}
    Threads::Threads
)

target_include_directories(cache_network PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Security library
add_library(cache_security
    security/auth_manager.cpp
    security/crypto_utils.cpp
)

target_link_libraries(cache_security
    cache_core
    Threads::Threads
)

target_include_directories(cache_security PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Monitoring library
add_library(cache_monitoring
    monitoring/monitor.cpp
    monitoring/metrics_collector.cpp
    monitoring/hot_key_detector.cpp
)

target_link_libraries(cache_monitoring
    cache_core
    Threads::Threads
)

target_include_directories(cache_monitoring PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Main server library
add_library(cache_server_lib
    cache_server.cpp
    config/config_parser.cpp
)

target_link_libraries(cache_server_lib
    cache_core
    cache_storage
    cache_cluster
    cache_network
    cache_security
    cache_monitoring
    Threads::Threads
)