#include "cache/cache_server.h"
#include "cache/storage/wal.h"
#include "cache/storage/memtable.h" 
#include "cache/storage/sstable.h"
#include "cache/storage/lsm_tree.h"
#include "cache/cluster/split_brain_detector.h"
#include "cache/transaction/transaction_manager.h"
#include "cache/time/time_sync.h"
#include "cache/hotspot/hotspot_manager.h"
#include "cache/monitoring/metrics.h"
#include "cache/security/auth_manager.h"
#include <sstream>
#include <iostream>
#include <algorithm>

namespace cache {

// CacheServerEventHandler - 实现所有事件处理器接口
class CacheServerEventHandler : 
    public cluster::SplitBrainEventHandler,
    public transaction::TransactionEventHandler,
    public time::TimeSyncEventHandler,
    public hotspot::HotspotEventHandler {

private:
    std::shared_ptr<monitoring::MetricsCollector> metrics_;

public:
    CacheServerEventHandler(std::shared_ptr<monitoring::MetricsCollector> metrics) 
        : metrics_(metrics) {}

    // SplitBrainEventHandler 实现
    void on_split_brain_detected(const cluster::NetworkPartition& partition) override {
        LOG_WARN("Split brain detected between partitions");
        if (metrics_) {
            metrics_->increment_counter("split_brain_detected");
        }
    }

    void on_split_brain_resolved(const cluster::NetworkPartition& partition) override {
        LOG_INFO("Split brain resolved");
        if (metrics_) {
            metrics_->increment_counter("split_brain_resolved");
        }
    }

    void on_node_isolated(const cluster::NodeId& node_id, const std::string& reason) override {
        LOG_WARN("Node isolated: " + node_id + ", reason: " + reason);
        if (metrics_) {
            metrics_->increment_counter("node_isolated");
        }
    }

    void on_quorum_lost() override {
        LOG_ERROR("Quorum lost - entering read-only mode");
        if (metrics_) {
            metrics_->increment_counter("quorum_lost");
        }
    }

    void on_quorum_restored() override {
        LOG_INFO("Quorum restored - resuming normal operations");
        if (metrics_) {
            metrics_->increment_counter("quorum_restored");
        }
    }

    // TransactionEventHandler 实现
    void on_transaction_started(transaction::TransactionId tx_id) override {
        if (metrics_) {
            metrics_->increment_counter("transactions_started");
        }
    }

    void on_transaction_committed(transaction::TransactionId tx_id) override {
        if (metrics_) {
            metrics_->increment_counter("transactions_committed");
        }
    }

    void on_transaction_aborted(transaction::TransactionId tx_id, const std::string& reason) override {
        LOG_WARN("Transaction aborted: " + std::to_string(tx_id) + ", reason: " + reason);
        if (metrics_) {
            metrics_->increment_counter("transactions_aborted");
        }
    }

    void on_deadlock_detected(const std::vector<transaction::TransactionId>& cycle) override {
        LOG_WARN("Deadlock detected involving " + std::to_string(cycle.size()) + " transactions");
        if (metrics_) {
            metrics_->increment_counter("deadlocks_detected");
        }
    }

    // TimeSyncEventHandler 实现
    void on_time_sync_completed(const cluster::NodeId& node_id, time::ClockOffset offset) override {
        if (metrics_) {
            metrics_->set_gauge("time_offset_ms", offset.count());
        }
    }

    void on_time_sync_failed(const cluster::NodeId& node_id, const std::string& reason) override {
        LOG_WARN("Time sync failed with node " + node_id + ": " + reason);
        if (metrics_) {
            metrics_->increment_counter("time_sync_failures");
        }
    }

    void on_clock_drift_detected(const cluster::NodeId& node_id, time::ClockOffset drift) override {
        LOG_WARN("Clock drift detected with node " + node_id + ": " + std::to_string(drift.count()) + "ms");
        if (metrics_) {
            metrics_->set_gauge("clock_drift_ms", std::abs(drift.count()));
        }
    }

    void on_ntp_sync_completed(time::ClockOffset offset) override {
        if (metrics_) {
            metrics_->set_gauge("ntp_offset_ms", offset.count());
        }
    }

    // HotspotEventHandler 实现
    void on_hotspot_detected(const MultiLevelKey& key, const hotspot::HotspotInfo& hotspot) override {
        LOG_INFO("Hotspot detected: " + key.to_string() + ", heat score: " + std::to_string(hotspot.heat_score));
        if (metrics_) {
            metrics_->increment_counter("hotspots_detected");
            metrics_->set_gauge("hotspot_heat_score", hotspot.heat_score);
        }
    }

    void on_hotspot_handled(const MultiLevelKey& key, hotspot::HandlingStrategy strategy) override {
        std::string strategy_str = hotspot::hotspot_utils::handling_strategy_to_string(strategy);
        LOG_INFO("Hotspot handled: " + key.to_string() + ", strategy: " + strategy_str);
        if (metrics_) {
            metrics_->increment_counter("hotspots_handled");
        }
    }

    void on_hotspot_cooled_down(const MultiLevelKey& key) override {
        LOG_INFO("Hotspot cooled down: " + key.to_string());
        if (metrics_) {
            metrics_->increment_counter("hotspots_cooled_down");
        }
    }
};

CacheServer::CacheServer() : is_running_(false) {
    // 初始化事件处理器
    event_handler_ = std::make_shared<CacheServerEventHandler>(nullptr);
}

CacheServer::~CacheServer() {
    if (is_running_) {
        stop();
    }
}

Result<void> CacheServer::start(const CacheConfig& config) {
    if (is_running_) {
        return Result<void>(Status::ALREADY_EXISTS, "Server is already running");
    }

    config_ = config;

    // 1. 初始化指标收集器
    metrics_collector_ = std::make_shared<monitoring::DefaultMetricsCollector>();
    auto metrics_result = metrics_collector_->start();
    if (!metrics_result.is_ok()) {
        return Result<void>(metrics_result.status, "Failed to start metrics collector: " + metrics_result.error_message);
    }

    // 更新事件处理器
    event_handler_ = std::make_shared<CacheServerEventHandler>(metrics_collector_);

    // 2. 初始化安全管理器
    if (config_.security.enable_authentication) {
        auth_manager_ = std::make_unique<security::DefaultAuthManager>();
        auto auth_result = auth_manager_->start(config_.security.auth_config);
        if (!auth_result.is_ok()) {
            return Result<void>(auth_result.status, "Failed to start auth manager: " + auth_result.error_message);
        }
    }

    // 3. 初始化时间同步服务
    time_service_ = std::make_unique<time::DefaultTimeService>();
    time_service_->set_event_handler(event_handler_);
    auto time_result = time_service_->start(config_.time_sync);
    if (!time_result.is_ok()) {
        return Result<void>(time_result.status, "Failed to start time service: " + time_result.error_message);
    }

    // 4. 初始化存储引擎
    auto storage_config = config_.storage;
    lsm_tree_ = std::make_unique<storage::LSMTree>(storage_config);
    auto storage_result = lsm_tree_->open();
    if (!storage_result.is_ok()) {
        return Result<void>(storage_result.status, "Failed to open storage engine: " + storage_result.error_message);
    }

    // 5. 初始化事务管理器
    transaction_manager_ = std::make_unique<transaction::DefaultTransactionManager>();
    transaction_manager_->set_event_handler(event_handler_);
    auto tx_result = transaction_manager_->start();
    if (!tx_result.is_ok()) {
        return Result<void>(tx_result.status, "Failed to start transaction manager: " + tx_result.error_message);
    }

    // 6. 初始化热点管理器
    hotspot_manager_ = std::make_unique<hotspot::DefaultHotspotManager>();
    hotspot_manager_->set_event_handler(event_handler_);
    auto hotspot_result = hotspot_manager_->start(config_.hotspot);
    if (!hotspot_result.is_ok()) {
        return Result<void>(hotspot_result.status, "Failed to start hotspot manager: " + hotspot_result.error_message);
    }

    // 7. 初始化脑裂检测器
    split_brain_detector_ = std::make_unique<cluster::DefaultSplitBrainDetector>();
    split_brain_detector_->set_event_handler(event_handler_);
    
    // 添加集群节点
    for (const auto& node : config_.cluster.nodes) {
        split_brain_detector_->add_node(node.id);
        time_service_->add_node(node.id);
    }
    
    auto split_brain_result = split_brain_detector_->start(config_.cluster.split_brain_config);
    if (!split_brain_result.is_ok()) {
        return Result<void>(split_brain_result.status, "Failed to start split brain detector: " + split_brain_result.error_message);
    }

    is_running_ = true;
    LOG_INFO("Cache server started successfully on " + config_.network.host + ":" + std::to_string(config_.network.port));

    return Result<void>(Status::OK);
}

Result<void> CacheServer::stop() {
    if (!is_running_) {
        return Result<void>(Status::OK);
    }

    LOG_INFO("Stopping cache server...");

    // 按相反顺序停止组件
    if (split_brain_detector_) {
        split_brain_detector_->stop();
    }

    if (hotspot_manager_) {
        hotspot_manager_->stop();
    }

    if (transaction_manager_) {
        transaction_manager_->stop();
    }

    if (lsm_tree_) {
        lsm_tree_->close();
    }

    if (time_service_) {
        time_service_->stop();
    }

    if (auth_manager_) {
        auth_manager_->stop();
    }

    if (metrics_collector_) {
        metrics_collector_->stop();
    }

    is_running_ = false;
    LOG_INFO("Cache server stopped");

    return Result<void>(Status::OK);
}

bool CacheServer::is_running() const {
    return is_running_;
}

Result<DataEntry> CacheServer::get(const MultiLevelKey& key, const RequestContext& context) {
    if (!is_running_) {
        return Result<DataEntry>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 记录访问信息用于热点检测
    hotspot::AccessInfo access_info;
    access_info.timestamp = time_service_->now();
    access_info.operation = OperationType::read;
    access_info.size = 0; // 读操作大小为0
    access_info.location = context.client_location;
    
    hotspot_manager_->record_access(key, access_info);

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<DataEntry>(Status::UNAUTHORIZED, "Authentication failed");
        }

        auto authz_result = auth_manager_->authorize(auth_result.data, key, security::Permission::READ);
        if (!authz_result.is_ok()) {
            metrics_collector_->increment_counter("authz_failures");
            return Result<DataEntry>(Status::FORBIDDEN, "Authorization failed");
        }
    }

    // 检查是否需要事务
    if (context.transaction_id.has_value()) {
        auto tx_result = transaction_manager_->read(context.transaction_id.value(), key);
        if (tx_result.is_ok()) {
            metrics_collector_->increment_counter("reads_tx_success");
            access_info.size = tx_result.data.value.size();
            hotspot_manager_->record_access(key, access_info);
        } else {
            metrics_collector_->increment_counter("reads_tx_failed");
        }
        return tx_result;
    }

    // 普通读取
    auto result = lsm_tree_->get(key);
    if (result.is_ok()) {
        metrics_collector_->increment_counter("reads_success");
        access_info.size = result.data.value.size();
        hotspot_manager_->record_access(key, access_info);
    } else {
        metrics_collector_->increment_counter("reads_failed");
    }

    return result;
}

Result<void> CacheServer::put(const MultiLevelKey& key, const DataEntry& value, const RequestContext& context) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 记录访问信息用于热点检测
    hotspot::AccessInfo access_info;
    access_info.timestamp = time_service_->now();
    access_info.operation = OperationType::WRITE;
    access_info.size = value.value.size();
    access_info.location = context.client_location;
    
    hotspot_manager_->record_access(key, access_info);

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<void>(Status::UNAUTHORIZED, "Authentication failed");
        }

        auto authz_result = auth_manager_->authorize(auth_result.data, key, security::Permission::write);
        if (!authz_result.is_ok()) {
            metrics_collector_->increment_counter("authz_failures");
            return Result<void>(Status::FORBIDDEN, "Authorization failed");
        }
    }

    // 检查集群状态
    if (!split_brain_detector_->has_quorum()) {
        metrics_collector_->increment_counter("writes_rejected_no_quorum");
        return Result<void>(Status::SERVICE_UNAVAILABLE, "No quorum available");
    }

    // 检查是否需要事务
    if (context.transaction_id.has_value()) {
        auto tx_result = transaction_manager_->write(context.transaction_id.value(), key, value);
        if (tx_result.is_ok()) {
            metrics_collector_->increment_counter("writes_tx_success");
        } else {
            metrics_collector_->increment_counter("writes_tx_failed");
        }
        return tx_result;
    }

    // 普通写入
    auto result = lsm_tree_->put(key, value);
    if (result.is_ok()) {
        metrics_collector_->increment_counter("writes_success");
    } else {
        metrics_collector_->increment_counter("writes_failed");
    }

    return result;
}

Result<void> CacheServer::remove(const MultiLevelKey& key, const RequestContext& context) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 记录访问信息用于热点检测
    hotspot::AccessInfo access_info;
    access_info.timestamp = time_service_->now();
    access_info.operation = OperationType::DELETE;
    access_info.size = 0;
    access_info.location = context.client_location;
    
    hotspot_manager_->record_access(key, access_info);

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<void>(Status::UNAUTHORIZED, "Authentication failed");
        }

        auto authz_result = auth_manager_->authorize(auth_result.data, key, security::Permission::write);
        if (!authz_result.is_ok()) {
            metrics_collector_->increment_counter("authz_failures");
            return Result<void>(Status::FORBIDDEN, "Authorization failed");
        }
    }

    // 检查集群状态
    if (!split_brain_detector_->has_quorum()) {
        metrics_collector_->increment_counter("deletes_rejected_no_quorum");
        return Result<void>(Status::SERVICE_UNAVAILABLE, "No quorum available");
    }

    // 检查是否需要事务
    if (context.transaction_id.has_value()) {
        auto tx_result = transaction_manager_->remove(context.transaction_id.value(), key);
        if (tx_result.is_ok()) {
            metrics_collector_->increment_counter("deletes_tx_success");
        } else {
            metrics_collector_->increment_counter("deletes_tx_failed");
        }
        return tx_result;
    }

    // 普通删除
    auto result = lsm_tree_->remove(key);
    if (result.is_ok()) {
        metrics_collector_->increment_counter("deletes_success");
    } else {
        metrics_collector_->increment_counter("deletes_failed");
    }

    return result;
}

Result<std::vector<DataEntry>> CacheServer::range_query(const MultiLevelKey& start_key, 
                                                       const MultiLevelKey& end_key,
                                                       const RequestContext& context) {
    if (!is_running_) {
        return Result<std::vector<DataEntry>>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<std::vector<DataEntry>>(Status::UNAUTHORIZED, "Authentication failed");
        }

        auto authz_result = auth_manager_->authorize(auth_result.data, start_key, security::Permission::read);
        if (!authz_result.is_ok()) {
            metrics_collector_->increment_counter("authz_failures");
            return Result<std::vector<DataEntry>>(Status::FORBIDDEN, "Authorization failed");
        }
    }

    auto result = lsm_tree_->range_query(start_key, end_key);
    if (result.is_ok()) {
        metrics_collector_->increment_counter("range_queries_success");
        metrics_collector_->set_gauge("last_range_query_size", result.data.size());
    } else {
        metrics_collector_->increment_counter("range_queries_failed");
    }

    return result;
}

Result<transaction::TransactionId> CacheServer::begin_transaction(const RequestContext& context,
                                                                 transaction::IsolationLevel isolation) {
    if (!is_running_) {
        return Result<transaction::TransactionId>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<transaction::TransactionId>(Status::UNAUTHORIZED, "Authentication failed");
        }
    }

    // 检查集群状态
    if (!split_brain_detector_->has_quorum()) {
        metrics_collector_->increment_counter("tx_begin_rejected_no_quorum");
        return Result<transaction::TransactionId>(Status::SERVICE_UNAVAILABLE, "No quorum available");
    }

    return transaction_manager_->begin_transaction(isolation);
}

Result<void> CacheServer::commit_transaction(transaction::TransactionId tx_id, const RequestContext& context) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<void>(Status::UNAUTHORIZED, "Authentication failed");
        }
    }

    return transaction_manager_->commit_transaction(tx_id);
}

Result<void> CacheServer::abort_transaction(transaction::TransactionId tx_id, const RequestContext& context) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 安全验证
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            metrics_collector_->increment_counter("auth_failures");
            return Result<void>(Status::UNAUTHORIZED, "Authentication failed");
        }
    }

    return transaction_manager_->abort_transaction(tx_id);
}

monitoring::SystemMetrics CacheServer::get_metrics() const {
    if (!metrics_collector_) {
        return monitoring::SystemMetrics{};
    }

    return metrics_collector_->get_system_metrics();
}

cluster::ClusterStatus CacheServer::get_cluster_status() const {
    cluster::ClusterStatus status;
    
    if (split_brain_detector_) {
        status.has_quorum = split_brain_detector_->has_quorum();
        status.split_brain_status = split_brain_detector_->get_status();
        status.detected_partitions = split_brain_detector_->get_detected_partitions();
    }

    if (time_service_) {
        status.time_sync_quality = time_service_->get_sync_quality();
        status.estimated_uncertainty = time_service_->get_estimated_uncertainty();
    }

    return status;
}

std::vector<hotspot::HotspotInfo> CacheServer::get_hotspots() const {
    if (!hotspot_manager_) {
        return {};
    }

    return hotspot_manager_->get_hotspots();
}

hotspot::HotspotSummary CacheServer::get_hotspot_summary() const {
    if (!hotspot_manager_) {
        return hotspot::HotspotSummary{};
    }

    return hotspot_manager_->get_summary();
}

Result<void> CacheServer::handle_hotspot(const MultiLevelKey& key, hotspot::HandlingStrategy strategy,
                                        const RequestContext& context) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }

    // 安全验证 - 需要管理员权限
    if (auth_manager_) {
        auto auth_result = auth_manager_->authenticate(context.auth_token);
        if (!auth_result.is_ok()) {
            return Result<void>(Status::UNAUTHORIZED, "Authentication failed");
        }

        auto authz_result = auth_manager_->authorize(auth_result.data, key, security::Permission::admin);
        if (!authz_result.is_ok()) {
            return Result<void>(Status::FORBIDDEN, "Admin permission required");
        }
    }

    return hotspot_manager_->handle_hotspot(key, strategy);
}

time::HybridLogicalClock CacheServer::get_current_time() const {
    if (!time_service_) {
        time::HybridLogicalClock hlc;
        hlc.physical_time = std::chrono::system_clock::now();
        hlc.logical_time = 0;
        return hlc;
    }

    return time_service_->now_logical();
}

Result<void> CacheServer::sync_time_with_node(const cluster::NodeId& node_id) {
    if (!time_service_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Time service not available");
    }

    return time_service_->sync_with_node(node_id);
}

std::vector<transaction::TransactionId> CacheServer::get_active_transactions() const {
    if (!transaction_manager_) {
        return {};
    }

    return transaction_manager_->get_active_transactions();
}

transaction::TransactionState CacheServer::get_transaction_state(transaction::TransactionId tx_id) const {
    if (!transaction_manager_) {
        return transaction::TransactionState::ABORTED;
    }

    return transaction_manager_->get_transaction_state(tx_id);
}

std::string CacheServer::get_status_summary() const {
    std::ostringstream oss;
    
    oss << "=== Cache Server Status ===\n";
    oss << "Running: " << (is_running_ ? "Yes" : "No") << "\n";

    if (is_running_) {
        // 基础状态
        auto metrics = get_metrics();
        oss << "Memory Usage: " << metrics.memory_usage_mb << " MB\n";
        oss << "CPU Usage: " << std::fixed << std::setprecision(1) << metrics.cpu_usage_percent << "%\n";

        // 集群状态
        auto cluster_status = get_cluster_status();
        oss << "Has Quorum: " << (cluster_status.has_quorum ? "Yes" : "No") << "\n";
        oss << "Split Brain Status: " << static_cast<int>(cluster_status.split_brain_status) << "\n";
        oss << "Time Sync Quality: " << std::fixed << std::setprecision(2) << cluster_status.time_sync_quality << "\n";

        // 热点状态
        auto hotspot_summary = get_hotspot_summary();
        oss << "Active Hotspots: " << hotspot_summary.active_hotspots << "\n";
        oss << "Total Keys: " << hotspot_summary.total_keys << "\n";
        oss << "Total Requests: " << hotspot_summary.total_requests << "\n";

        // 事务状态
        auto active_txs = get_active_transactions();
        oss << "Active Transactions: " << active_txs.size() << "\n";
    }

    return oss.str();
}

Result<void> CacheServer::force_compaction() {
    if (!lsm_tree_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Storage engine not available");
    }

    return lsm_tree_->force_compaction();
}

Result<void> CacheServer::backup_data(const std::string& backup_path) {
    if (!lsm_tree_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Storage engine not available");
    }

    // 简化的备份实现
    // 实际实现需要完整的备份和恢复机制
    return Result<void>(Status::NOT_IMPLEMENTED, "Backup feature not implemented");
}

Result<void> CacheServer::restore_data(const std::string& backup_path) {
    if (!lsm_tree_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Storage engine not available");
    }

    // 简化的恢复实现
    return Result<void>(Status::NOT_IMPLEMENTED, "Restore feature not implemented");
}

} // namespace cache