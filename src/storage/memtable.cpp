#include "cache/storage/lsm_tree.h"
#include <algorithm>
#include <shared_mutex>

namespace cache {
namespace storage {

MemTable::MemTable() : memory_usage_(0) {
}

Result<DataEntry> MemTable::get(const MultiLevelKey& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = data_.find(key);
    if (it == data_.end()) {
        return Result<DataEntry>::error(Status::NOT_FOUND, "Key not found in memtable");
    }
    
    if (it->second.deleted) {
        return Result<DataEntry>::error(Status::NOT_FOUND, "Key is marked as deleted");
    }
    
    return Result<DataEntry>(Status::OK, it->second);
}

Result<void> MemTable::put(const MultiLevelKey& key, const DataEntry& entry) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // 检查key和value大小限制
    if (key.to_string().size() > config::MAX_KEY_SIZE) {
        return Result<void>::error(Status::INVALID_ARGUMENT, "Key size exceeds limit");
    }
    
    if (entry.value.size() > config::MAX_VALUE_SIZE) {
        return Result<void>::error(Status::INVALID_ARGUMENT, "Value size exceeds limit");
    }
    
    // 计算内存使用变化
    size_t new_entry_size = key.to_string().size() + entry.value.size() + sizeof(DataEntry);
    size_t old_entry_size = 0;
    
    auto it = data_.find(key);
    if (it != data_.end()) {
        old_entry_size = key.to_string().size() + it->second.value.size() + sizeof(DataEntry);
    }
    
    // 更新数据
    data_[key] = entry;
    
    // 更新内存使用统计
    memory_usage_ += new_entry_size;
    memory_usage_ -= old_entry_size;
    
    return Result<void>(Status::OK);
}

Result<void> MemTable::remove(const MultiLevelKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = data_.find(key);
    if (it == data_.end()) {
        // 即使key不存在，也要插入删除标记
        DataEntry delete_entry;
        delete_entry.deleted = true;
        delete_entry.timestamp = std::chrono::duration_cast<Timestamp>(
            std::chrono::system_clock::now().time_since_epoch());
        
        data_[key] = delete_entry;
        memory_usage_ += key.to_string().size() + sizeof(DataEntry);
        
        return Result<void>(Status::OK);
    }
    
    // 标记为删除
    it->second.deleted = true;
    it->second.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch());
    
    return Result<void>(Status::OK);
}

Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
MemTable::range_scan(const RangeQuery& query) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<std::pair<MultiLevelKey, DataEntry>> results;
    
    // 使用map的有序特性进行范围查询
    auto start_it = data_.lower_bound(query.start_key);
    auto end_it = data_.upper_bound(query.end_key);
    
    if (!query.inclusive_start && start_it != data_.end() && start_it->first == query.start_key) {
        ++start_it;
    }
    
    if (query.inclusive_end && end_it != data_.end() && end_it->first == query.end_key) {
        ++end_it;
    }
    
    uint32_t count = 0;
    for (auto it = start_it; it != end_it && count < query.limit; ++it, ++count) {
        if (!it->second.deleted) {
            results.emplace_back(it->first, it->second);
        }
    }
    
    return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(Status::OK, results);
}

size_t MemTable::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_.size();
}

size_t MemTable::memory_usage() const {
    return memory_usage_.load();
}

bool MemTable::should_flush() const {
    return memory_usage() >= config::MEMTABLE_SIZE_LIMIT;
}

std::map<MultiLevelKey, DataEntry> MemTable::get_all_data() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return data_;
}

void MemTable::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    data_.clear();
    memory_usage_ = 0;
}

} // namespace storage
} // namespace cache