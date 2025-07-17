#pragma once

#include "cache/storage/storage_engine.h"
#include "cache/storage/wal.h"
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

namespace cache {
namespace storage {

// SSTable for persistent storage
class SSTable {
public:
    struct IndexEntry {
        MultiLevelKey key;
        uint64_t offset;
        uint32_t size;
    };
    
    SSTable(const std::string& filename);
    ~SSTable();
    
    Result<void> write(const std::map<MultiLevelKey, DataEntry>& data);
    Result<DataEntry> read(const MultiLevelKey& key);
    Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
        range_scan(const RangeQuery& query);
    
    size_t size() const { return size_; }
    const std::string& filename() const { return filename_; }
    
private:
    std::string filename_;
    std::vector<IndexEntry> index_;
    std::atomic<size_t> size_;
    std::mutex mutex_;
    
    Result<void> build_index();
    Result<void> load_index();
    Result<void> save_index();
};

// In-memory table (MemTable)
class MemTable {
public:
    MemTable();
    ~MemTable() = default;
    
    Result<DataEntry> get(const MultiLevelKey& key);
    Result<void> put(const MultiLevelKey& key, const DataEntry& entry);
    Result<void> remove(const MultiLevelKey& key);
    
    Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
        range_scan(const RangeQuery& query);
    
    size_t size() const;
    size_t memory_usage() const;
    bool should_flush() const;
    
    std::map<MultiLevelKey, DataEntry> get_all_data() const;
    void clear();
    
private:
    std::map<MultiLevelKey, DataEntry> data_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> memory_usage_;
};

// LSM Tree storage engine implementation
class LSMTreeEngine : public StorageEngine {
public:
    explicit LSMTreeEngine(const std::string& data_dir, 
                          const std::map<std::string, std::string>& config = {});
    ~LSMTreeEngine() override;
    
    // StorageEngine interface implementation
    Result<DataEntry> get(const MultiLevelKey& key) override;
    Result<void> put(const MultiLevelKey& key, const DataEntry& entry) override;
    Result<void> put_if_version_match(const MultiLevelKey& key, const DataEntry& entry, Version expected_version) override;
    Result<void> remove(const MultiLevelKey& key) override;
    Result<void> remove_if_version_match(const MultiLevelKey& key, Version expected_version) override;
    
    Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
        range_scan(const RangeQuery& query) override;
    
    Result<std::vector<DataEntry>> 
        multi_get(const std::vector<MultiLevelKey>& keys) override;
    Result<void> 
        multi_put(const std::vector<std::pair<MultiLevelKey, DataEntry>>& entries) override;
    
    Result<void> flush() override;
    Result<void> compact() override;
    Result<void> backup(const std::string& backup_path) override;
    Result<void> restore(const std::string& backup_path) override;
    
    size_t size() const override;
    size_t memory_usage() const override;
    double cache_hit_ratio() const override;
    
    Result<void> open() override;
    Result<void> close() override;
    bool is_open() const override;
    
private:
    std::string data_dir_;
    std::unique_ptr<MemTable> memtable_;
    std::unique_ptr<MemTable> immutable_memtable_;
    std::vector<std::unique_ptr<SSTable>> sstables_;
    std::unique_ptr<WAL> wal_;
    
    mutable std::shared_mutex mutex_;
    std::mutex flush_mutex_;
    std::condition_variable flush_cv_;
    std::thread background_thread_;
    std::atomic<bool> stop_background_;
    std::atomic<bool> is_open_;
    
    // Statistics
    mutable std::atomic<uint64_t> total_reads_;
    mutable std::atomic<uint64_t> cache_hits_;
    
    // Configuration
    size_t memtable_size_limit_;
    size_t max_sstables_;
    bool enable_compression_;
    
    // Private methods
    void background_work();
    Result<void> flush_memtable();
    Result<void> compact_sstables();
    Result<void> recover_from_wal();
    Result<void> load_sstables();
    
    Result<DataEntry> get_from_memtable(const MultiLevelKey& key);
    Result<DataEntry> get_from_sstables(const MultiLevelKey& key);
    
    std::string generate_sstable_filename();
    bool need_compaction() const;
};

} // namespace storage
} // namespace cache