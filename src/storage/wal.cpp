#include "cache/storage/wal.h"
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <crc32c/crc32c.h>

namespace cache {
namespace storage {

void WALEntry::calculate_checksum() {
    auto serialized = serialize();
    checksum = crc32c::Crc32c(serialized.c_str(), serialized.size());
}

bool WALEntry::verify_checksum() const {
    auto entry_copy = *this;
    entry_copy.checksum = 0;
    auto calculated = crc32c::Crc32c(entry_copy.serialize().c_str(), entry_copy.serialize().size());
    return calculated == checksum;
}

std::string WALEntry::serialize() const {
    std::string result;
    result.push_back(static_cast<char>(type));
    
    // Serialize sequence number
    uint64_t seq = sequence_number;
    result.append(reinterpret_cast<const char*>(&seq), sizeof(seq));
    
    // Serialize key
    result.append(key.primary_key);
    result.push_back('\0');
    result.append(key.secondary_key);
    result.push_back('\0');
    
    // Serialize data
    result.append(data.value);
    
    return result;
}

Result<WALEntry> WALEntry::deserialize(const std::string& data) {
    if (data.empty()) {
        return Result<WALEntry>::error(Status::INVALID_ARGUMENT, "Empty data");
    }
    
    WALEntry entry;
    size_t pos = 0;
    
    // Deserialize type
    if (pos >= data.size()) {
        return Result<WALEntry>::error(Status::INVALID_ARGUMENT, "Invalid data format");
    }
    entry.type = static_cast<WALEntryType>(data[pos++]);
    
    // Deserialize sequence number
    if (pos + sizeof(uint64_t) > data.size()) {
        return Result<WALEntry>::error(Status::INVALID_ARGUMENT, "Invalid data format");
    }
    std::memcpy(&entry.sequence_number, data.data() + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
    
    // Deserialize primary key
    size_t null_pos = data.find('\0', pos);
    if (null_pos == std::string::npos) {
        return Result<WALEntry>::error(Status::INVALID_ARGUMENT, "Invalid key format");
    }
    entry.key.primary_key = data.substr(pos, null_pos - pos);
    pos = null_pos + 1;
    
    // Deserialize secondary key
    null_pos = data.find('\0', pos);
    if (null_pos == std::string::npos) {
        return Result<WALEntry>::error(Status::INVALID_ARGUMENT, "Invalid key format");
    }
    entry.key.secondary_key = data.substr(pos, null_pos - pos);
    pos = null_pos + 1;
    
    // Deserialize value
    entry.data.value = data.substr(pos);
    
    return Result<WALEntry>(Status::OK, entry);
}

WAL::WAL(const std::string& filename) 
    : wal_file_(filename), file_(std::make_unique<std::fstream>()), 
      is_open_(false), sequence_number_(0) {
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
        std::filesystem::path file_path(wal_file_);
        std::filesystem::create_directories(file_path.parent_path());
        
        file_->open(wal_file_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_->is_open()) {
            file_->open(wal_file_, std::ios::binary | std::ios::out);
            if (!file_->is_open()) {
                return Result<void>::error(Status::STORAGE_ERROR, "Failed to create WAL file: " + wal_file_);
            }
            file_->close();
            file_->open(wal_file_, std::ios::binary | std::ios::in | std::ios::out);
        }
        
        if (!file_->is_open()) {
            return Result<void>::error(Status::STORAGE_ERROR, "Failed to open WAL file: " + wal_file_);
        }
        
        file_->seekp(0, std::ios::end);
        is_open_ = true;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>::error(Status::STORAGE_ERROR, "Failed to open WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::OK);
    }
    
    try {
        if (file_->is_open()) {
            file_->flush();
            file_->close();
        }
        
        is_open_ = false;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>::error(Status::STORAGE_ERROR, "Failed to close WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::append(const WALEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>::error(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        auto serialized_entry = entry.serialize();
        uint32_t record_length = serialized_entry.size();
        uint32_t crc = crc32c::Crc32c(serialized_entry.c_str(), serialized_entry.size());
        
        file_->write(reinterpret_cast<const char*>(&record_length), sizeof(record_length));
        file_->write(reinterpret_cast<const char*>(&crc), sizeof(crc));
        file_->write(serialized_entry.c_str(), serialized_entry.size());
        
        sequence_number_++;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>::error(Status::STORAGE_ERROR, "Failed to append to WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>::error(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        file_->flush();
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>::error(Status::STORAGE_ERROR, "Failed to sync WAL: " + std::string(e.what()));
    }
}

Result<std::vector<WALEntry>> WAL::replay_from(uint64_t sequence_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    std::vector<WALEntry> entries;
    
    try {
        file_->seekg(0);
        
        while (file_->good()) {
            uint32_t record_length;
            uint32_t expected_crc;
            
            file_->read(reinterpret_cast<char*>(&record_length), sizeof(record_length));
            if (file_->gcount() != sizeof(record_length)) {
                break;
            }
            
            file_->read(reinterpret_cast<char*>(&expected_crc), sizeof(expected_crc));
            if (file_->gcount() != sizeof(expected_crc)) {
                break;
            }
            
            if (record_length > 1024 * 1024) { // Max 1MB per record
                return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR,
                    "Invalid record length in WAL: " + std::to_string(record_length));
            }
            
            std::string record_data(record_length, '\0');
            file_->read(&record_data[0], record_length);
            if (static_cast<uint32_t>(file_->gcount()) != record_length) {
                return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR,
                    "Incomplete record in WAL");
            }
            
            uint32_t actual_crc = crc32c::Crc32c(record_data.c_str(), record_data.size());
            if (actual_crc != expected_crc) {
                return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR,
                    "CRC mismatch in WAL record");
            }
            
            auto deserialize_result = WALEntry::deserialize(record_data);
            if (!deserialize_result.is_ok()) {
                return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR,
                    "Failed to deserialize WAL entry");
            }
            
            if (deserialize_result.data.sequence_number >= sequence_number) {
                entries.push_back(deserialize_result.data);
            }
        }
        
        return Result<std::vector<WALEntry>>(Status::OK, entries);
        
    } catch (const std::exception& e) {
        return Result<std::vector<WALEntry>>::error(Status::STORAGE_ERROR,
            "Failed to replay WAL: " + std::string(e.what()));
    }
}

Result<void> WAL::truncate(uint64_t sequence_number) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>::error(Status::STORAGE_ERROR, "WAL is not open");
    }
    
    try {
        // Simple truncation - rewrite file with entries after sequence_number
        auto replay_result = replay_from(sequence_number);
        if (!replay_result.is_ok()) {
            return Result<void>::error(Status::STORAGE_ERROR, replay_result.error_message);
        }
        
        file_->close();
        
        std::ofstream output_file(wal_file_, std::ios::binary | std::ios::trunc);
        if (!output_file.is_open()) {
            return Result<void>::error(Status::STORAGE_ERROR, "Failed to truncate WAL file");
        }
        
        for (const auto& entry : replay_result.data) {
            auto serialized = entry.serialize();
            uint32_t record_length = serialized.size();
            uint32_t crc = crc32c::Crc32c(serialized.c_str(), serialized.size());
            
            output_file.write(reinterpret_cast<const char*>(&record_length), sizeof(record_length));
            output_file.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
            output_file.write(serialized.c_str(), serialized.size());
        }
        
        output_file.close();
        
        file_->open(wal_file_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_->is_open()) {
            return Result<void>::error(Status::STORAGE_ERROR, "Failed to reopen WAL after truncation");
        }
        
        file_->seekp(0, std::ios::end);
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>::error(Status::STORAGE_ERROR, "Failed to truncate WAL: " + std::string(e.what()));
    }
}

size_t WAL::file_size() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    if (!is_open_ || !file_) return 0;
    auto pos = file_->tellp();
    return pos >= 0 ? static_cast<size_t>(pos) : 0;
}

uint32_t WAL::calculate_crc32(const void* data, size_t length) const {
    return crc32c::Crc32c(reinterpret_cast<const char*>(data), length);
}

} // namespace storage
} // namespace cache