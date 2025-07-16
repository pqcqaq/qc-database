#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>

namespace cache {
namespace cluster {

// 脑裂检测状态
enum class SplitBrainStatus {
    HEALTHY,           // 集群健康
    SUSPECTED,         // 疑似脑裂
    CONFIRMED,         // 确认脑裂
    RECOVERING,        // 恢复中
    ISOLATED           // 节点被隔离
};

// 仲裁策略
enum class QuorumStrategy {
    MAJORITY,          // 多数派策略
    STATIC_QUORUM,     // 静态仲裁
    DYNAMIC_QUORUM,    // 动态仲裁
    WITNESS_BASED      // 见证者策略
};

// 网络分区信息
struct NetworkPartition {
    std::vector<NodeId> partition_a;
    std::vector<NodeId> partition_b;
    std::chrono::system_clock::time_point detected_at;
    std::chrono::milliseconds duration;
    bool resolved;
    
    NetworkPartition() : resolved(false) {
        detected_at = std::chrono::system_clock::now();
    }
};

// 仲裁结果
struct QuorumDecision {
    bool has_quorum;
    std::vector<NodeId> active_nodes;
    std::vector<NodeId> isolated_nodes;
    QuorumStrategy strategy_used;
    std::string reason;
    
    QuorumDecision() : has_quorum(false), strategy_used(QuorumStrategy::MAJORITY) {}
};

// 脑裂检测配置
struct SplitBrainConfig {
    // 检测参数
    std::chrono::milliseconds heartbeat_timeout;
    std::chrono::milliseconds split_detection_timeout;
    uint32_t max_missed_heartbeats;
    
    // 仲裁配置
    QuorumStrategy quorum_strategy;
    size_t min_quorum_size;
    std::vector<NodeId> witness_nodes;
    
    // 恢复策略
    bool auto_recovery_enabled;
    std::chrono::milliseconds recovery_timeout;
    uint32_t max_recovery_attempts;
    
    SplitBrainConfig() 
        : heartbeat_timeout(std::chrono::milliseconds(1000)),
          split_detection_timeout(std::chrono::milliseconds(5000)),
          max_missed_heartbeats(3),
          quorum_strategy(QuorumStrategy::MAJORITY),
          min_quorum_size(2),
          auto_recovery_enabled(true),
          recovery_timeout(std::chrono::milliseconds(30000)),
          max_recovery_attempts(3) {}
};

// 脑裂事件回调
class SplitBrainEventHandler {
public:
    virtual ~SplitBrainEventHandler() = default;
    
    virtual void on_split_brain_detected(const NetworkPartition& partition) = 0;
    virtual void on_split_brain_resolved(const NetworkPartition& partition) = 0;
    virtual void on_node_isolated(const NodeId& node_id, const std::string& reason) = 0;
    virtual void on_quorum_lost() = 0;
    virtual void on_quorum_restored() = 0;
};

// 脑裂检测器接口
class SplitBrainDetector {
public:
    virtual ~SplitBrainDetector() = default;
    
    // 生命周期
    virtual Result<void> start(const SplitBrainConfig& config) = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // 节点管理
    virtual void add_node(const NodeId& node_id) = 0;
    virtual void remove_node(const NodeId& node_id) = 0;
    virtual void update_node_status(const NodeId& node_id, bool is_alive, 
                                   std::chrono::system_clock::time_point last_seen) = 0;
    
    // 心跳处理
    virtual void process_heartbeat(const NodeId& from_node, const NodeId& to_node,
                                  std::chrono::system_clock::time_point timestamp) = 0;
    
    // 状态查询
    virtual SplitBrainStatus get_status() const = 0;
    virtual QuorumDecision check_quorum() const = 0;
    virtual std::vector<NetworkPartition> get_detected_partitions() const = 0;
    virtual bool has_quorum() const = 0;
    virtual bool is_node_isolated(const NodeId& node_id) const = 0;
    
    // 恢复操作
    virtual Result<void> force_merge_partitions() = 0;
    virtual Result<void> isolate_node(const NodeId& node_id, const std::string& reason) = 0;
    virtual Result<void> restore_node(const NodeId& node_id) = 0;
    
    // 事件处理
    virtual void set_event_handler(std::shared_ptr<SplitBrainEventHandler> handler) = 0;
    
    // 配置更新
    virtual void update_config(const SplitBrainConfig& config) = 0;
    virtual SplitBrainConfig get_config() const = 0;
};

// 默认脑裂检测器实现
class DefaultSplitBrainDetector : public SplitBrainDetector {
public:
    DefaultSplitBrainDetector();
    ~DefaultSplitBrainDetector() override;
    
    // SplitBrainDetector 接口实现
    Result<void> start(const SplitBrainConfig& config) override;
    Result<void> stop() override;
    bool is_running() const override;
    
    void add_node(const NodeId& node_id) override;
    void remove_node(const NodeId& node_id) override;
    void update_node_status(const NodeId& node_id, bool is_alive,
                           std::chrono::system_clock::time_point last_seen) override;
    
    void process_heartbeat(const NodeId& from_node, const NodeId& to_node,
                          std::chrono::system_clock::time_point timestamp) override;
    
    SplitBrainStatus get_status() const override;
    QuorumDecision check_quorum() const override;
    std::vector<NetworkPartition> get_detected_partitions() const override;
    bool has_quorum() const override;
    bool is_node_isolated(const NodeId& node_id) const override;
    
    Result<void> force_merge_partitions() override;
    Result<void> isolate_node(const NodeId& node_id, const std::string& reason) override;
    Result<void> restore_node(const NodeId& node_id) override;
    
    void set_event_handler(std::shared_ptr<SplitBrainEventHandler> handler) override;
    
    void update_config(const SplitBrainConfig& config) override;
    SplitBrainConfig get_config() const override;
    
private:
    // 节点状态信息
    struct NodeStatus {
        NodeId node_id;
        bool is_alive;
        std::chrono::system_clock::time_point last_seen;
        std::chrono::system_clock::time_point last_heartbeat;
        uint32_t missed_heartbeats;
        bool is_isolated;
        std::string isolation_reason;
        
        NodeStatus() : is_alive(false), missed_heartbeats(0), is_isolated(false) {}
        NodeStatus(const NodeId& id) : node_id(id), is_alive(false), 
                                      missed_heartbeats(0), is_isolated(false) {}
    };
    
    SplitBrainConfig config_;
    std::shared_ptr<SplitBrainEventHandler> event_handler_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<NodeId, NodeStatus> nodes_;
    std::vector<NetworkPartition> detected_partitions_;
    std::unordered_set<NodeId> isolated_nodes_;
    
    std::atomic<SplitBrainStatus> status_;
    std::atomic<bool> is_running_;
    
    // 后台线程
    std::thread detection_thread_;
    std::thread recovery_thread_;
    std::atomic<bool> stop_threads_;
    
    // 私有方法
    void detection_loop();
    void recovery_loop();
    
    void detect_network_partitions();
    QuorumDecision calculate_quorum() const;
    bool detect_split_brain();
    void handle_partition_detected(const NetworkPartition& partition);
    void handle_partition_resolved(const NetworkPartition& partition);
    
    // 仲裁策略实现
    QuorumDecision majority_quorum() const;
    QuorumDecision static_quorum() const;
    QuorumDecision dynamic_quorum() const;
    QuorumDecision witness_based_quorum() const;
    
    // 工具方法
    std::vector<std::vector<NodeId>> find_connected_components() const;
    bool are_nodes_connected(const NodeId& node1, const NodeId& node2) const;
    void update_partition_status();
    
    // 事件通知
    void notify_split_brain_detected(const NetworkPartition& partition);
    void notify_split_brain_resolved(const NetworkPartition& partition);
    void notify_node_isolated(const NodeId& node_id, const std::string& reason);
    void notify_quorum_lost();
    void notify_quorum_restored();
};

// 工具函数
namespace split_brain_utils {
    // 计算网络连通性矩阵
    std::vector<std::vector<bool>> calculate_connectivity_matrix(
        const std::vector<NodeId>& nodes,
        const std::function<bool(const NodeId&, const NodeId&)>& connectivity_checker);
    
    // 寻找网络分区
    std::vector<std::vector<NodeId>> find_network_partitions(
        const std::vector<NodeId>& nodes,
        const std::vector<std::vector<bool>>& connectivity_matrix);
    
    // 计算仲裁大小
    size_t calculate_required_quorum_size(size_t total_nodes, QuorumStrategy strategy);
    
    // 验证仲裁决策
    bool validate_quorum_decision(const QuorumDecision& decision, 
                                 const std::vector<NodeId>& all_nodes);
}

} // namespace cluster
} // namespace cache