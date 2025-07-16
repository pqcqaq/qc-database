#pragma once

#include "cache/common/types.h"
#include "cache/storage/storage_engine.h"
#include "cache/cluster/cluster_manager.h"
#include "cache/network/cache_service.h"
#include "cache/security/auth_manager.h"
#include "cache/monitoring/monitor.h"
#include <memory>
#include <string>
#include <map>

namespace cache {

// Server configuration
struct ServerConfig {
    // Network settings
    std::string listen_address;
    uint16_t listen_port;
    uint32_t max_connections;
    uint32_t request_timeout_ms;
    
    // Storage settings
    std::string data_directory;
    storage::StorageEngineFactory::EngineType storage_engine_type;
    std::map<std::string, std::string> storage_config;
    
    // Cluster settings
    cluster::ClusterConfig cluster_config;
    
    // Security settings
    bool enable_authentication;
    std::string auth_token_secret;
    std::chrono::seconds token_expiry_time;
    
    // Monitoring settings
    bool enable_monitoring;
    uint16_t metrics_port;
    std::chrono::seconds metric_retention_period;
    
    // Performance settings
    size_t thread_pool_size;
    size_t max_batch_size;
    bool enable_compression;
    
    ServerConfig() 
        : listen_address("0.0.0.0"), listen_port(8080), max_connections(1000),
          request_timeout_ms(100), data_directory("./data"),
          storage_engine_type(storage::StorageEngineFactory::EngineType::LSM_TREE),
          enable_authentication(true), token_expiry_time(std::chrono::hours(24)),
          enable_monitoring(true), metrics_port(9090),
          metric_retention_period(std::chrono::hours(24)), 
          thread_pool_size(std::thread::hardware_concurrency()),
          max_batch_size(1000), enable_compression(false) {}
    
    // Load configuration from file
    static Result<ServerConfig> load_from_file(const std::string& config_file);
    
    // Save configuration to file
    Result<void> save_to_file(const std::string& config_file) const;
    
    // Validate configuration
    Result<void> validate() const;
};

// Server statistics
struct ServerStats {
    std::chrono::system_clock::time_point start_time;
    std::chrono::milliseconds uptime;
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t total_requests;
    uint64_t successful_requests;
    uint64_t failed_requests;
    double average_latency_ms;
    double p99_latency_ms;
    
    // Storage stats
    uint64_t keys_count;
    uint64_t memory_usage_bytes;
    uint64_t disk_usage_bytes;
    double cache_hit_ratio;
    
    // Cluster stats
    size_t cluster_size;
    size_t healthy_nodes;
    bool is_leader;
    double load_balance_factor;
    
    ServerStats() : uptime(0), total_connections(0), active_connections(0),
                   total_requests(0), successful_requests(0), failed_requests(0),
                   average_latency_ms(0.0), p99_latency_ms(0.0),
                   keys_count(0), memory_usage_bytes(0), disk_usage_bytes(0),
                   cache_hit_ratio(0.0), cluster_size(0), healthy_nodes(0),
                   is_leader(false), load_balance_factor(1.0) {
        start_time = std::chrono::system_clock::now();
    }
};

// Main cache server class
class CacheServer {
public:
    explicit CacheServer(const ServerConfig& config);
    ~CacheServer();
    
    // Server lifecycle
    Result<void> start();
    Result<void> stop();
    Result<void> restart();
    bool is_running() const;
    
    // Configuration management
    const ServerConfig& get_config() const { return config_; }
    Result<void> update_config(const ServerConfig& new_config);
    Result<void> reload_config(const std::string& config_file);
    
    // Statistics and monitoring
    ServerStats get_stats() const;
    std::string get_health_status() const;
    
    // Administrative operations
    Result<void> flush_all_data();
    Result<void> compact_storage();
    Result<void> backup_data(const std::string& backup_path);
    Result<void> restore_data(const std::string& backup_path);
    
    // Cluster management
    Result<void> join_cluster();
    Result<void> leave_cluster();
    Result<void> trigger_rebalance();
    std::vector<cluster::NodeInfo> get_cluster_nodes() const;
    
    // User management (admin operations)
    Result<void> create_user(const std::string& username, const std::string& password,
                            const std::unordered_set<security::Permission>& permissions);
    Result<void> delete_user(const std::string& username);
    Result<void> update_user_permissions(const std::string& username,
                                        const std::unordered_set<security::Permission>& permissions);
    
    // Monitoring and alerts
    std::vector<monitoring::Alert> get_active_alerts() const;
    std::vector<monitoring::HotKey> get_hot_keys(size_t limit = 100) const;
    std::string export_metrics(const std::string& format = "prometheus") const;
    
    // Graceful shutdown handling
    void set_shutdown_handler(std::function<void()> handler);
    
private:
    ServerConfig config_;
    std::atomic<bool> is_running_;
    std::atomic<bool> shutdown_requested_;
    
    // Core components
    std::unique_ptr<storage::StorageEngine> storage_engine_;
    std::unique_ptr<cluster::ClusterManager> cluster_manager_;
    std::unique_ptr<security::AuthManager> auth_manager_;
    std::unique_ptr<monitoring::Monitor> monitor_;
    std::unique_ptr<network::CacheService> cache_service_;
    std::unique_ptr<network::CacheServer> network_server_;
    
    // Shutdown handler
    std::function<void()> shutdown_handler_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    ServerStats stats_;
    
    // Private initialization methods
    Result<void> initialize_storage();
    Result<void> initialize_cluster();
    Result<void> initialize_security();
    Result<void> initialize_monitoring();
    Result<void> initialize_network();
    
    // Cleanup methods
    void cleanup_storage();
    void cleanup_cluster();
    void cleanup_security();
    void cleanup_monitoring();
    void cleanup_network();
    
    // Statistics collection
    void update_stats();
    void start_stats_collection();
    void stop_stats_collection();
    
    std::thread stats_thread_;
    std::atomic<bool> stop_stats_collection_;
    
    // Signal handling
    void setup_signal_handlers();
    static void signal_handler(int signal);
    static CacheServer* instance_;
};

// Utility functions for configuration parsing
namespace config {
    Result<ServerConfig> parse_config_file(const std::string& filename);
    Result<void> validate_config(const ServerConfig& config);
    std::string generate_default_config();
    
    // Environment variable handling
    std::string get_env_var(const std::string& name, const std::string& default_value = "");
    void apply_env_overrides(ServerConfig& config);
}

} // namespace cache