#pragma once

#include "cache/common/types.h"
#include "cache/monitoring/monitor.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>
#include <queue>

namespace cache {
namespace hotspot {

// 热点等级
enum class HotspotLevel {
    NORMAL,     // 正常
    WARM,       // 温热
    HOT,        // 热点
    CRITICAL    // 严重热点
};

// 热点处理策略
enum class HotspotStrategy {
    REPLICATION,    // 数据复制
    PARTITIONING,   // 数据分区
    CACHING,        // 本地缓存
    LOAD_BALANCING, // 负载均衡
    RATE_LIMITING   // 限流
};

// 访问模式
enum class AccessPattern {
    READ_HEAVY,     // 读密集
    WRITE_HEAVY,    // 写密集
    MIXED,          // 混合
    BURST,          // 突发
    PERIODIC        // 周期性
};

// 热点键信息
struct HotspotKeyInfo {
    MultiLevelKey key;
    HotspotLevel level;
    AccessPattern pattern;
    
    // 访问统计
    uint64_t total_access_count;
    uint64_t read_count;
    uint64_t write_count;
    double access_rate;  // 每秒访问次数
    
    // 时间信息
    std::chrono::system_clock::time_point first_detected;
    std::chrono::system_clock::time_point last_access;
    std::chrono::milliseconds detection_window;
    
    // 地理分布
    std::unordered_map<NodeId, uint64_t> node_access_counts;
    
    // 处理策略
    std::vector<HotspotStrategy> applied_strategies;
    
    HotspotKeyInfo() : level(HotspotLevel::NORMAL), pattern(AccessPattern::MIXED),
                      total_access_count(0), read_count(0), write_count(0), access_rate(0.0) {
        auto now = std::chrono::system_clock::now();
        first_detected = now;
        last_access = now;
        detection_window = std::chrono::milliseconds(60000); // 1分钟窗口
    }
    
    double get_read_ratio() const {
        return total_access_count > 0 ? static_cast<double>(read_count) / total_access_count : 0.0;
    }
    
    double get_write_ratio() const {
        return total_access_count > 0 ? static_cast<double>(write_count) / total_access_count : 0.0;
    }
    
    bool is_read_heavy() const {
        return get_read_ratio() > 0.8; // 80%以上是读操作
    }
    
    bool is_write_heavy() const {
        return get_write_ratio() > 0.5; // 50%以上是写操作
    }
};

// 热点检测配置
struct HotspotDetectionConfig {
    // 检测阈值
    uint64_t warm_threshold;        // 温热阈值 (访问次数/分钟)
    uint64_t hot_threshold;         // 热点阈值
    uint64_t critical_threshold;    // 严重热点阈值
    
    // 时间窗口
    std::chrono::milliseconds detection_window;     // 检测时间窗口
    std::chrono::milliseconds cooling_period;       // 冷却期
    
    // 地理分布检测
    double max_node_access_ratio;   // 单节点最大访问比例
    size_t min_nodes_for_distribution; // 分布式检测最小节点数
    
    // 模式检测
    bool enable_pattern_detection;  // 启用模式检测
    std::chrono::milliseconds burst_detection_window;  // 突发检测窗口
    double burst_multiplier;        // 突发倍数
    
    HotspotDetectionConfig() 
        : warm_threshold(1000), hot_threshold(5000), critical_threshold(20000),
          detection_window(std::chrono::milliseconds(60000)),
          cooling_period(std::chrono::milliseconds(300000)),
          max_node_access_ratio(0.7), min_nodes_for_distribution(2),
          enable_pattern_detection(true),
          burst_detection_window(std::chrono::milliseconds(10000)),
          burst_multiplier(5.0) {}
};

// 热点处理配置
struct HotspotHandlingConfig {
    // 复制策略
    size_t max_replicas;            // 最大副本数
    size_t min_replicas_for_hot;    // 热点键最小副本数
    
    // 缓存策略
    size_t local_cache_size;        // 本地缓存大小
    std::chrono::milliseconds cache_ttl; // 缓存TTL
    
    // 限流策略
    uint64_t max_qps_per_key;       // 每个键的最大QPS
    std::chrono::milliseconds rate_limit_window; // 限流窗口
    
    // 分区策略
    bool enable_key_partitioning;   // 启用键分区
    size_t max_partitions_per_key;  // 每个键的最大分区数
    
    // 自动处理
    bool auto_handle_hotspots;      // 自动处理热点
    std::vector<HotspotStrategy> preferred_strategies; // 优先策略
    
    HotspotHandlingConfig() 
        : max_replicas(5), min_replicas_for_hot(3),
          local_cache_size(10000),
          cache_ttl(std::chrono::milliseconds(300000)),
          max_qps_per_key(10000),
          rate_limit_window(std::chrono::milliseconds(1000)),
          enable_key_partitioning(true), max_partitions_per_key(4),
          auto_handle_hotspots(true) {
        preferred_strategies = {HotspotStrategy::REPLICATION, HotspotStrategy::CACHING};
    }
};

// 热点事件
struct HotspotEvent {
    enum Type {
        HOTSPOT_DETECTED,
        HOTSPOT_RESOLVED,
        STRATEGY_APPLIED,
        STRATEGY_REMOVED
    };
    
    Type type;
    MultiLevelKey key;
    HotspotLevel level;
    HotspotStrategy strategy;
    std::chrono::system_clock::time_point timestamp;
    std::string details;
    
    HotspotEvent() : type(HOTSPOT_DETECTED), level(HotspotLevel::NORMAL),
                    strategy(HotspotStrategy::REPLICATION) {
        timestamp = std::chrono::system_clock::now();
    }
};

// 热点事件处理器
class HotspotEventHandler {
public:
    virtual ~HotspotEventHandler() = default;
    
    virtual void on_hotspot_detected(const HotspotKeyInfo& hotspot) = 0;
    virtual void on_hotspot_resolved(const MultiLevelKey& key) = 0;
    virtual void on_strategy_applied(const MultiLevelKey& key, HotspotStrategy strategy) = 0;
    virtual void on_strategy_failed(const MultiLevelKey& key, HotspotStrategy strategy, 
                                   const std::string& reason) = 0;
};

// 访问记录器
class AccessRecorder {
public:
    virtual ~AccessRecorder() = default;
    
    // 记录访问
    virtual void record_access(const MultiLevelKey& key, OperationType op_type, 
                              const NodeId& node_id) = 0;
    
    // 获取访问统计
    virtual uint64_t get_access_count(const MultiLevelKey& key, 
                                     std::chrono::milliseconds window) const = 0;
    virtual double get_access_rate(const MultiLevelKey& key, 
                                  std::chrono::milliseconds window) const = 0;
    
    // 获取详细统计
    virtual HotspotKeyInfo get_key_stats(const MultiLevelKey& key) const = 0;
    virtual std::vector<MultiLevelKey> get_top_keys(size_t limit) const = 0;
    
    // 清理
    virtual void cleanup_old_records(std::chrono::milliseconds max_age) = 0;
};

// 热点处理器
class HotspotHandler {
public:
    virtual ~HotspotHandler() = default;
    
    // 应用策略
    virtual Result<void> apply_strategy(const MultiLevelKey& key, HotspotStrategy strategy) = 0;
    virtual Result<void> remove_strategy(const MultiLevelKey& key, HotspotStrategy strategy) = 0;
    
    // 查询策略状态
    virtual std::vector<HotspotStrategy> get_applied_strategies(const MultiLevelKey& key) const = 0;
    virtual bool is_strategy_applied(const MultiLevelKey& key, HotspotStrategy strategy) const = 0;
    
    // 策略效果评估
    virtual double evaluate_strategy_effectiveness(const MultiLevelKey& key, 
                                                  HotspotStrategy strategy) const = 0;
};

// 热点管理器接口
class HotspotManager {
public:
    virtual ~HotspotManager() = default;
    
    // 生命周期
    virtual Result<void> start(const HotspotDetectionConfig& detection_config,
                              const HotspotHandlingConfig& handling_config) = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // 访问记录
    virtual void record_access(const MultiLevelKey& key, OperationType op_type, 
                              const NodeId& node_id = "") = 0;
    
    // 热点检测
    virtual std::vector<HotspotKeyInfo> detect_hotspots() = 0;
    virtual std::vector<HotspotKeyInfo> get_current_hotspots() const = 0;
    virtual HotspotLevel get_hotspot_level(const MultiLevelKey& key) const = 0;
    
    // 热点处理
    virtual Result<void> handle_hotspot(const MultiLevelKey& key, 
                                       const std::vector<HotspotStrategy>& strategies = {}) = 0;
    virtual Result<void> resolve_hotspot(const MultiLevelKey& key) = 0;
    
    // 统计信息
    virtual std::vector<MultiLevelKey> get_top_accessed_keys(size_t limit = 100) const = 0;
    virtual HotspotKeyInfo get_key_info(const MultiLevelKey& key) const = 0;
    virtual std::vector<HotspotEvent> get_recent_events(size_t limit = 100) const = 0;
    
    // 配置
    virtual void set_event_handler(std::shared_ptr<HotspotEventHandler> handler) = 0;
    virtual void update_detection_config(const HotspotDetectionConfig& config) = 0;
    virtual void update_handling_config(const HotspotHandlingConfig& config) = 0;
    
    // 手动控制
    virtual Result<void> force_replicate_key(const MultiLevelKey& key, size_t replica_count) = 0;
    virtual Result<void> set_rate_limit(const MultiLevelKey& key, uint64_t max_qps) = 0;
    virtual Result<void> clear_hotspot_status(const MultiLevelKey& key) = 0;
};

// 默认访问记录器实现
class DefaultAccessRecorder : public AccessRecorder {
public:
    DefaultAccessRecorder();
    ~DefaultAccessRecorder() override;
    
    // AccessRecorder 接口实现
    void record_access(const MultiLevelKey& key, OperationType op_type, 
                      const NodeId& node_id) override;
    
    uint64_t get_access_count(const MultiLevelKey& key, 
                             std::chrono::milliseconds window) const override;
    double get_access_rate(const MultiLevelKey& key, 
                          std::chrono::milliseconds window) const override;
    
    HotspotKeyInfo get_key_stats(const MultiLevelKey& key) const override;
    std::vector<MultiLevelKey> get_top_keys(size_t limit) const override;
    
    void cleanup_old_records(std::chrono::milliseconds max_age) override;
    
private:
    // 访问记录
    struct AccessRecord {
        std::chrono::system_clock::time_point timestamp;
        OperationType op_type;
        NodeId node_id;
        
        AccessRecord() : op_type(OperationType::GET) {
            timestamp = std::chrono::system_clock::now();
        }
    };
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<MultiLevelKey, std::vector<AccessRecord>, MultiLevelKeyHash> access_records_;
    std::unordered_map<MultiLevelKey, HotspotKeyInfo, MultiLevelKeyHash> key_stats_;
    
    // 私有方法
    void update_key_stats(const MultiLevelKey& key, const AccessRecord& record);
    std::vector<AccessRecord> get_recent_accesses(const MultiLevelKey& key,
                                                 std::chrono::milliseconds window) const;
};

// 默认热点管理器实现
class DefaultHotspotManager : public HotspotManager {
public:
    DefaultHotspotManager();
    ~DefaultHotspotManager() override;
    
    // HotspotManager 接口实现
    Result<void> start(const HotspotDetectionConfig& detection_config,
                      const HotspotHandlingConfig& handling_config) override;
    Result<void> stop() override;
    bool is_running() const override;
    
    void record_access(const MultiLevelKey& key, OperationType op_type, 
                      const NodeId& node_id = "") override;
    
    std::vector<HotspotKeyInfo> detect_hotspots() override;
    std::vector<HotspotKeyInfo> get_current_hotspots() const override;
    HotspotLevel get_hotspot_level(const MultiLevelKey& key) const override;
    
    Result<void> handle_hotspot(const MultiLevelKey& key, 
                               const std::vector<HotspotStrategy>& strategies = {}) override;
    Result<void> resolve_hotspot(const MultiLevelKey& key) override;
    
    std::vector<MultiLevelKey> get_top_accessed_keys(size_t limit = 100) const override;
    HotspotKeyInfo get_key_info(const MultiLevelKey& key) const override;
    std::vector<HotspotEvent> get_recent_events(size_t limit = 100) const override;
    
    void set_event_handler(std::shared_ptr<HotspotEventHandler> handler) override;
    void update_detection_config(const HotspotDetectionConfig& config) override;
    void update_handling_config(const HotspotHandlingConfig& config) override;
    
    Result<void> force_replicate_key(const MultiLevelKey& key, size_t replica_count) override;
    Result<void> set_rate_limit(const MultiLevelKey& key, uint64_t max_qps) override;
    Result<void> clear_hotspot_status(const MultiLevelKey& key) override;
    
private:
    HotspotDetectionConfig detection_config_;
    HotspotHandlingConfig handling_config_;
    
    std::unique_ptr<AccessRecorder> access_recorder_;
    std::unique_ptr<HotspotHandler> hotspot_handler_;
    std::shared_ptr<HotspotEventHandler> event_handler_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<MultiLevelKey, HotspotKeyInfo, MultiLevelKeyHash> current_hotspots_;
    std::queue<HotspotEvent> recent_events_;
    
    std::atomic<bool> is_running_;
    
    // 后台线程
    std::thread detection_thread_;
    std::thread handling_thread_;
    std::thread cleanup_thread_;
    std::atomic<bool> stop_threads_;
    
    // 私有方法
    void detection_loop();
    void handling_loop();
    void cleanup_loop();
    
    HotspotLevel calculate_hotspot_level(const HotspotKeyInfo& key_info) const;
    AccessPattern detect_access_pattern(const HotspotKeyInfo& key_info) const;
    
    std::vector<HotspotStrategy> select_strategies(const HotspotKeyInfo& hotspot) const;
    Result<void> apply_strategy_internal(const MultiLevelKey& key, HotspotStrategy strategy);
    
    void add_event(const HotspotEvent& event);
    void notify_hotspot_detected(const HotspotKeyInfo& hotspot);
    void notify_hotspot_resolved(const MultiLevelKey& key);
    void notify_strategy_applied(const MultiLevelKey& key, HotspotStrategy strategy);
    void notify_strategy_failed(const MultiLevelKey& key, HotspotStrategy strategy,
                               const std::string& reason);
    
    // 算法实现
    bool is_geographically_concentrated(const HotspotKeyInfo& key_info) const;
    bool is_burst_pattern(const HotspotKeyInfo& key_info) const;
    double calculate_access_variance(const std::vector<uint64_t>& access_counts) const;
};

// 工具函数
namespace hotspot_utils {
    // 热点等级转换
    std::string hotspot_level_to_string(HotspotLevel level);
    HotspotLevel string_to_hotspot_level(const std::string& str);
    
    // 策略转换
    std::string strategy_to_string(HotspotStrategy strategy);
    HotspotStrategy string_to_strategy(const std::string& str);
    
    // 模式转换
    std::string pattern_to_string(AccessPattern pattern);
    AccessPattern string_to_pattern(const std::string& str);
    
    // 负载计算
    double calculate_load_balance_factor(const std::unordered_map<NodeId, uint64_t>& node_loads);
    std::vector<NodeId> find_overloaded_nodes(const std::unordered_map<NodeId, uint64_t>& node_loads,
                                             double threshold);
    
    // 统计分析
    double calculate_access_entropy(const std::vector<uint64_t>& access_counts);
    bool is_power_law_distribution(const std::vector<uint64_t>& access_counts);
}

} // namespace hotspot
} // namespace cache