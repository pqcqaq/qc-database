# Example programs

# Basic client example
add_executable(basic_client
    basic_client.cpp
)

target_link_libraries(basic_client
    cache_network
    cache_core
)

# Benchmark tool
add_executable(benchmark
    benchmark.cpp
)

target_link_libraries(benchmark
    cache_network
    cache_core
    Threads::Threads
)

# Cluster admin tool
add_executable(cluster_admin
    cluster_admin.cpp
)

target_link_libraries(cluster_admin
    cache_network
    cache_cluster
    cache_core
)

# Performance test client
add_executable(perf_client
    performance_test.cpp
)

target_link_libraries(perf_client
    cache_network
    cache_core
    Threads::Threads
)

# Data migration tool
add_executable(data_migrator
    data_migrator.cpp
)

target_link_libraries(data_migrator
    cache_server_lib
    cache_core
)