# Enable testing
enable_testing()

# Find Google Test
find_package(GTest REQUIRED)

# Test utilities
add_library(test_utils
    test_utils.cpp
)

target_link_libraries(test_utils
    cache_core
    GTest::gtest
)

# Storage engine tests
add_executable(storage_tests
    storage/test_wal.cpp
    storage/test_lsm_tree.cpp
    storage/test_memtable.cpp
)

target_link_libraries(storage_tests
    cache_storage
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Cluster tests
add_executable(cluster_tests
    cluster/test_raft.cpp
    cluster/test_shard_manager.cpp
    cluster/test_consistent_hash.cpp
)

target_link_libraries(cluster_tests
    cache_cluster
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Network tests
add_executable(network_tests
    network/test_cache_service.cpp
    network/test_protocol.cpp
)

target_link_libraries(network_tests
    cache_network
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Security tests
add_executable(security_tests
    security/test_auth_manager.cpp
)

target_link_libraries(security_tests
    cache_security
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Monitoring tests
add_executable(monitoring_tests
    monitoring/test_monitor.cpp
    monitoring/test_hot_key_detector.cpp
)

target_link_libraries(monitoring_tests
    cache_monitoring
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Integration tests
add_executable(integration_tests
    integration/test_end_to_end.cpp
    integration/test_cluster_integration.cpp
)

target_link_libraries(integration_tests
    cache_server_lib
    test_utils
    GTest::gtest
    GTest::gtest_main
)

# Register tests with CTest
add_test(NAME StorageTests COMMAND storage_tests)
add_test(NAME ClusterTests COMMAND cluster_tests)
add_test(NAME NetworkTests COMMAND network_tests)
add_test(NAME SecurityTests COMMAND security_tests)
add_test(NAME MonitoringTests COMMAND monitoring_tests)
add_test(NAME IntegrationTests COMMAND integration_tests)

# Performance tests (optional)
add_executable(performance_tests
    performance/test_throughput.cpp
    performance/test_latency.cpp
    performance/test_scalability.cpp
)

target_link_libraries(performance_tests
    cache_server_lib
    test_utils
    GTest::gtest
    GTest::gtest_main
)