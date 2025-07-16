#include "cache/cache_server.h"
#include "cache/storage/storage_factory.h"
#include "cache/cluster/split_brain_detector.h"
#include "cache/transaction/transaction_manager.h"
#include "cache/time/time_sync.h"
#include "cache/hotspot/hotspot_manager.h"
#include <thread>
#include <chrono>
#include <random>

namespace cache {

CacheServer::CacheServer(const CacheServerConfig& config) 
    : config_(config), is_running_(false) {
    
    // 初始化日志系统
    init_logging();
    
    // 创建存储引擎
    storage_ = storage::StorageFactory::create_storage_engine(
        config_.storage_config.engine_type, 
        config_.storage_config.data_directory,
        config_.storage_config.engine_options
    );
    
    // 创建监控系统
    monitor_ = std::make_unique<monitoring::DefaultMonitor>();
    
    // 创建安全管理器
    security_ = std::make_unique<security::DefaultSecurityManager>();
    
    // 创建网络服务
    network_ = std::make_unique<network::DefaultNetworkService>();
    
    // 创建分片管理器
    shard_manager_ = std::make_unique<cluster::DefaultShardManager>();
    
    // 创建脑裂检测器
    split_brain_detector_ = std::make_unique<cluster::DefaultSplitBrainDetector>();
    
    // 创建事务管理器
    transaction_manager_ = std::make_unique<transaction::DefaultTransactionManager>();
    
    // 创建时间同步服务
    time_service_ = std::make_unique<time::DefaultTimeService>();
    
    // 创建热点管理器
    hotspot_manager_ = std::make_unique<hotspot::DefaultHotspotManager>();
    
    // 初始化性能统计
    stats_.start_time = std::chrono::steady_clock::now();
    stats_.total_requests = 0;
    stats_.successful_requests = 0;
    stats_.failed_requests = 0;
    stats_.read_requests = 0;
    stats_.write_requests = 0;
    stats_.delete_requests = 0;
    stats_.cache_hits = 0;
    stats_.cache_misses = 0;
}

CacheServer::~CacheServer() {
    if (is_running_) {
        stop();
    }
}

Result<void> CacheServer::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_running_) {
        return Result<void>(Status::OK);
    }
    
    try {
        log_info("Starting Cache Server...");
        
        // 1. 启动存储引擎
        log_info("Starting storage engine...");
        auto storage_result = storage_->open();
        if (!storage_result.is_ok()) {
            log_error("Failed to start storage engine: " + storage_result.error_message);
            return storage_result;
        }
        
        // 2. 启动监控系统
        log_info("Starting monitoring system...");
        auto monitor_result = monitor_->start();
        if (!monitor_result.is_ok()) {
            log_error("Failed to start monitoring: " + monitor_result.error_message);
            return monitor_result;
        }
        
        // 3. 启动安全管理器
        log_info("Starting security manager...");
        auto security_result = security_->start();
        if (!security_result.is_ok()) {
            log_error("Failed to start security manager: " + security_result.error_message);
            return security_result;
        }
        
        // 4. 启动时间同步服务
        log_info("Starting time synchronization service...");
        auto time_result = time_service_->start(config_.time_sync_config);
        if (!time_result.is_ok()) {
            log_error("Failed to start time sync service: " + time_result.error_message);
            return time_result;
        }
        
        // 5. 启动事务管理器
        log_info("Starting transaction manager...");
        auto tx_result = transaction_manager_->start();
        if (!tx_result.is_ok()) {
            log_error("Failed to start transaction manager: " + tx_result.error_message);
            return tx_result;
        }
        
        // 6. 启动热点管理器
        log_info("Starting hotspot manager...");
        auto hotspot_result = hotspot_manager_->start(
            config_.hotspot_detection_config, 
            config_.hotspot_handling_config
        );
        if (!hotspot_result.is_ok()) {
            log_error("Failed to start hotspot manager: " + hotspot_result.error_message);
            return hotspot_result;
        }
        
        // 7. 启动集群组件
        if (config_.cluster_config.enable_clustering) {
            log_info("Starting cluster components...");
            
            // 启动分片管理器
            auto shard_result = shard_manager_->start(config_.cluster_config);
            if (!shard_result.is_ok()) {
                log_error("Failed to start shard manager: " + shard_result.error_message);
                return shard_result;
            }
            
            // 启动脑裂检测器
            cluster::SplitBrainConfig sb_config;
            auto sb_result = split_brain_detector_->start(sb_config);
            if (!sb_result.is_ok()) {
                log_error("Failed to start split brain detector: " + sb_result.error_message);
                return sb_result;
            }
            
            // 加入集群
            if (!config_.cluster_config.seed_nodes.empty()) {
                auto join_result = join_cluster();
                if (!join_result.is_ok()) {
                    log_error("Failed to join cluster: " + join_result.error_message);
                    return join_result;
                }
            }
        }
        
        // 8. 启动网络服务（最后启动，这样可以开始接受请求）
        log_info("Starting network service...");
        network::NetworkConfig net_config;
        net_config.listen_address = config_.network_config.listen_address;
        net_config.listen_port = config_.network_config.listen_port;
        net_config.max_connections = config_.network_config.max_connections;
        net_config.enable_ssl = config_.network_config.enable_ssl;
        
        auto network_result = network_->start(net_config);
        if (!network_result.is_ok()) {
            log_error("Failed to start network service: " + network_result.error_message);
            return network_result;
        }
        
        // 设置请求处理器
        network_->set_request_handler(
            [this](const network::Request& req) -> network::Response {
                return handle_request(req);
            }
        );
        
        // 启动后台任务
        start_background_tasks();
        
        is_running_ = true;
        log_info("Cache Server started successfully on " + 
                config_.network_config.listen_address + ":" + 
                std::to_string(config_.network_config.listen_port));
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        log_error("Exception during server startup: " + std::string(e.what()));
        return Result<void>(Status::INTERNAL_ERROR, "Failed to start server: " + std::string(e.what()));
    }
}

Result<void> CacheServer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_running_) {
        return Result<void>(Status::OK);
    }
    
    log_info("Stopping Cache Server...");
    
    try {
        // 停止后台任务
        stop_background_tasks();
        
        // 停止网络服务
        if (network_) {
            network_->stop();
        }
        
        // 停止集群组件
        if (split_brain_detector_) {
            split_brain_detector_->stop();
        }
        
        if (shard_manager_) {
            shard_manager_->stop();
        }
        
        // 停止应用层组件
        if (hotspot_manager_) {
            hotspot_manager_->stop();
        }
        
        if (transaction_manager_) {
            transaction_manager_->stop();
        }
        
        if (time_service_) {
            time_service_->stop();
        }
        
        if (security_) {
            security_->stop();
        }
        
        if (monitor_) {
            monitor_->stop();
        }
        
        // 最后停止存储引擎
        if (storage_) {
            storage_->close();
        }
        
        is_running_ = false;
        log_info("Cache Server stopped successfully");
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        log_error("Exception during server shutdown: " + std::string(e.what()));
        return Result<void>(Status::INTERNAL_ERROR, "Failed to stop server gracefully");
    }
}

bool CacheServer::is_running() const {
    return is_running_;
}

Result<DataEntry> CacheServer::get(const MultiLevelKey& key, const std::string& client_id) {
    if (!is_running_) {
        return Result<DataEntry>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 记录访问用于热点检测
    hotspot_manager_->record_access(key, OperationType::GET, get_node_id());
    
    // 更新统计
    stats_.total_requests++;
    stats_.read_requests++;
    
    try {
        // 检查权限
        auto auth_result = check_permission(client_id, key, "read");
        if (!auth_result.is_ok()) {
            stats_.failed_requests++;
            return Result<DataEntry>(auth_result.status, auth_result.error_message);
        }
        
        // 如果启用了集群，检查分片
        if (config_.cluster_config.enable_clustering) {
            auto shard_result = get_key_shard(key);
            if (!shard_result.is_ok()) {
                stats_.failed_requests++;
                return Result<DataEntry>(shard_result.status, shard_result.error_message);
            }
            
            // 如果key不在本节点，转发请求
            if (shard_result.data != get_node_id()) {
                auto forward_result = forward_get_request(key, shard_result.data);
                if (forward_result.is_ok()) {
                    stats_.successful_requests++;
                } else {
                    stats_.failed_requests++;
                }
                return forward_result;
            }
        }
        
        // 从存储引擎读取
        auto result = storage_->get(key);
        
        if (result.is_ok()) {
            stats_.successful_requests++;
            stats_.cache_hits++;
        } else {
            if (result.status == Status::NOT_FOUND) {
                stats_.cache_misses++;
            } else {
                stats_.failed_requests++;
            }
        }
        
        // 记录延迟
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        monitor_->record_latency("get", latency);
        
        return result;
        
    } catch (const std::exception& e) {
        stats_.failed_requests++;
        log_error("Exception in get operation: " + std::string(e.what()));
        return Result<DataEntry>(Status::INTERNAL_ERROR, "Internal server error");
    }
}

Result<void> CacheServer::put(const MultiLevelKey& key, const DataEntry& entry, const std::string& client_id) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 记录访问用于热点检测
    hotspot_manager_->record_access(key, OperationType::PUT, get_node_id());
    
    // 更新统计
    stats_.total_requests++;
    stats_.write_requests++;
    
    try {
        // 检查权限
        auto auth_result = check_permission(client_id, key, "write");
        if (!auth_result.is_ok()) {
            stats_.failed_requests++;
            return Result<void>(auth_result.status, auth_result.error_message);
        }
        
        // 验证数据大小
        if (key.to_string().size() > config::MAX_KEY_SIZE) {
            stats_.failed_requests++;
            return Result<void>(Status::INVALID_ARGUMENT, "Key size exceeds limit");
        }
        
        if (entry.value.size() > config::MAX_VALUE_SIZE) {
            stats_.failed_requests++;
            return Result<void>(Status::INVALID_ARGUMENT, "Value size exceeds limit");
        }
        
        // 如果启用了集群，检查分片
        if (config_.cluster_config.enable_clustering) {
            auto shard_result = get_key_shard(key);
            if (!shard_result.is_ok()) {
                stats_.failed_requests++;
                return Result<void>(shard_result.status, shard_result.error_message);
            }
            
            // 如果key不在本节点，转发请求
            if (shard_result.data != get_node_id()) {
                auto forward_result = forward_put_request(key, entry, shard_result.data);
                if (forward_result.is_ok()) {
                    stats_.successful_requests++;
                } else {
                    stats_.failed_requests++;
                }
                return forward_result;
            }
        }
        
        // 写入存储引擎
        auto result = storage_->put(key, entry);
        
        if (result.is_ok()) {
            stats_.successful_requests++;
            
            // 如果启用了集群，同步到副本
            if (config_.cluster_config.enable_clustering && config_.cluster_config.replication_factor > 1) {
                replicate_write(key, entry);
            }
        } else {
            stats_.failed_requests++;
        }
        
        // 记录延迟
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        monitor_->record_latency("put", latency);
        
        return result;
        
    } catch (const std::exception& e) {
        stats_.failed_requests++;
        log_error("Exception in put operation: " + std::string(e.what()));
        return Result<void>(Status::INTERNAL_ERROR, "Internal server error");
    }
}

Result<void> CacheServer::remove(const MultiLevelKey& key, const std::string& client_id) {
    if (!is_running_) {
        return Result<void>(Status::SERVICE_UNAVAILABLE, "Server is not running");
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 记录访问用于热点检测
    hotspot_manager_->record_access(key, OperationType::DELETE, get_node_id());
    
    // 更新统计
    stats_.total_requests++;
    stats_.delete_requests++;
    
    try {
        // 检查权限
        auto auth_result = check_permission(client_id, key, "delete");
        if (!auth_result.is_ok()) {
            stats_.failed_requests++;
            return Result<void>(auth_result.status, auth_result.error_message);
        }
        
        // 如果启用了集群，检查分片
        if (config_.cluster_config.enable_clustering) {
            auto shard_result = get_key_shard(key);
            if (!shard_result.is_ok()) {
                stats_.failed_requests++;
                return Result<void>(shard_result.status, shard_result.error_message);
            }
            
            // 如果key不在本节点，转发请求
            if (shard_result.data != get_node_id()) {
                auto forward_result = forward_delete_request(key, shard_result.data);
                if (forward_result.is_ok()) {
                    stats_.successful_requests++;
                } else {
                    stats_.failed_requests++;
                }
                return forward_result;
            }
        }
        
        // 从存储引擎删除
        auto result = storage_->remove(key);
        
        if (result.is_ok()) {
            stats_.successful_requests++;
            
            // 如果启用了集群，同步到副本
            if (config_.cluster_config.enable_clustering && config_.cluster_config.replication_factor > 1) {
                replicate_delete(key);
            }
        } else {
            stats_.failed_requests++;
        }
        
        // 记录延迟
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        monitor_->record_latency("remove", latency);
        
        return result;
        
    } catch (const std::exception& e) {
        stats_.failed_requests++;
        log_error("Exception in remove operation: " + std::string(e.what()));
        return Result<void>(Status::INTERNAL_ERROR, "Internal server error");
    }
}

Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
CacheServer::range_scan(const RangeQuery& query, const std::string& client_id) {
    if (!is_running_) {
        return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
            Status::SERVICE_UNAVAILABLE, "Server is not running");
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    // 更新统计
    stats_.total_requests++;
    stats_.read_requests++;
    
    try {
        // 检查权限
        auto auth_result = check_permission(client_id, query.start_key, "read");
        if (!auth_result.is_ok()) {
            stats_.failed_requests++;
            return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
                auth_result.status, auth_result.error_message);
        }
        
        // 限制范围查询的结果数量
        RangeQuery limited_query = query;
        if (limited_query.limit > config_.performance_config.max_range_scan_limit) {
            limited_query.limit = config_.performance_config.max_range_scan_limit;
        }
        
        // 从存储引擎扫描
        auto result = storage_->range_scan(limited_query);
        
        if (result.is_ok()) {
            stats_.successful_requests++;
        } else {
            stats_.failed_requests++;
        }
        
        // 记录延迟
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        monitor_->record_latency("range_scan", latency);
        
        return result;
        
    } catch (const std::exception& e) {
        stats_.failed_requests++;
        log_error("Exception in range_scan operation: " + std::string(e.what()));
        return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
            Status::INTERNAL_ERROR, "Internal server error");
    }
}

CacheServerStats CacheServer::get_stats() const {
    CacheServerStats current_stats = stats_;
    
    // 计算运行时间
    auto now = std::chrono::steady_clock::now();
    current_stats.uptime = std::chrono::duration_cast<std::chrono::seconds>(
        now - current_stats.start_time);
    
    // 获取存储统计
    if (storage_) {
        current_stats.storage_size = storage_->size();
        current_stats.memory_usage = storage_->memory_usage();
        current_stats.cache_hit_ratio = storage_->cache_hit_ratio();
    }
    
    // 获取热点统计
    if (hotspot_manager_) {
        current_stats.hotspot_keys = hotspot_manager_->get_current_hotspots().size();
    }
    
    return current_stats;
}

std::string CacheServer::get_node_id() const {
    return config_.node_config.node_id;
}

// 私有方法实现

void CacheServer::init_logging() {
    // 初始化日志系统
    // 这里可以配置日志级别、输出目标等
}

void CacheServer::log_info(const std::string& message) {
    // 记录信息日志
    std::cout << "[INFO] " << message << std::endl;
}

void CacheServer::log_error(const std::string& message) {
    // 记录错误日志
    std::cerr << "[ERROR] " << message << std::endl;
}

void CacheServer::log_debug(const std::string& message) {
    // 记录调试日志
    if (config_.debug_config.enable_debug_logging) {
        std::cout << "[DEBUG] " << message << std::endl;
    }
}

network::Response CacheServer::handle_request(const network::Request& request) {
    network::Response response;
    response.request_id = request.request_id;
    
    try {
        // 解析请求类型
        switch (request.type) {
            case network::RequestType::GET: {
                auto result = get(request.key, request.client_id);
                if (result.is_ok()) {
                    response.status = network::ResponseStatus::SUCCESS;
                    response.data = result.data;
                } else {
                    response.status = network::ResponseStatus::ERROR;
                    response.error_message = result.error_message;
                }
                break;
            }
            
            case network::RequestType::PUT: {
                auto result = put(request.key, request.entry, request.client_id);
                if (result.is_ok()) {
                    response.status = network::ResponseStatus::SUCCESS;
                } else {
                    response.status = network::ResponseStatus::ERROR;
                    response.error_message = result.error_message;
                }
                break;
            }
            
            case network::RequestType::DELETE: {
                auto result = remove(request.key, request.client_id);
                if (result.is_ok()) {
                    response.status = network::ResponseStatus::SUCCESS;
                } else {
                    response.status = network::ResponseStatus::ERROR;
                    response.error_message = result.error_message;
                }
                break;
            }
            
            case network::RequestType::RANGE_SCAN: {
                auto result = range_scan(request.range_query, request.client_id);
                if (result.is_ok()) {
                    response.status = network::ResponseStatus::SUCCESS;
                    response.scan_results = result.data;
                } else {
                    response.status = network::ResponseStatus::ERROR;
                    response.error_message = result.error_message;
                }
                break;
            }
            
            case network::RequestType::STATS: {
                response.status = network::ResponseStatus::SUCCESS;
                response.stats = get_stats();
                break;
            }
            
            default:
                response.status = network::ResponseStatus::ERROR;
                response.error_message = "Unsupported request type";
                break;
        }
        
    } catch (const std::exception& e) {
        response.status = network::ResponseStatus::ERROR;
        response.error_message = "Internal server error: " + std::string(e.what());
    }
    
    return response;
}

Result<void> CacheServer::check_permission(const std::string& client_id, 
                                          const MultiLevelKey& key, 
                                          const std::string& operation) {
    if (!config_.security_config.enable_authentication) {
        return Result<void>(Status::OK);
    }
    
    return security_->check_permission(client_id, key.to_string(), operation);
}

Result<NodeId> CacheServer::get_key_shard(const MultiLevelKey& key) {
    return shard_manager_->get_shard_for_key(key);
}

Result<void> CacheServer::join_cluster() {
    // 实现集群加入逻辑
    for (const auto& seed_node : config_.cluster_config.seed_nodes) {
        auto result = shard_manager_->join_cluster(seed_node);
        if (result.is_ok()) {
            log_info("Successfully joined cluster via seed node: " + seed_node);
            return result;
        }
    }
    
    return Result<void>(Status::NETWORK_ERROR, "Failed to join cluster via any seed node");
}

void CacheServer::start_background_tasks() {
    stop_background_ = false;
    
    // 启动健康检查任务
    health_check_thread_ = std::thread(&CacheServer::health_check_loop, this);
    
    // 启动统计报告任务
    stats_report_thread_ = std::thread(&CacheServer::stats_report_loop, this);
    
    // 启动垃圾回收任务
    gc_thread_ = std::thread(&CacheServer::garbage_collection_loop, this);
}

void CacheServer::stop_background_tasks() {
    stop_background_ = true;
    
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }
    
    if (stats_report_thread_.joinable()) {
        stats_report_thread_.join();
    }
    
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

void CacheServer::health_check_loop() {
    while (!stop_background_) {
        try {
            // 检查各组件健康状态
            bool storage_healthy = storage_ && storage_->is_open();
            bool network_healthy = network_ && network_->is_running();
            bool cluster_healthy = !config_.cluster_config.enable_clustering || 
                                  (shard_manager_ && shard_manager_->is_running());
            
            if (!storage_healthy) {
                log_error("Storage engine is not healthy");
            }
            
            if (!network_healthy) {
                log_error("Network service is not healthy");
            }
            
            if (!cluster_healthy) {
                log_error("Cluster components are not healthy");
            }
            
            // 检查资源使用情况
            auto stats = get_stats();
            if (stats.memory_usage > config_.performance_config.max_memory_usage) {
                log_error("Memory usage exceeded limit: " + std::to_string(stats.memory_usage));
            }
            
        } catch (const std::exception& e) {
            log_error("Exception in health check: " + std::string(e.what()));
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

void CacheServer::stats_report_loop() {
    while (!stop_background_) {
        try {
            auto stats = get_stats();
            
            // 记录关键指标到监控系统
            if (monitor_) {
                monitor_->record_counter("total_requests", stats.total_requests);
                monitor_->record_counter("successful_requests", stats.successful_requests);
                monitor_->record_counter("failed_requests", stats.failed_requests);
                monitor_->record_gauge("cache_hit_ratio", stats.cache_hit_ratio);
                monitor_->record_gauge("memory_usage", stats.memory_usage);
                monitor_->record_gauge("storage_size", stats.storage_size);
            }
            
        } catch (const std::exception& e) {
            log_error("Exception in stats reporting: " + std::string(e.what()));
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

void CacheServer::garbage_collection_loop() {
    while (!stop_background_) {
        try {
            // 触发存储引擎的压缩
            if (storage_) {
                storage_->compact();
            }
            
            // 清理过期的事务
            if (transaction_manager_) {
                transaction_manager_->cleanup_expired_transactions();
            }
            
            // 清理过期的热点记录
            if (hotspot_manager_) {
                // hotspot_manager_->cleanup_expired_records();
            }
            
        } catch (const std::exception& e) {
            log_error("Exception in garbage collection: " + std::string(e.what()));
        }
        
        std::this_thread::sleep_for(std::chrono::minutes(10));
    }
}

Result<DataEntry> CacheServer::forward_get_request(const MultiLevelKey& key, const NodeId& target_node) {
    // 实现请求转发逻辑
    return network_->forward_get_request(key, target_node);
}

Result<void> CacheServer::forward_put_request(const MultiLevelKey& key, 
                                             const DataEntry& entry, 
                                             const NodeId& target_node) {
    return network_->forward_put_request(key, entry, target_node);
}

Result<void> CacheServer::forward_delete_request(const MultiLevelKey& key, const NodeId& target_node) {
    return network_->forward_delete_request(key, target_node);
}

void CacheServer::replicate_write(const MultiLevelKey& key, const DataEntry& entry) {
    // 获取副本节点列表
    auto replica_nodes = shard_manager_->get_replica_nodes(key);
    
    // 异步复制到副本节点
    for (const auto& node : replica_nodes) {
        if (node != get_node_id()) {
            std::thread([this, key, entry, node]() {
                try {
                    auto result = network_->replicate_write(key, entry, node);
                    if (!result.is_ok()) {
                        log_error("Failed to replicate write to node " + node + ": " + result.error_message);
                    }
                } catch (const std::exception& e) {
                    log_error("Exception during write replication: " + std::string(e.what()));
                }
            }).detach();
        }
    }
}

void CacheServer::replicate_delete(const MultiLevelKey& key) {
    // 获取副本节点列表
    auto replica_nodes = shard_manager_->get_replica_nodes(key);
    
    // 异步复制删除到副本节点
    for (const auto& node : replica_nodes) {
        if (node != get_node_id()) {
            std::thread([this, key, node]() {
                try {
                    auto result = network_->replicate_delete(key, node);
                    if (!result.is_ok()) {
                        log_error("Failed to replicate delete to node " + node + ": " + result.error_message);
                    }
                } catch (const std::exception& e) {
                    log_error("Exception during delete replication: " + std::string(e.what()));
                }
            }).detach();
        }
    }
}

} // namespace cache