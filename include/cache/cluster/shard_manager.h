#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <algorithm>

namespace cache {
namespace cluster {

// Shard information
struct ShardInfo {
    ShardId shard_id;
    std::vector<NodeId> replicas;
    NodeId primary;
    size_t data_size;
    uint64_t key_count;
    std::chrono::system_clock::time_point last_updated;
    
    ShardInfo() : shard_id(0), data_size(0), key_count(0) {}
    ShardInfo(ShardId id) : shard_id(id), data_size(0), key_count(0),
                           last_updated(std::chrono::system_clock::now()) {}
    
    bool has_replica(const NodeId& node_id) const {
        return std::find(replicas.begin(), replicas.end(), node_id) != replicas.end();
    }
    
    bool is_primary(const NodeId& node_id) const {
        return primary == node_id;
    }
};

// Shard migration task
struct MigrationTask {
    ShardId shard_id;
    NodeId source_node;
    NodeId target_node;
    size_t total_keys;
    size_t migrated_keys;
    std::chrono::system_clock::time_point start_time;
    bool completed;
    
    MigrationTask() : shard_id(0), total_keys(0), migrated_keys(0), completed(false) {}
    MigrationTask(ShardId id, const NodeId& src, const NodeId& dst)
        : shard_id(id), source_node(src), target_node(dst), 
          total_keys(0), migrated_keys(0), 
          start_time(std::chrono::system_clock::now()), completed(false) {}
    
    double progress() const {
        return total_keys > 0 ? static_cast<double>(migrated_keys) / total_keys : 0.0;
    }
};

// Consistent hash ring for shard distribution
class ConsistentHashRing {
public:
    ConsistentHashRing(size_t virtual_nodes_per_node = 150);
    ~ConsistentHashRing() = default;
    
    void add_node(const NodeId& node_id);
    void remove_node(const NodeId& node_id);
    NodeId get_node(const MultiLevelKey& key) const;
    std::vector<NodeId> get_nodes(const MultiLevelKey& key, size_t count) const;
    std::vector<NodeId> get_all_nodes() const;
    
    // Get affected shards when node is added/removed
    std::unordered_set<ShardId> get_affected_shards(const NodeId& node_id, size_t shard_count) const;
    
private:
    size_t virtual_nodes_per_node_;
    std::map<uint32_t, NodeId> ring_;
    std::unordered_set<NodeId> nodes_;
    mutable std::shared_mutex mutex_;
    
    uint32_t hash(const std::string& input) const;
    std::string get_virtual_node_key(const NodeId& node_id, size_t index) const;
};

// Shard manager interface
class ShardManager {
public:
    using MigrationCallback = std::function<void(const MigrationTask&)>;
    
    virtual ~ShardManager() = default;
    
    // Initialization
    virtual Result<void> initialize(size_t shard_count, size_t replication_factor,
                                    const std::vector<NodeId>& initial_nodes) = 0;
    
    // Shard information
    virtual ShardId get_shard_for_key(const MultiLevelKey& key) const = 0;
    virtual std::vector<NodeId> get_shard_replicas(ShardId shard_id) const = 0;
    virtual NodeId get_shard_primary(ShardId shard_id) const = 0;
    virtual std::unordered_set<ShardId> get_node_shards(const NodeId& node_id) const = 0;
    virtual std::vector<ShardInfo> get_all_shards() const = 0;
    
    // Node management
    virtual Result<void> add_node(const NodeId& node_id) = 0;
    virtual Result<void> remove_node(const NodeId& node_id) = 0;
    virtual std::vector<NodeId> get_all_nodes() const = 0;
    
    // Rebalancing
    virtual Result<void> rebalance() = 0;
    virtual bool is_rebalancing() const = 0;
    virtual double get_rebalance_progress() const = 0;
    virtual std::vector<MigrationTask> get_active_migrations() const = 0;
    
    // Statistics
    virtual std::unordered_map<NodeId, size_t> get_node_shard_counts() const = 0;
    virtual std::unordered_map<NodeId, size_t> get_node_data_sizes() const = 0;
    virtual double get_load_balance_factor() const = 0;
    
    // Event handling
    virtual void set_migration_callback(MigrationCallback callback) = 0;
    
    // Shard metadata updates
    virtual Result<void> update_shard_stats(ShardId shard_id, size_t data_size, uint64_t key_count) = 0;
};

// Concrete implementation
class DefaultShardManager : public ShardManager {
public:
    DefaultShardManager();
    ~DefaultShardManager() override;
    
    // ShardManager interface implementation
    Result<void> initialize(size_t shard_count, size_t replication_factor,
                           const std::vector<NodeId>& initial_nodes) override;
    
    ShardId get_shard_for_key(const MultiLevelKey& key) const override;
    std::vector<NodeId> get_shard_replicas(ShardId shard_id) const override;
    NodeId get_shard_primary(ShardId shard_id) const override;
    std::unordered_set<ShardId> get_node_shards(const NodeId& node_id) const override;
    std::vector<ShardInfo> get_all_shards() const override;
    
    Result<void> add_node(const NodeId& node_id) override;
    Result<void> remove_node(const NodeId& node_id) override;
    std::vector<NodeId> get_all_nodes() const override;
    
    Result<void> rebalance() override;
    bool is_rebalancing() const override;
    double get_rebalance_progress() const override;
    std::vector<MigrationTask> get_active_migrations() const override;
    
    std::unordered_map<NodeId, size_t> get_node_shard_counts() const override;
    std::unordered_map<NodeId, size_t> get_node_data_sizes() const override;
    double get_load_balance_factor() const override;
    
    void set_migration_callback(MigrationCallback callback) override;
    
    Result<void> update_shard_stats(ShardId shard_id, size_t data_size, uint64_t key_count) override;
    
private:
    size_t shard_count_;
    size_t replication_factor_;
    std::unique_ptr<ConsistentHashRing> hash_ring_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<ShardId, ShardInfo> shards_;
    std::unordered_set<NodeId> nodes_;
    std::vector<MigrationTask> active_migrations_;
    std::atomic<bool> is_rebalancing_;
    
    MigrationCallback migration_callback_;
    
    // Private methods
    uint32_t hash_key(const MultiLevelKey& key) const;
    Result<void> assign_shards_to_nodes();
    Result<void> create_migration_plan(const std::vector<NodeId>& new_nodes);
    void execute_migrations();
    void notify_migration_progress(const MigrationTask& task);
    
    // Load balancing helpers
    std::unordered_map<NodeId, double> calculate_node_loads() const;
    bool is_cluster_balanced() const;
    std::vector<std::pair<ShardId, NodeId>> find_shards_to_migrate(const NodeId& overloaded_node) const;
    NodeId find_best_target_node(ShardId shard_id) const;
};

} // namespace cluster
} // namespace cache