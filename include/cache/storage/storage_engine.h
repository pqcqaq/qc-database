#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>

namespace cache {
namespace storage {

class StorageEngine {
public:
    virtual ~StorageEngine() = default;
    
    // Basic CRUD operations
    virtual Result<DataEntry> get(const MultiLevelKey& key) = 0;
    virtual Result<void> put(const MultiLevelKey& key, const DataEntry& entry) = 0;
    virtual Result<void> put_if_version_match(const MultiLevelKey& key, const DataEntry& entry, Version expected_version) = 0;
    virtual Result<void> remove(const MultiLevelKey& key) = 0;
    virtual Result<void> remove_if_version_match(const MultiLevelKey& key, Version expected_version) = 0;
    
    // Range operations
    virtual Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
        range_scan(const RangeQuery& query) = 0;
    
    // Batch operations
    virtual Result<std::vector<DataEntry>> 
        multi_get(const std::vector<MultiLevelKey>& keys) = 0;
    virtual Result<void> 
        multi_put(const std::vector<std::pair<MultiLevelKey, DataEntry>>& entries) = 0;
    
    // Persistence operations
    virtual Result<void> flush() = 0;
    virtual Result<void> compact() = 0;
    virtual Result<void> backup(const std::string& backup_path) = 0;
    virtual Result<void> restore(const std::string& backup_path) = 0;
    
    // Statistics
    virtual size_t size() const = 0;
    virtual size_t memory_usage() const = 0;
    virtual double cache_hit_ratio() const = 0;
    
    // Lifecycle
    virtual Result<void> open() = 0;
    virtual Result<void> close() = 0;
    virtual bool is_open() const = 0;
};

// Factory for creating storage engines
class StorageEngineFactory {
public:
    enum class EngineType {
        LSM_TREE,
        B_PLUS_TREE,
        HASH_TABLE
    };
    
    static std::unique_ptr<StorageEngine> create_engine(
        EngineType type, 
        const std::string& data_dir,
        const std::map<std::string, std::string>& config = {});
};

} // namespace storage
} // namespace cache