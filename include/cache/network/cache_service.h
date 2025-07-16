#pragma once

#include "cache/common/types.h"
#include "cache/cluster/cluster_manager.h"
#include "cache/storage/storage_engine.h"
#include "cache/security/auth_manager.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace cache {
namespace network {

// Request types
enum class RequestType {
    GET = 1,
    PUT = 2,
    DELETE = 3,
    RANGE_SCAN = 4,
    MULTI_GET = 5,
    MULTI_PUT = 6,
    PING = 7,
    STATS = 8
};

// Basic request structure
struct CacheRequest {
    RequestType type;
    std::string request_id;
    std::string client_id;
    std::string auth_token;
    std::chrono::system_clock::time_point timestamp;
    
    CacheRequest() : type(RequestType::PING) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Response structure
struct CacheResponse {
    Status status;
    std::string request_id;
    std::string error_message;
    std::chrono::system_clock::time_point timestamp;
    uint32_t latency_ms;
    
    CacheResponse() : status(Status::OK), latency_ms(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// Specific request types
struct GetRequest : public CacheRequest {
    MultiLevelKey key;
    
    GetRequest() { type = RequestType::GET; }
};

struct GetResponse : public CacheResponse {
    DataEntry data;
    bool found;
    
    GetResponse() : found(false) {}
};

struct PutRequest : public CacheRequest {
    MultiLevelKey key;
    DataEntry data;
    bool require_version_match;
    Version expected_version;
    
    PutRequest() : require_version_match(false), expected_version(0) { 
        type = RequestType::PUT; 
    }
};

struct PutResponse : public CacheResponse {
    Version new_version;
    
    PutResponse() : new_version(0) {}
};

struct DeleteRequest : public CacheRequest {
    MultiLevelKey key;
    bool require_version_match;
    Version expected_version;
    
    DeleteRequest() : require_version_match(false), expected_version(0) { 
        type = RequestType::DELETE; 
    }
};

struct DeleteResponse : public CacheResponse {
    bool deleted;
    
    DeleteResponse() : deleted(false) {}
};

struct RangeScanRequest : public CacheRequest {
    RangeQuery query;
    
    RangeScanRequest() { type = RequestType::RANGE_SCAN; }
};

struct RangeScanResponse : public CacheResponse {
    std::vector<std::pair<MultiLevelKey, DataEntry>> results;
    bool has_more;
    
    RangeScanResponse() : has_more(false) {}
};

struct MultiGetRequest : public CacheRequest {
    std::vector<MultiLevelKey> keys;
    
    MultiGetRequest() { type = RequestType::MULTI_GET; }
};

struct MultiGetResponse : public CacheResponse {
    std::vector<std::pair<MultiLevelKey, DataEntry>> results;
    std::vector<MultiLevelKey> not_found_keys;
};

struct MultiPutRequest : public CacheRequest {
    std::vector<std::pair<MultiLevelKey, DataEntry>> entries;
    bool atomic;
    
    MultiPutRequest() : atomic(false) { type = RequestType::MULTI_PUT; }
};

struct MultiPutResponse : public CacheResponse {
    std::vector<Version> new_versions;
    std::vector<MultiLevelKey> failed_keys;
};

struct StatsRequest : public CacheRequest {
    bool include_cluster_stats;
    bool include_shard_stats;
    
    StatsRequest() : include_cluster_stats(false), include_shard_stats(false) {
        type = RequestType::STATS;
    }
};

struct StatsResponse : public CacheResponse {
    std::map<std::string, std::string> stats;
};

// Cache service interface
class CacheService {
public:
    virtual ~CacheService() = default;
    
    // Basic operations
    virtual GetResponse handle_get(const GetRequest& request) = 0;
    virtual PutResponse handle_put(const PutRequest& request) = 0;
    virtual DeleteResponse handle_delete(const DeleteRequest& request) = 0;
    virtual RangeScanResponse handle_range_scan(const RangeScanRequest& request) = 0;
    virtual MultiGetResponse handle_multi_get(const MultiGetRequest& request) = 0;
    virtual MultiPutResponse handle_multi_put(const MultiPutRequest& request) = 0;
    virtual StatsResponse handle_stats(const StatsRequest& request) = 0;
    
    // Health check
    virtual CacheResponse handle_ping(const CacheRequest& request) = 0;
};

// Cache service implementation
class CacheServiceImpl : public CacheService {
public:
    CacheServiceImpl(std::shared_ptr<storage::StorageEngine> storage,
                     std::shared_ptr<cluster::ClusterManager> cluster,
                     std::shared_ptr<security::AuthManager> auth);
    ~CacheServiceImpl() override;
    
    // CacheService interface implementation
    GetResponse handle_get(const GetRequest& request) override;
    PutResponse handle_put(const PutRequest& request) override;
    DeleteResponse handle_delete(const DeleteRequest& request) override;
    RangeScanResponse handle_range_scan(const RangeScanRequest& request) override;
    MultiGetResponse handle_multi_get(const MultiGetRequest& request) override;
    MultiPutResponse handle_multi_put(const MultiPutRequest& request) override;
    StatsResponse handle_stats(const StatsRequest& request) override;
    
    CacheResponse handle_ping(const CacheRequest& request) override;
    
    // Configuration
    void set_request_timeout(uint32_t timeout_ms) { request_timeout_ms_ = timeout_ms; }
    void set_max_batch_size(size_t max_size) { max_batch_size_ = max_size; }
    
private:
    std::shared_ptr<storage::StorageEngine> storage_;
    std::shared_ptr<cluster::ClusterManager> cluster_;
    std::shared_ptr<security::AuthManager> auth_;
    
    uint32_t request_timeout_ms_;
    size_t max_batch_size_;
    
    // Statistics
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> successful_requests_;
    std::atomic<uint64_t> failed_requests_;
    std::atomic<uint64_t> total_latency_ms_;
    
    // Helper methods
    bool authenticate_request(const CacheRequest& request);
    bool is_local_key(const MultiLevelKey& key);
    CacheResponse forward_request_to_owner(const CacheRequest& request, const MultiLevelKey& key);
    void update_request_stats(const CacheResponse& response, 
                             std::chrono::system_clock::time_point start_time);
    std::map<std::string, std::string> collect_stats(bool include_cluster, bool include_shards);
};

// Server interface
class CacheServer {
public:
    virtual ~CacheServer() = default;
    
    virtual Result<void> start(const std::string& address, uint16_t port) = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    virtual void set_service(std::shared_ptr<CacheService> service) = 0;
    virtual std::shared_ptr<CacheService> get_service() const = 0;
    
    // Server statistics
    virtual uint64_t get_active_connections() const = 0;
    virtual uint64_t get_total_requests() const = 0;
    virtual double get_average_latency_ms() const = 0;
};

// Client interface
class CacheClient {
public:
    virtual ~CacheClient() = default;
    
    virtual Result<void> connect(const std::string& address, uint16_t port) = 0;
    virtual Result<void> disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // Operations
    virtual GetResponse get(const MultiLevelKey& key, const std::string& auth_token = "") = 0;
    virtual PutResponse put(const MultiLevelKey& key, const DataEntry& data, 
                           const std::string& auth_token = "") = 0;
    virtual PutResponse put_if_version_match(const MultiLevelKey& key, const DataEntry& data,
                                           Version expected_version, const std::string& auth_token = "") = 0;
    virtual DeleteResponse remove(const MultiLevelKey& key, const std::string& auth_token = "") = 0;
    virtual DeleteResponse remove_if_version_match(const MultiLevelKey& key, Version expected_version,
                                                  const std::string& auth_token = "") = 0;
    virtual RangeScanResponse range_scan(const RangeQuery& query, const std::string& auth_token = "") = 0;
    virtual MultiGetResponse multi_get(const std::vector<MultiLevelKey>& keys, 
                                      const std::string& auth_token = "") = 0;
    virtual MultiPutResponse multi_put(const std::vector<std::pair<MultiLevelKey, DataEntry>>& entries,
                                      bool atomic = false, const std::string& auth_token = "") = 0;
    
    // Administrative operations
    virtual StatsResponse get_stats(bool include_cluster = false, bool include_shards = false,
                                   const std::string& auth_token = "") = 0;
    virtual CacheResponse ping() = 0;
    
    // Configuration
    virtual void set_timeout(uint32_t timeout_ms) = 0;
    virtual void set_retry_count(uint32_t retry_count) = 0;
};

} // namespace network
} // namespace cache