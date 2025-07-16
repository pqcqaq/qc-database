#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <functional>

namespace cache {
namespace monitoring {

// Metric types
enum class MetricType {
    COUNTER,      // Monotonically increasing value
    GAUGE,        // Arbitrary value that can go up and down
    HISTOGRAM,    // Distribution of values
    TIMER         // Time-based measurements
};

// Metric data point
struct MetricPoint {
    std::string name;
    MetricType type;
    double value;
    std::chrono::system_clock::time_point timestamp;
    std::unordered_map<std::string, std::string> tags;
    
    MetricPoint() : type(MetricType::GAUGE), value(0.0) {
        timestamp = std::chrono::system_clock::now();
    }
    
    MetricPoint(const std::string& n, MetricType t, double v)
        : name(n), type(t), value(v) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Hot key information
struct HotKey {
    MultiLevelKey key;
    uint64_t access_count;
    uint64_t read_count;
    uint64_t write_count;
    double access_rate;  // accesses per second
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_access;
    
    HotKey() : access_count(0), read_count(0), write_count(0), access_rate(0.0) {}
    
    HotKey(const MultiLevelKey& k) : key(k), access_count(0), read_count(0), 
                                    write_count(0), access_rate(0.0) {
        auto now = std::chrono::system_clock::now();
        first_seen = now;
        last_access = now;
    }
};

// System resource metrics
struct ResourceMetrics {
    double cpu_usage_percent;
    uint64_t memory_used_bytes;
    uint64_t memory_total_bytes;
    uint64_t disk_used_bytes;
    uint64_t disk_total_bytes;
    uint64_t network_in_bytes;
    uint64_t network_out_bytes;
    std::chrono::system_clock::time_point timestamp;
    
    ResourceMetrics() : cpu_usage_percent(0.0), memory_used_bytes(0), memory_total_bytes(0),
                       disk_used_bytes(0), disk_total_bytes(0), 
                       network_in_bytes(0), network_out_bytes(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Cache performance metrics
struct CacheMetrics {
    uint64_t total_requests;
    uint64_t successful_requests;
    uint64_t failed_requests;
    uint64_t cache_hits;
    uint64_t cache_misses;
    double hit_ratio;
    double average_latency_ms;
    double p99_latency_ms;
    uint64_t keys_count;
    uint64_t memory_usage_bytes;
    std::chrono::system_clock::time_point timestamp;
    
    CacheMetrics() : total_requests(0), successful_requests(0), failed_requests(0),
                    cache_hits(0), cache_misses(0), hit_ratio(0.0),
                    average_latency_ms(0.0), p99_latency_ms(0.0),
                    keys_count(0), memory_usage_bytes(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Cluster metrics
struct ClusterMetrics {
    size_t cluster_size;
    size_t healthy_nodes;
    size_t failed_nodes;
    bool is_leader;
    double load_balance_factor;
    size_t active_migrations;
    double rebalance_progress;
    std::chrono::system_clock::time_point timestamp;
    
    ClusterMetrics() : cluster_size(0), healthy_nodes(0), failed_nodes(0),
                      is_leader(false), load_balance_factor(1.0),
                      active_migrations(0), rebalance_progress(0.0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Alert levels
enum class AlertLevel {
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

// Alert information
struct Alert {
    std::string id;
    AlertLevel level;
    std::string title;
    std::string description;
    std::string source;
    std::chrono::system_clock::time_point triggered_at;
    bool acknowledged;
    
    Alert() : level(AlertLevel::INFO), acknowledged(false) {
        triggered_at = std::chrono::system_clock::now();
    }
    
    Alert(AlertLevel lvl, const std::string& t, const std::string& desc, const std::string& src)
        : level(lvl), title(t), description(desc), source(src), acknowledged(false) {
        triggered_at = std::chrono::system_clock::now();
        id = generate_alert_id();
    }
    
private:
    std::string generate_alert_id();
};

// Monitoring callbacks
class MonitoringCallbacks {
public:
    virtual ~MonitoringCallbacks() = default;
    
    virtual void on_alert_triggered(const Alert& alert) = 0;
    virtual void on_hot_key_detected(const HotKey& hot_key) = 0;
    virtual void on_threshold_exceeded(const std::string& metric_name, double value, double threshold) = 0;
};

// Monitor interface
class Monitor {
public:
    virtual ~Monitor() = default;
    
    // Lifecycle
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // Metric collection
    virtual void record_metric(const MetricPoint& metric) = 0;
    virtual void increment_counter(const std::string& name, double value = 1.0,
                                  const std::unordered_map<std::string, std::string>& tags = {}) = 0;
    virtual void set_gauge(const std::string& name, double value,
                          const std::unordered_map<std::string, std::string>& tags = {}) = 0;
    virtual void record_timer(const std::string& name, std::chrono::milliseconds duration,
                             const std::unordered_map<std::string, std::string>& tags = {}) = 0;
    
    // Key access tracking
    virtual void record_key_access(const MultiLevelKey& key, OperationType operation) = 0;
    virtual std::vector<HotKey> get_hot_keys(size_t limit = 100) = 0;
    virtual void reset_hot_key_stats() = 0;
    
    // System metrics
    virtual ResourceMetrics get_resource_metrics() = 0;
    virtual CacheMetrics get_cache_metrics() = 0;
    virtual ClusterMetrics get_cluster_metrics() = 0;
    
    // Historical data
    virtual std::vector<MetricPoint> get_metric_history(const std::string& metric_name,
                                                       std::chrono::system_clock::time_point start,
                                                       std::chrono::system_clock::time_point end) = 0;
    
    // Alerts
    virtual std::vector<Alert> get_active_alerts() = 0;
    virtual Result<void> acknowledge_alert(const std::string& alert_id) = 0;
    virtual Result<void> set_alert_threshold(const std::string& metric_name, double threshold,
                                            AlertLevel level = AlertLevel::WARNING) = 0;
    
    // Configuration
    virtual void set_callbacks(std::shared_ptr<MonitoringCallbacks> callbacks) = 0;
    virtual void set_hot_key_threshold(uint64_t access_count, double access_rate) = 0;
    virtual void set_metric_retention_period(std::chrono::seconds retention) = 0;
    
    // Export/Integration
    virtual std::string export_metrics_prometheus() = 0;
    virtual std::string export_metrics_json() = 0;
};

// Concrete implementation
class DefaultMonitor : public Monitor {
public:
    DefaultMonitor();
    ~DefaultMonitor() override;
    
    // Monitor interface implementation
    Result<void> start() override;
    Result<void> stop() override;
    bool is_running() const override;
    
    void record_metric(const MetricPoint& metric) override;
    void increment_counter(const std::string& name, double value = 1.0,
                          const std::unordered_map<std::string, std::string>& tags = {}) override;
    void set_gauge(const std::string& name, double value,
                  const std::unordered_map<std::string, std::string>& tags = {}) override;
    void record_timer(const std::string& name, std::chrono::milliseconds duration,
                     const std::unordered_map<std::string, std::string>& tags = {}) override;
    
    void record_key_access(const MultiLevelKey& key, OperationType operation) override;
    std::vector<HotKey> get_hot_keys(size_t limit = 100) override;
    void reset_hot_key_stats() override;
    
    ResourceMetrics get_resource_metrics() override;
    CacheMetrics get_cache_metrics() override;
    ClusterMetrics get_cluster_metrics() override;
    
    std::vector<MetricPoint> get_metric_history(const std::string& metric_name,
                                               std::chrono::system_clock::time_point start,
                                               std::chrono::system_clock::time_point end) override;
    
    std::vector<Alert> get_active_alerts() override;
    Result<void> acknowledge_alert(const std::string& alert_id) override;
    Result<void> set_alert_threshold(const std::string& metric_name, double threshold,
                                    AlertLevel level = AlertLevel::WARNING) override;
    
    void set_callbacks(std::shared_ptr<MonitoringCallbacks> callbacks) override;
    void set_hot_key_threshold(uint64_t access_count, double access_rate) override;
    void set_metric_retention_period(std::chrono::seconds retention) override;
    
    std::string export_metrics_prometheus() override;
    std::string export_metrics_json() override;
    
private:
    mutable std::shared_mutex mutex_;
    std::atomic<bool> is_running_;
    
    // Metric storage
    std::unordered_map<std::string, std::vector<MetricPoint>> metric_history_;
    std::unordered_map<std::string, MetricPoint> current_metrics_;
    
    // Hot key tracking
    std::unordered_map<MultiLevelKey, HotKey, MultiLevelKeyHash> hot_keys_;
    uint64_t hot_key_access_threshold_;
    double hot_key_rate_threshold_;
    
    // Alerts
    std::vector<Alert> active_alerts_;
    std::unordered_map<std::string, std::pair<double, AlertLevel>> alert_thresholds_;
    
    // Configuration
    std::shared_ptr<MonitoringCallbacks> callbacks_;
    std::chrono::seconds metric_retention_;
    
    // Background threads
    std::thread metrics_collector_thread_;
    std::thread hot_key_analyzer_thread_;
    std::thread alert_processor_thread_;
    std::atomic<bool> stop_threads_;
    
    // Private methods
    void metrics_collection_loop();
    void hot_key_analysis_loop();
    void alert_processing_loop();
    
    void collect_system_metrics();
    void analyze_hot_keys();
    void check_alert_thresholds();
    void cleanup_old_metrics();
    
    ResourceMetrics collect_resource_metrics();
    void trigger_alert(AlertLevel level, const std::string& title, 
                      const std::string& description, const std::string& source);
    
    std::string format_prometheus_metric(const MetricPoint& metric);
    std::string format_json_metrics();
};

// Utility functions
std::string alert_level_to_string(AlertLevel level);
AlertLevel string_to_alert_level(const std::string& str);
std::string metric_type_to_string(MetricType type);

} // namespace monitoring
} // namespace cache