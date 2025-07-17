#pragma once

#include "cache/common/types.h"
#include "cache/cluster/raft.h"
#include "cache/cluster/shard_manager.h"
#include <memory>
#include <vector>
#include <unordered_set>
#include <functional>
#include <shared_mutex>

namespace cache {
namespace cluster {

// Node information
struct NodeInfo {
    NodeId node_id;
    std::string address;
    uint16_t port;
    std::chrono::system_clock::time_point last_heartbeat;
    bool is_alive;
    std::unordered_set<ShardId> shards;
    
    NodeInfo() : port(0), is_alive(false) {}
    NodeInfo(const NodeId& id, const std::string& addr, uint16_t p)
        : node_id(id), address(addr), port(p), 
          last_heartbeat(std::chrono::system_clock::now()), 
          is_alive(true) {}
    
    std::string endpoint() const {
        return address + ":" + std::to_string(port);
    }
};

// Cluster membership change events
enum class MembershipEventType {
    NODE_JOINED,
    NODE_LEFT,
    NODE_FAILED,
    SHARD_MOVED,
    REBALANCE_STARTED,
    REBALANCE_COMPLETED
};

struct MembershipEvent {
    MembershipEventType type;
    NodeId node_id;
    std::chrono::system_clock::time_point timestamp;
    std::string details;
    
    MembershipEvent(MembershipEventType t, const NodeId& id, const std::string& d = "")
        : type(t), node_id(id), timestamp(std::chrono::system_clock::now()), details(d) {}
};

// Cluster configuration
struct ClusterConfig {
    NodeId node_id;
    std::string listen_address;
    uint16_t listen_port;
    std::vector<std::string> seed_nodes;
    size_t replication_factor;
    size_t shard_count;
    uint32_t heartbeat_interval_ms;
    uint32_t failure_detection_timeout_ms;
    bool enable_auto_rebalance;
    
    ClusterConfig() 
        : listen_port(0), replication_factor(3), shard_count(1024),
          heartbeat_interval_ms(1000), failure_detection_timeout_ms(5000),
          enable_auto_rebalance(true) {}
};

// Cluster manager interface
class ClusterManager {
public:
    using MembershipEventCallback = std::function<void(const MembershipEvent&)>;
    
    virtual ~ClusterManager() = default;
    
    // Lifecycle
    virtual Result<void> start(const ClusterConfig& config) = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // Node management
    virtual Result<void> join_cluster() = 0;
    virtual Result<void> leave_cluster() = 0;
    virtual std::vector<NodeInfo> get_cluster_nodes() const = 0;
    virtual NodeInfo get_local_node() const = 0;
    virtual bool is_leader() const = 0;
    
    // Shard management
    virtual ShardId get_shard_for_key(const MultiLevelKey& key) const = 0;
    virtual std::vector<NodeId> get_shard_replicas(ShardId shard_id) const = 0;
    virtual NodeId get_shard_primary(ShardId shard_id) const = 0;
    virtual std::unordered_set<ShardId> get_local_shards() const = 0;
    
    // Data rebalancing
    virtual Result<void> trigger_rebalance() = 0;
    virtual bool is_rebalancing() const = 0;
    virtual double get_rebalance_progress() const = 0;
    
    // Event handling
    virtual void set_membership_callback(MembershipEventCallback callback) = 0;
    
    // Statistics
    virtual size_t get_cluster_size() const = 0;
    virtual std::map<NodeId, std::chrono::system_clock::time_point> get_node_last_seen() const = 0;
};

// Concrete implementation
class RaftClusterManager : public ClusterManager {
public:
    RaftClusterManager();
    ~RaftClusterManager() override;
    
    // ClusterManager interface implementation
    Result<void> start(const ClusterConfig& config) override;
    Result<void> stop() override;
    bool is_running() const override;
    
    Result<void> join_cluster() override;
    Result<void> leave_cluster() override;
    std::vector<NodeInfo> get_cluster_nodes() const override;
    NodeInfo get_local_node() const override;
    bool is_leader() const override;
    
    ShardId get_shard_for_key(const MultiLevelKey& key) const override;
    std::vector<NodeId> get_shard_replicas(ShardId shard_id) const override;
    NodeId get_shard_primary(ShardId shard_id) const override;
    std::unordered_set<ShardId> get_local_shards() const override;
    
    Result<void> trigger_rebalance() override;
    bool is_rebalancing() const override;
    double get_rebalance_progress() const override;
    
    void set_membership_callback(MembershipEventCallback callback) override;
    
    size_t get_cluster_size() const override;
    std::map<NodeId, std::chrono::system_clock::time_point> get_node_last_seen() const override;
    
private:
    ClusterConfig config_;
    std::unique_ptr<Raft> raft_;
    std::unique_ptr<ShardManager> shard_manager_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<NodeId, NodeInfo> nodes_;
    MembershipEventCallback membership_callback_;
    
    std::thread heartbeat_thread_;
    std::thread failure_detector_thread_;
    std::atomic<bool> stop_threads_;
    std::atomic<bool> is_running_;
    
    // Internal methods
    void heartbeat_loop();
    void failure_detection_loop();
    void handle_node_failure(const NodeId& node_id);
    void handle_node_recovery(const NodeId& node_id);
    void notify_membership_change(const MembershipEvent& event);
    
    Result<void> initialize_shards();
    Result<void> redistribute_shards();
    
    // Hash function for consistent hashing
    uint32_t hash_key(const MultiLevelKey& key) const;
};

} // namespace cluster
} // namespace cache