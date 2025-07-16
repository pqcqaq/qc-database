#include "cache/storage/wal.h"
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <crc32c/crc32c.h>

namespace cache {
namespace storage {

WAL::WAL(const std::string& filename) : filename_(filename), is_open_(false) {
}

WAL::~WAL() {
    if (is_open_) {
        close();
    }
}

Result<void> WAL::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_open_) {
        return Result<void>(Status::OK);
    }
    
    try {
        // 确保目录存在
        std::filesystem::path file_path(filename_);
        std::filesystem::create_directories(file_path.parent_path());
        
        // 以追加模式打开文件
        file_.open(filename_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_.is_open()) {
            // 文件不存在，创建新文件
            file_.open(filename_, std::ios::binary | std::ios::out);
            if (!file_.is_open()) {
                return Result<void>(Status::STORAGE_ERROR, "Failed to create WAL file: " + filename_);
            }
            file_.close();
            file_.open(filename_, std::ios::binary | std::ios::in | std::ios::out);
        }
        
        if (!file_.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to open WAL file: " + filename_);
        }
        
        // 移动到文件末尾准备追加
        file_.seekp(0, std::ios::end);
        current_offset_ = file_.tellp();
        
        is_open_ = true;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to open WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::OK);
    }
    
    try {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        
        is_open_ = false;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to close WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::append(const WALEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        // 序列化WAL条目
        std::vector<uint8_t> serialized_entry;
        auto serialize_result = serialize_entry(entry, serialized_entry);
        if (!serialize_result.is_ok()) {
            return serialize_result;
        }
        
        // 计算CRC32校验和
        uint32_t crc = crc32c::Crc32c(serialized_entry.data(), serialized_entry.size());
        
        // 写入记录头：记录长度 + CRC + 记录数据
        uint32_t record_length = serialized_entry.size();
        
        file_.write(reinterpret_cast<const char*>(&record_length), sizeof(record_length));
        file_.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
        file_.write(reinterpret_cast<const char*>(serialized_entry.data()), serialized_entry.size());
        
        // 更新当前偏移
        current_offset_ = file_.tellp();
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to append to WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        file_.flush();
        
        // 在某些系统上可能需要调用fsync
        #ifdef __linux__
        int fd = -1;
        // 获取文件描述符并调用fsync
        // 这里简化处理，实际实现可能需要更复杂的文件描述符管理
        #endif
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to sync WAL: " + std::string(e.what()));
    }
}

Result<std::vector<WALEntry>> WAL::replay_from(uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<std::vector<WALEntry>>(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    std::vector<WALEntry> entries;
    
    try {
        file_.seekg(offset);
        
        while (file_.good() && file_.tellg() < current_offset_) {
            // 读取记录头
            uint32_t record_length;
            uint32_t expected_crc;
            
            file_.read(reinterpret_cast<char*>(&record_length), sizeof(record_length));
            if (file_.gcount() != sizeof(record_length)) {
                break; // 文件结束
            }
            
            file_.read(reinterpret_cast<char*>(&expected_crc), sizeof(expected_crc));
            if (file_.gcount() != sizeof(expected_crc)) {
                break; // 文件结束或损坏
            }
            
            // 检查记录长度的合理性
            if (record_length > MAX_RECORD_SIZE) {
                return Result<std::vector<WALEntry>>(Status::STORAGE_ERROR, 
                    "Invalid record length in WAL: " + std::to_string(record_length));
            }
            
            // 读取记录数据
            std::vector<uint8_t> record_data(record_length);
            file_.read(reinterpret_cast<char*>(record_data.data()), record_length);
            if (static_cast<uint32_t>(file_.gcount()) != record_length) {
                return Result<std::vector<WALEntry>>(Status::STORAGE_ERROR, 
                    "Incomplete record in WAL");
            }
            
            // 验证CRC
            uint32_t actual_crc = crc32c::Crc32c(record_data.data(), record_data.size());
            if (actual_crc != expected_crc) {
                return Result<std::vector<WALEntry>>(Status::STORAGE_ERROR, 
                    "CRC mismatch in WAL record");
            }
            
            // 反序列化WAL条目
            WALEntry entry;
            auto deserialize_result = deserialize_entry(record_data, entry);
            if (!deserialize_result.is_ok()) {
                return Result<std::vector<WALEntry>>(deserialize_result.status, 
                    deserialize_result.error_message);
            }
            
            entries.push_back(entry);
        }
        
        return Result<std::vector<WALEntry>>(Status::OK, entries);
        
    } catch (const std::exception& e) {
        return Result<std::vector<WALEntry>>(Status::STORAGE_ERROR, 
            "Failed to replay WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::truncate_from(uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        // 关闭当前文件
        file_.close();
        
        // 重新打开文件进行截断
        std::fstream temp_file(filename_, std::ios::binary | std::ios::in | std::ios::out);
        if (!temp_file.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to reopen WAL for truncation");
        }
        
        // 读取要保留的数据
        temp_file.seekg(0);
        std::vector<char> data(offset);
        temp_file.read(data.data(), offset);
        temp_file.close();
        
        // 重新创建文件并写入保留的数据
        std::ofstream output_file(filename_, std::ios::binary | std::ios::trunc);
        if (!output_file.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to truncate WAL file");
        }
        
        if (offset > 0) {
            output_file.write(data.data(), offset);
        }
        output_file.close();
        
        // 重新打开文件
        file_.open(filename_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_.is_open()) {
            return Result<void>(Status::STORAGE_ERROR, "Failed to reopen WAL after truncation");
        }
        
        file_.seekp(0, std::ios::end);
        current_offset_ = file_.tellp();
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to truncate WAL: " + std::string(e.what()));
    }
}

uint64_t WAL::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_offset_;
}

bool WAL::is_open() const {
    return is_open_;
}

// 私有方法实现

Result<void> WAL::serialize_entry(const WALEntry& entry, std::vector<uint8_t>& buffer) {
    try {
        buffer.clear();
        
        // 序列化格式：
        // 1. Entry type (1 byte)
        // 2. Primary key length (4 bytes) + Primary key
        // 3. Secondary key length (4 bytes) + Secondary key  
        // 4. Value length (4 bytes) + Value
        // 5. Version (8 bytes)
        // 6. Timestamp (8 bytes)
        // 7. Deleted flag (1 byte)
        
        // Entry type
        uint8_t type_byte = static_cast<uint8_t>(entry.type);
        buffer.push_back(type_byte);
        
        // Primary key
        std::string primary_key = entry.key.primary_key;
        uint32_t primary_key_length = primary_key.size();
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&primary_key_length), sizeof(primary_key_length));
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(primary_key.c_str()), primary_key_length);
        
        // Secondary key
        std::string secondary_key = entry.key.secondary_key;
        uint32_t secondary_key_length = secondary_key.size();
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&secondary_key_length), sizeof(secondary_key_length));
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(secondary_key.c_str()), secondary_key_length);
        
        // Value
        uint32_t value_length = entry.data.value.size();
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&value_length), sizeof(value_length));
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(entry.data.value.c_str()), value_length);
        
        // Version
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&entry.data.version), sizeof(entry.data.version));
        
        // Timestamp
        uint64_t timestamp_ms = entry.data.timestamp.count();
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&timestamp_ms), sizeof(timestamp_ms));
        
        // Deleted flag
        uint8_t deleted_flag = entry.data.deleted ? 1 : 0;
        buffer.push_back(deleted_flag);
        
        // Transaction ID
        append_bytes(buffer, reinterpret_cast<const uint8_t*>(&entry.transaction_id), sizeof(entry.transaction_id));
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to serialize WAL entry: " + std::string(e.what()));
    }
}

Result<void> WAL::deserialize_entry(const std::vector<uint8_t>& buffer, WALEntry& entry) {
    try {
        if (buffer.empty()) {
            return Result<void>(Status::INVALID_ARGUMENT, "Empty buffer for WAL entry deserialization");
        }
        
        size_t offset = 0;
        
        // Entry type
        if (offset >= buffer.size()) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing type");
        }
        entry.type = static_cast<WALEntryType>(buffer[offset++]);
        
        // Primary key
        uint32_t primary_key_length;
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&primary_key_length), sizeof(primary_key_length))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing primary key length");
        }
        
        if (primary_key_length > config::MAX_KEY_SIZE) {
            return Result<void>(Status::STORAGE_ERROR, "Invalid primary key length in WAL entry");
        }
        
        entry.key.primary_key.resize(primary_key_length);
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&entry.key.primary_key[0]), primary_key_length)) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing primary key data");
        }
        
        // Secondary key
        uint32_t secondary_key_length;
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&secondary_key_length), sizeof(secondary_key_length))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing secondary key length");
        }
        
        if (secondary_key_length > config::MAX_KEY_SIZE) {
            return Result<void>(Status::STORAGE_ERROR, "Invalid secondary key length in WAL entry");
        }
        
        entry.key.secondary_key.resize(secondary_key_length);
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&entry.key.secondary_key[0]), secondary_key_length)) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing secondary key data");
        }
        
        // Value
        uint32_t value_length;
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&value_length), sizeof(value_length))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing value length");
        }
        
        if (value_length > config::MAX_VALUE_SIZE) {
            return Result<void>(Status::STORAGE_ERROR, "Invalid value length in WAL entry");
        }
        
        entry.data.value.resize(value_length);
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&entry.data.value[0]), value_length)) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing value data");
        }
        
        // Version
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&entry.data.version), sizeof(entry.data.version))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing version");
        }
        
        // Timestamp
        uint64_t timestamp_ms;
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&timestamp_ms), sizeof(timestamp_ms))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing timestamp");
        }
        entry.data.timestamp = Timestamp(timestamp_ms);
        
        // Deleted flag
        if (offset >= buffer.size()) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing deleted flag");
        }
        entry.data.deleted = (buffer[offset++] != 0);
        
        // Transaction ID
        if (!read_bytes(buffer, offset, reinterpret_cast<uint8_t*>(&entry.transaction_id), sizeof(entry.transaction_id))) {
            return Result<void>(Status::STORAGE_ERROR, "Incomplete WAL entry: missing transaction ID");
        }
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to deserialize WAL entry: " + std::string(e.what()));
    }
}

void WAL::append_bytes(std::vector<uint8_t>& buffer, const uint8_t* data, size_t length) {
    buffer.insert(buffer.end(), data, data + length);
}

bool WAL::read_bytes(const std::vector<uint8_t>& buffer, size_t& offset, uint8_t* data, size_t length) {
    if (offset + length > buffer.size()) {
        return false;
    }
    
    std::memcpy(data, buffer.data() + offset, length);
    offset += length;
    return true;
}

} // namespace storage
} // namespace cache