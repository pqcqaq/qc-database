#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace cache {

// Basic types
using Key = std::string;
using Value = std::string;
using Version = uint64_t;
using NodeId = std::string;
using ShardId = uint32_t;
using Timestamp = std::chrono::milliseconds;

// Multi-level key support
struct MultiLevelKey {
    Key primary_key;
    Key secondary_key;
    
    MultiLevelKey() = default;
    MultiLevelKey(const Key& pk, const Key& sk) : primary_key(pk), secondary_key(sk) {}
    
    std::string to_string() const {
        return primary_key + ":" + secondary_key;
    }
    
    bool operator==(const MultiLevelKey& other) const {
        return primary_key == other.primary_key && secondary_key == other.secondary_key;
    }
    
    bool operator<(const MultiLevelKey& other) const {
        if (primary_key != other.primary_key) {
            return primary_key < other.primary_key;
        }
        return secondary_key < other.secondary_key;
    }
};

// Hash function for MultiLevelKey
struct MultiLevelKeyHash {
    std::size_t operator()(const MultiLevelKey& key) const {
        std::size_t h1 = std::hash<std::string>{}(key.primary_key);
        std::size_t h2 = std::hash<std::string>{}(key.secondary_key);
        return h1 ^ (h2 << 1);
    }
};

// Data entry with version and timestamp
struct DataEntry {
    Value value;
    Version version;
    Timestamp timestamp;
    bool deleted;
    
    DataEntry() : version(0), timestamp(std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch())), deleted(false) {}
    
    DataEntry(const Value& val, Version ver = 1) 
        : value(val), version(ver), 
          timestamp(std::chrono::duration_cast<Timestamp>(
              std::chrono::system_clock::now().time_since_epoch())), 
          deleted(false) {}
};

// Operation types
enum class OperationType {
    GET,
    PUT,
    DELETE,
    RANGE_SCAN,
    MULTI_GET,
    MULTI_PUT
};

// Result status
enum class Status {
    OK,
    NOT_FOUND,
    VERSION_CONFLICT,
    TIMEOUT,
    NETWORK_ERROR,
    STORAGE_ERROR,
    PERMISSION_DENIED,
    INVALID_ARGUMENT,
    CLUSTER_ERROR,
    ALREADY_EXISTS,
    NOT_IMPLEMENTED,
    SERVICE_UNAVAILABLE,
    UNAUTHORIZED,
    FORBIDDEN,
    DEADLOCK_DETECTED
};

// Range query parameters
struct RangeQuery {
    MultiLevelKey start_key;
    MultiLevelKey end_key;
    uint32_t limit;
    bool inclusive_start;
    bool inclusive_end;
    
    RangeQuery() : limit(1000), inclusive_start(true), inclusive_end(false) {}
};

// Operation result
template<typename T>
struct Result {
    Status status;
    T data;
    std::string error_message;
    
    Result(Status s = Status::OK) : status(s) {}
    Result(Status s, const T& d) : status(s), data(d) {}
    
    // Named constructor for error cases to avoid ambiguity
    static Result error(Status s, const std::string& err) {
        Result result(s);
        result.error_message = err;
        return result;
    }
    
    bool is_ok() const { return status == Status::OK; }
    bool is_error() const { return status != Status::OK; }
};

// Specialization for void
template<>
struct Result<void> {
    Status status;
    std::string error_message;
    
    Result(Status s = Status::OK) : status(s) {}
    
    // Named constructor for error cases
    static Result error(Status s, const std::string& err) {
        Result result(s);
        result.error_message = err;
        return result;
    }
    
    bool is_ok() const { return status == Status::OK; }
    bool is_error() const { return status != Status::OK; }
};

// Configuration constants
namespace config {
    static constexpr size_t MAX_KEY_SIZE = 256;
    static constexpr size_t MAX_VALUE_SIZE = 4 * 1024;  // 4KB
    static constexpr size_t DEFAULT_SHARD_COUNT = 1024;
    static constexpr size_t DEFAULT_REPLICATION_FACTOR = 3;
    static constexpr uint32_t DEFAULT_HEARTBEAT_INTERVAL_MS = 1000;
    static constexpr uint32_t DEFAULT_ELECTION_TIMEOUT_MS = 5000;
    static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT_MS = 100;
    static constexpr uint32_t WAL_SYNC_INTERVAL_MS = 100;
    static constexpr size_t MEMTABLE_SIZE_LIMIT = 64 * 1024 * 1024;  // 64MB
}

} // namespace cache