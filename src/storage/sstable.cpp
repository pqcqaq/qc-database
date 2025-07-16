#include "cache/storage/lsm_tree.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <filesystem>

namespace cache {
namespace storage {

SSTable::SSTable(const std::string& filename) : filename_(filename), size_(0) {
}

SSTable::~SSTable() {
}

Result<void> SSTable::write(const std::map<MultiLevelKey, DataEntry>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (data.empty()) {
        return Result<void>(Status::INVALID_ARGUMENT, "Cannot write empty data");
    }
    
    try {
        // 确保目录存在
        std::filesystem::path file_path(filename_);
        std::filesystem::create_directories(file_path.parent_path());
        
        // 打开文件进行写入
        std::ofstream file(filename_, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to open SSTable file for writing: " + filename_);
        }
        
        // 写入头部信息
        uint32_t magic_number = 0x5354414C; // "STAL" = SSTable
        uint32_t version = 1;
        uint32_t entry_count = data.size();
        
        file.write(reinterpret_cast<const char*>(&magic_number), sizeof(magic_number));
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        file.write(reinterpret_cast<const char*>(&entry_count), sizeof(entry_count));
        
        // 数据区起始位置
        uint64_t data_offset = file.tellp();
        uint64_t current_offset = data_offset;
        
        index_.clear();
        
        // 写入数据并构建索引
        for (const auto& [key, entry] : data) {
            IndexEntry index_entry;
            index_entry.key = key;
            index_entry.offset = current_offset;
            
            // 序列化键
            std::string key_str = key.to_string();
            uint32_t key_size = key_str.size();
            file.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            file.write(key_str.c_str(), key_size);
            
            // 序列化数据
            uint32_t value_size = entry.value.size();
            file.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
            file.write(entry.value.c_str(), value_size);
            file.write(reinterpret_cast<const char*>(&entry.version), sizeof(entry.version));
            
            auto timestamp_ms = entry.timestamp.count();
            file.write(reinterpret_cast<const char*>(&timestamp_ms), sizeof(timestamp_ms));
            
            uint8_t deleted_flag = entry.deleted ? 1 : 0;
            file.write(reinterpret_cast<const char*>(&deleted_flag), sizeof(deleted_flag));
            
            index_entry.size = static_cast<uint64_t>(file.tellp()) - current_offset;
            current_offset = file.tellp();
            
            index_.push_back(index_entry);
        }
        
        // 写入索引
        uint64_t index_offset = file.tellp();
        uint32_t index_size = index_.size();
        file.write(reinterpret_cast<const char*>(&index_size), sizeof(index_size));
        
        for (const auto& entry : index_) {
            // 写入键
            std::string key_str = entry.key.to_string();
            uint32_t key_size = key_str.size();
            file.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
            file.write(key_str.c_str(), key_size);
            
            // 写入偏移和大小
            file.write(reinterpret_cast<const char*>(&entry.offset), sizeof(entry.offset));
            file.write(reinterpret_cast<const char*>(&entry.size), sizeof(entry.size));
        }
        
        // 写入尾部信息（索引偏移）
        file.write(reinterpret_cast<const char*>(&index_offset), sizeof(index_offset));
        
        file.flush();
        file.close();
        
        size_ = data.size();
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to write SSTable: " + std::string(e.what()));
    }
}

Result<DataEntry> SSTable::read(const MultiLevelKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 确保索引已加载
    if (index_.empty()) {
        auto load_result = load_index();
        if (!load_result.is_ok()) {
            return Result<DataEntry>(load_result.status, load_result.error_message);
        }
    }
    
    // 二分查找索引
    auto it = std::lower_bound(index_.begin(), index_.end(), key,
        [](const IndexEntry& entry, const MultiLevelKey& k) {
            return entry.key < k;
        });
    
    if (it == index_.end() || it->key != key) {
        return Result<DataEntry>(Status::NOT_FOUND, "Key not found in SSTable");
    }
    
    try {
        std::ifstream file(filename_, std::ios::binary);
        if (!file.is_open()) {
            return Result<DataEntry>(Status::STORAGE_ERROR, "Failed to open SSTable file for reading");
        }
        
        // 跳转到数据位置
        file.seekg(it->offset);
        
        // 读取键（验证）
        uint32_t key_size;
        file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
        std::string read_key(key_size, '\0');
        file.read(&read_key[0], key_size);
        
        if (read_key != key.to_string()) {
            return Result<DataEntry>(Status::STORAGE_ERROR, "Key mismatch in SSTable");
        }
        
        // 读取数据
        DataEntry entry;
        uint32_t value_size;
        file.read(reinterpret_cast<char*>(&value_size), sizeof(value_size));
        entry.value.resize(value_size);
        file.read(&entry.value[0], value_size);
        
        file.read(reinterpret_cast<char*>(&entry.version), sizeof(entry.version));
        
        uint64_t timestamp_ms;
        file.read(reinterpret_cast<char*>(&timestamp_ms), sizeof(timestamp_ms));
        entry.timestamp = Timestamp(timestamp_ms);
        
        uint8_t deleted_flag;
        file.read(reinterpret_cast<char*>(&deleted_flag), sizeof(deleted_flag));
        entry.deleted = (deleted_flag != 0);
        
        file.close();
        
        if (entry.deleted) {
            return Result<DataEntry>(Status::NOT_FOUND, "Key is marked as deleted");
        }
        
        return Result<DataEntry>(Status::OK, entry);
        
    } catch (const std::exception& e) {
        return Result<DataEntry>(Status::STORAGE_ERROR, "Failed to read from SSTable: " + std::string(e.what()));
    }
}

Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
SSTable::range_scan(const RangeQuery& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 确保索引已加载
    if (index_.empty()) {
        auto load_result = load_index();
        if (!load_result.is_ok()) {
            return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
                load_result.status, load_result.error_message);
        }
    }
    
    std::vector<std::pair<MultiLevelKey, DataEntry>> results;
    
    try {
        std::ifstream file(filename_, std::ios::binary);
        if (!file.is_open()) {
            return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
                Status::STORAGE_ERROR, "Failed to open SSTable file for reading");
        }
        
        // 找到范围的起始和结束位置
        auto start_it = std::lower_bound(index_.begin(), index_.end(), query.start_key,
            [](const IndexEntry& entry, const MultiLevelKey& k) {
                return entry.key < k;
            });
        
        auto end_it = std::upper_bound(index_.begin(), index_.end(), query.end_key,
            [](const MultiLevelKey& k, const IndexEntry& entry) {
                return k < entry.key;
            });
        
        if (!query.inclusive_start && start_it != index_.end() && start_it->key == query.start_key) {
            ++start_it;
        }
        
        if (query.inclusive_end && end_it != index_.begin()) {
            auto prev_it = end_it - 1;
            if (prev_it->key == query.end_key) {
                end_it = prev_it + 1;
            }
        }
        
        uint32_t count = 0;
        for (auto it = start_it; it != end_it && count < query.limit; ++it, ++count) {
            // 读取数据
            file.seekg(it->offset);
            
            // 跳过键
            uint32_t key_size;
            file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
            file.seekg(key_size, std::ios::cur);
            
            // 读取值
            DataEntry entry;
            uint32_t value_size;
            file.read(reinterpret_cast<char*>(&value_size), sizeof(value_size));
            entry.value.resize(value_size);
            file.read(&entry.value[0], value_size);
            
            file.read(reinterpret_cast<char*>(&entry.version), sizeof(entry.version));
            
            uint64_t timestamp_ms;
            file.read(reinterpret_cast<char*>(&timestamp_ms), sizeof(timestamp_ms));
            entry.timestamp = Timestamp(timestamp_ms);
            
            uint8_t deleted_flag;
            file.read(reinterpret_cast<char*>(&deleted_flag), sizeof(deleted_flag));
            entry.deleted = (deleted_flag != 0);
            
            if (!entry.deleted) {
                results.emplace_back(it->key, entry);
            }
        }
        
        file.close();
        
        return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(Status::OK, results);
        
    } catch (const std::exception& e) {
        return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
            Status::STORAGE_ERROR, "Failed to scan SSTable: " + std::string(e.what()));
    }
}

Result<void> SSTable::build_index() {
    return load_index();
}

Result<void> SSTable::load_index() {
    try {
        std::ifstream file(filename_, std::ios::binary);
        if (!file.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to open SSTable file");
        }
        
        // 验证文件头
        uint32_t magic_number, version, entry_count;
        file.read(reinterpret_cast<char*>(&magic_number), sizeof(magic_number));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&entry_count), sizeof(entry_count));
        
        if (magic_number != 0x5354414C) {
            return Result<void>(Status::STORAGE_ERROR, "Invalid SSTable magic number");
        }
        
        if (version != 1) {
            return Result<void>(Status::STORAGE_ERROR, "Unsupported SSTable version");
        }
        
        // 读取索引偏移
        file.seekg(-static_cast<int>(sizeof(uint64_t)), std::ios::end);
        uint64_t index_offset;
        file.read(reinterpret_cast<char*>(&index_offset), sizeof(index_offset));
        
        // 读取索引
        file.seekg(index_offset);
        uint32_t index_size;
        file.read(reinterpret_cast<char*>(&index_size), sizeof(index_size));
        
        index_.clear();
        index_.reserve(index_size);
        
        for (uint32_t i = 0; i < index_size; ++i) {
            IndexEntry entry;
            
            // 读取键
            uint32_t key_size;
            file.read(reinterpret_cast<char*>(&key_size), sizeof(key_size));
            std::string key_str(key_size, '\0');
            file.read(&key_str[0], key_size);
            
            // 解析多级键
            size_t colon_pos = key_str.find(':');
            if (colon_pos != std::string::npos) {
                entry.key.primary_key = key_str.substr(0, colon_pos);
                entry.key.secondary_key = key_str.substr(colon_pos + 1);
            } else {
                entry.key.primary_key = key_str;
            }
            
            // 读取偏移和大小
            file.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
            file.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));
            
            index_.push_back(entry);
        }
        
        file.close();
        size_ = index_.size();
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to load SSTable index: " + std::string(e.what()));
    }
}

Result<void> SSTable::save_index() {
    // 索引在写入数据时已经保存到文件中
    return Result<void>(Status::OK);
}

} // namespace storage
} // namespace cache