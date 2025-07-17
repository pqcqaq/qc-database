#include "cache/cluster/split_brain_detector.h"
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <queue>

namespace cache {
namespace cluster {

DefaultSplitBrainDetector::DefaultSplitBrainDetector() 
    : status_(SplitBrainStatus::HEALTHY), is_running_(false), stop_threads_(false) {
}

DefaultSplitBrainDetector::~DefaultSplitBrainDetector() {
    if (is_running_) {
        stop();
    }
}

Result<void> DefaultSplitBrainDetector::start(const SplitBrainConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (is_running_) {
        return Result<void>(Status::OK);
    }
    
    config_ = config;
    status_ = SplitBrainStatus::HEALTHY;
    stop_threads_ = false;
    
    // 启动检测线程
    detection_thread_ = std::thread(&DefaultSplitBrainDetector::detection_loop, this);
    recovery_thread_ = std::thread(&DefaultSplitBrainDetector::recovery_loop, this);
    
    is_running_ = true;
    return Result<void>(Status::OK);
}

Result<void> DefaultSplitBrainDetector::stop() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (!is_running_) {
        return Result<void>(Status::OK);
    }
    
    stop_threads_ = true;
    
    if (detection_thread_.joinable()) {
        detection_thread_.join();
    }
    
    if (recovery_thread_.joinable()) {
        recovery_thread_.join();
    }
    
    is_running_ = false;
    return Result<void>(Status::OK);
}

bool DefaultSplitBrainDetector::is_running() const {
    return is_running_;
}

void DefaultSplitBrainDetector::add_node(const NodeId& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    nodes_[node_id] = NodeStatus(node_id);
}

void DefaultSplitBrainDetector::remove_node(const NodeId& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    nodes_.erase(node_id);
    isolated_nodes_.erase(node_id);
}

void DefaultSplitBrainDetector::update_node_status(const NodeId& node_id, bool is_alive, 
                                                   std::chrono::system_clock::time_point last_seen) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second.is_alive = is_alive;
        it->second.last_seen = last_seen;
        
        if (is_alive) {
            it->second.missed_heartbeats = 0;
        }
    }
}

void DefaultSplitBrainDetector::process_heartbeat(const NodeId& from_node, const NodeId& to_node,
                                                 std::chrono::system_clock::time_point timestamp) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = nodes_.find(from_node);
    if (it != nodes_.end()) {
        it->second.last_heartbeat = timestamp;
        it->second.is_alive = true;
        it->second.missed_heartbeats = 0;
    }
}

SplitBrainStatus DefaultSplitBrainDetector::get_status() const {
    return status_.load();
}

QuorumDecision DefaultSplitBrainDetector::check_quorum() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return calculate_quorum();
}

std::vector<NetworkPartition> DefaultSplitBrainDetector::get_detected_partitions() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return detected_partitions_;
}

bool DefaultSplitBrainDetector::has_quorum() const {
    auto decision = check_quorum();
    return decision.has_quorum;
}

bool DefaultSplitBrainDetector::is_node_isolated(const NodeId& node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return isolated_nodes_.find(node_id) != isolated_nodes_.end();
}

Result<void> DefaultSplitBrainDetector::force_merge_partitions() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    // 强制合并所有分区
    for (auto& partition : detected_partitions_) {
        if (!partition.resolved) {
            partition.resolved = true;
            handle_partition_resolved(partition);
        }
    }
    
    // 清理已解决的分区
    detected_partitions_.erase(
        std::remove_if(detected_partitions_.begin(), detected_partitions_.end(),
                      [](const NetworkPartition& p) { return p.resolved; }),
        detected_partitions_.end());
    
    status_ = SplitBrainStatus::HEALTHY;
    return Result<void>(Status::OK);
}

Result<void> DefaultSplitBrainDetector::isolate_node(const NodeId& node_id, const std::string& reason) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second.is_isolated = true;
        it->second.isolation_reason = reason;
        isolated_nodes_.insert(node_id);
        
        notify_node_isolated(node_id, reason);
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultSplitBrainDetector::restore_node(const NodeId& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        it->second.is_isolated = false;
        it->second.isolation_reason.clear();
        isolated_nodes_.erase(node_id);
    }
    
    return Result<void>(Status::OK);
}

void DefaultSplitBrainDetector::set_event_handler(std::shared_ptr<SplitBrainEventHandler> handler) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    event_handler_ = handler;
}

void DefaultSplitBrainDetector::update_config(const SplitBrainConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    config_ = config;
}

SplitBrainConfig DefaultSplitBrainDetector::get_config() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

// 私有方法实现

void DefaultSplitBrainDetector::detection_loop() {
    while (!stop_threads_) {
        try {
            detect_network_partitions();
            
            if (detect_split_brain()) {
                auto decision = calculate_quorum();
                
                if (!decision.has_quorum) {
                    status_ = SplitBrainStatus::CONFIRMED;
                    notify_quorum_lost();
                } else {
                    // 隔离少数派节点
                    for (const auto& node : decision.isolated_nodes) {
                        isolate_node(node, "Split brain - minority partition");
                    }
                }
            }
            
            std::this_thread::sleep_for(config_.split_detection_timeout / 10);
            
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
    }
}

void DefaultSplitBrainDetector::recovery_loop() {
    while (!stop_threads_) {
        try {
            if (config_.auto_recovery_enabled && status_ == SplitBrainStatus::CONFIRMED) {
                status_ = SplitBrainStatus::RECOVERING;
                
                // 尝试恢复网络连接
                std::this_thread::sleep_for(config_.recovery_timeout);
                
                // 重新检查网络状态
                update_partition_status();
                
                bool all_resolved = true;
                for (const auto& partition : detected_partitions_) {
                    if (!partition.resolved) {
                        all_resolved = false;
                        break;
                    }
                }
                
                if (all_resolved) {
                    status_ = SplitBrainStatus::HEALTHY;
                    notify_quorum_restored();
                }
            }
            
            std::this_thread::sleep_for(config_.recovery_timeout);
            
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
    }
}

void DefaultSplitBrainDetector::detect_network_partitions() {
    auto now = std::chrono::system_clock::now();
    
    // 检查心跳超时
    for (auto& [node_id, status] : nodes_) {
        if (!status.is_isolated) {
            auto time_since_heartbeat = now - status.last_heartbeat;
            
            if (time_since_heartbeat > config_.heartbeat_timeout) {
                status.missed_heartbeats++;
                
                if (status.missed_heartbeats >= config_.max_missed_heartbeats) {
                    status.is_alive = false;
                }
            }
        }
    }
    
    // 寻找连通组件
    auto components = find_connected_components();
    
    if (components.size() > 1) {
        // 检测到网络分区
        for (size_t i = 0; i < components.size(); i++) {
            for (size_t j = i + 1; j < components.size(); j++) {
                NetworkPartition partition;
                partition.partition_a = components[i];
                partition.partition_b = components[j];
                partition.detected_at = now;
                partition.resolved = false;
                
                detected_partitions_.push_back(partition);
                handle_partition_detected(partition);
            }
        }
    }
}

QuorumDecision DefaultSplitBrainDetector::calculate_quorum() const {
    switch (config_.quorum_strategy) {
        case QuorumStrategy::MAJORITY:
            return majority_quorum();
        case QuorumStrategy::STATIC_QUORUM:
            return static_quorum();
        case QuorumStrategy::DYNAMIC_QUORUM:
            return dynamic_quorum();
        case QuorumStrategy::WITNESS_BASED:
            return witness_based_quorum();
        default:
            return majority_quorum();
    }
}

bool DefaultSplitBrainDetector::detect_split_brain() {
    return !detected_partitions_.empty();
}

void DefaultSplitBrainDetector::handle_partition_detected(const NetworkPartition& partition) {
    notify_split_brain_detected(partition);
}

void DefaultSplitBrainDetector::handle_partition_resolved(const NetworkPartition& partition) {
    notify_split_brain_resolved(partition);
}

QuorumDecision DefaultSplitBrainDetector::majority_quorum() const {
    QuorumDecision decision;
    decision.strategy_used = QuorumStrategy::MAJORITY;
    
    std::vector<NodeId> alive_nodes;
    std::vector<NodeId> dead_nodes;
    
    for (const auto& [node_id, status] : nodes_) {
        if (status.is_alive && !status.is_isolated) {
            alive_nodes.push_back(node_id);
        } else {
            dead_nodes.push_back(node_id);
        }
    }
    
    size_t total_nodes = nodes_.size();
    size_t required_quorum = (total_nodes / 2) + 1;
    
    decision.has_quorum = alive_nodes.size() >= required_quorum;
    decision.active_nodes = alive_nodes;
    decision.isolated_nodes = dead_nodes;
    
    if (decision.has_quorum) {
        decision.reason = "Majority quorum achieved with " + 
                         std::to_string(alive_nodes.size()) + "/" + 
                         std::to_string(total_nodes) + " nodes";
    } else {
        decision.reason = "Majority quorum lost: only " + 
                         std::to_string(alive_nodes.size()) + "/" + 
                         std::to_string(total_nodes) + " nodes alive";
    }
    
    return decision;
}

QuorumDecision DefaultSplitBrainDetector::static_quorum() const {
    QuorumDecision decision;
    decision.strategy_used = QuorumStrategy::STATIC_QUORUM;
    
    std::vector<NodeId> alive_nodes;
    for (const auto& [node_id, status] : nodes_) {
        if (status.is_alive && !status.is_isolated) {
            alive_nodes.push_back(node_id);
        }
    }
    
    decision.has_quorum = alive_nodes.size() >= config_.min_quorum_size;
    decision.active_nodes = alive_nodes;
    decision.reason = "Static quorum: " + std::to_string(alive_nodes.size()) + 
                     " >= " + std::to_string(config_.min_quorum_size);
    
    return decision;
}

QuorumDecision DefaultSplitBrainDetector::dynamic_quorum() const {
    QuorumDecision decision;
    decision.strategy_used = QuorumStrategy::DYNAMIC_QUORUM;
    
    // 动态调整仲裁大小基于当前活跃节点
    std::vector<NodeId> alive_nodes;
    for (const auto& [node_id, status] : nodes_) {
        if (status.is_alive && !status.is_isolated) {
            alive_nodes.push_back(node_id);
        }
    }
    
    size_t dynamic_quorum_size = std::max(config_.min_quorum_size, 
                                         (alive_nodes.size() + 1) / 2);
    
    decision.has_quorum = alive_nodes.size() >= dynamic_quorum_size;
    decision.active_nodes = alive_nodes;
    decision.reason = "Dynamic quorum: " + std::to_string(alive_nodes.size()) + 
                     " >= " + std::to_string(dynamic_quorum_size);
    
    return decision;
}

QuorumDecision DefaultSplitBrainDetector::witness_based_quorum() const {
    QuorumDecision decision;
    decision.strategy_used = QuorumStrategy::WITNESS_BASED;
    
    std::vector<NodeId> alive_nodes;
    std::vector<NodeId> alive_witnesses;
    
    for (const auto& [node_id, status] : nodes_) {
        if (status.is_alive && !status.is_isolated) {
            alive_nodes.push_back(node_id);
            
            // 检查是否是见证者节点
            if (std::find(config_.witness_nodes.begin(), config_.witness_nodes.end(), 
                         node_id) != config_.witness_nodes.end()) {
                alive_witnesses.push_back(node_id);
            }
        }
    }
    
    // 见证者策略：需要至少一个见证者节点
    decision.has_quorum = !alive_witnesses.empty() && 
                         alive_nodes.size() >= config_.min_quorum_size;
    decision.active_nodes = alive_nodes;
    decision.reason = "Witness-based quorum: " + std::to_string(alive_witnesses.size()) + 
                     " witnesses, " + std::to_string(alive_nodes.size()) + " total nodes";
    
    return decision;
}

std::vector<std::vector<NodeId>> DefaultSplitBrainDetector::find_connected_components() const {
    std::vector<std::vector<NodeId>> components;
    std::unordered_set<NodeId> visited;
    
    for (const auto& [node_id, status] : nodes_) {
        if (visited.find(node_id) == visited.end() && status.is_alive) {
            std::vector<NodeId> component;
            std::queue<NodeId> queue;
            
            queue.push(node_id);
            visited.insert(node_id);
            
            while (!queue.empty()) {
                NodeId current = queue.front();
                queue.pop();
                component.push_back(current);
                
                // 查找与当前节点连通的其他节点
                for (const auto& [other_id, other_status] : nodes_) {
                    if (visited.find(other_id) == visited.end() && 
                        other_status.is_alive &&
                        are_nodes_connected(current, other_id)) {
                        queue.push(other_id);
                        visited.insert(other_id);
                    }
                }
            }
            
            if (!component.empty()) {
                components.push_back(component);
            }
        }
    }
    
    return components;
}

bool DefaultSplitBrainDetector::are_nodes_connected(const NodeId& node1, const NodeId& node2) const {
    // 简化的连通性检查：基于心跳时间
    auto it1 = nodes_.find(node1);
    auto it2 = nodes_.find(node2);
    
    if (it1 == nodes_.end() || it2 == nodes_.end()) {
        return false;
    }
    
    auto now = std::chrono::system_clock::now();
    auto heartbeat_threshold = config_.heartbeat_timeout * 2;
    
    bool node1_recent = (now - it1->second.last_heartbeat) < heartbeat_threshold;
    bool node2_recent = (now - it2->second.last_heartbeat) < heartbeat_threshold;
    
    return node1_recent && node2_recent;
}

void DefaultSplitBrainDetector::update_partition_status() {
    auto now = std::chrono::system_clock::now();
    
    for (auto& partition : detected_partitions_) {
        if (!partition.resolved) {
            // 检查分区是否已经恢复
            bool partition_a_connected = true;
            bool partition_b_connected = true;
            
            // 检查分区A内部连通性
            for (size_t i = 0; i < partition.partition_a.size(); i++) {
                for (size_t j = i + 1; j < partition.partition_a.size(); j++) {
                    if (!are_nodes_connected(partition.partition_a[i], partition.partition_a[j])) {
                        partition_a_connected = false;
                        break;
                    }
                }
                if (!partition_a_connected) break;
            }
            
            // 检查分区B内部连通性
            for (size_t i = 0; i < partition.partition_b.size(); i++) {
                for (size_t j = i + 1; j < partition.partition_b.size(); j++) {
                    if (!are_nodes_connected(partition.partition_b[i], partition.partition_b[j])) {
                        partition_b_connected = false;
                        break;
                    }
                }
                if (!partition_b_connected) break;
            }
            
            // 检查分区间连通性
            bool inter_partition_connected = false;
            for (const auto& node_a : partition.partition_a) {
                for (const auto& node_b : partition.partition_b) {
                    if (are_nodes_connected(node_a, node_b)) {
                        inter_partition_connected = true;
                        break;
                    }
                }
                if (inter_partition_connected) break;
            }
            
            if (inter_partition_connected) {
                partition.resolved = true;
                partition.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - partition.detected_at);
                handle_partition_resolved(partition);
            }
        }
    }
}

void DefaultSplitBrainDetector::notify_split_brain_detected(const NetworkPartition& partition) {
    if (event_handler_) {
        event_handler_->on_split_brain_detected(partition);
    }
}

void DefaultSplitBrainDetector::notify_split_brain_resolved(const NetworkPartition& partition) {
    if (event_handler_) {
        event_handler_->on_split_brain_resolved(partition);
    }
}

void DefaultSplitBrainDetector::notify_node_isolated(const NodeId& node_id, const std::string& reason) {
    if (event_handler_) {
        event_handler_->on_node_isolated(node_id, reason);
    }
}

void DefaultSplitBrainDetector::notify_quorum_lost() {
    if (event_handler_) {
        event_handler_->on_quorum_lost();
    }
}

void DefaultSplitBrainDetector::notify_quorum_restored() {
    if (event_handler_) {
        event_handler_->on_quorum_restored();
    }
}

// 工具函数实现
namespace split_brain_utils {

std::vector<std::vector<bool>> calculate_connectivity_matrix(
    const std::vector<NodeId>& nodes,
    const std::function<bool(const NodeId&, const NodeId&)>& connectivity_checker) {
    
    size_t n = nodes.size();
    std::vector<std::vector<bool>> matrix(n, std::vector<bool>(n, false));
    
    for (size_t i = 0; i < n; i++) {
        matrix[i][i] = true; // 节点与自己连通
        for (size_t j = i + 1; j < n; j++) {
            bool connected = connectivity_checker(nodes[i], nodes[j]);
            matrix[i][j] = connected;
            matrix[j][i] = connected; // 对称矩阵
        }
    }
    
    return matrix;
}

std::vector<std::vector<NodeId>> find_network_partitions(
    const std::vector<NodeId>& nodes,
    const std::vector<std::vector<bool>>& connectivity_matrix) {
    
    std::vector<std::vector<NodeId>> partitions;
    std::vector<bool> visited(nodes.size(), false);
    
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!visited[i]) {
            std::vector<NodeId> partition;
            std::queue<size_t> queue;
            
            queue.push(i);
            visited[i] = true;
            
            while (!queue.empty()) {
                size_t current = queue.front();
                queue.pop();
                partition.push_back(nodes[current]);
                
                for (size_t j = 0; j < nodes.size(); j++) {
                    if (!visited[j] && connectivity_matrix[current][j]) {
                        queue.push(j);
                        visited[j] = true;
                    }
                }
            }
            
            partitions.push_back(partition);
        }
    }
    
    return partitions;
}

size_t calculate_required_quorum_size(size_t total_nodes, QuorumStrategy strategy) {
    switch (strategy) {
        case QuorumStrategy::MAJORITY:
            return (total_nodes / 2) + 1;
        case QuorumStrategy::STATIC_QUORUM:
            return std::max(size_t(1), total_nodes / 3);
        case QuorumStrategy::DYNAMIC_QUORUM:
            return std::max(size_t(1), (total_nodes + 1) / 2);
        case QuorumStrategy::WITNESS_BASED:
            return std::max(size_t(1), total_nodes / 2);
        default:
            return (total_nodes / 2) + 1;
    }
}

bool validate_quorum_decision(const QuorumDecision& decision, 
                             const std::vector<NodeId>& all_nodes) {
    // 验证决策的一致性
    size_t total_accounted = decision.active_nodes.size() + decision.isolated_nodes.size();
    
    if (total_accounted != all_nodes.size()) {
        return false;
    }
    
    // 验证没有重复节点
    std::unordered_set<NodeId> all_accounted;
    for (const auto& node : decision.active_nodes) {
        if (all_accounted.find(node) != all_accounted.end()) {
            return false;
        }
        all_accounted.insert(node);
    }
    
    for (const auto& node : decision.isolated_nodes) {
        if (all_accounted.find(node) != all_accounted.end()) {
            return false;
        }
        all_accounted.insert(node);
    }
    
    return true;
}

} // namespace split_brain_utils

} // namespace cluster
} // namespace cache