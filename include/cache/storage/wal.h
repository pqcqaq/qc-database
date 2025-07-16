#pragma once

#include "cache/common/types.h"
#include <string>
#include <memory>
#include <mutex>
#include <fstream>
#include <atomic>

namespace cache {
namespace storage {

// WAL entry types
enum class WALEntryType : uint8_t {
    PUT = 1,
    DELETE = 2,
    FLUSH = 3,
    CHECKPOINT = 4
};

// WAL entry structure
struct WALEntry {
    WALEntryType type;
    MultiLevelKey key;
    DataEntry data;
    uint64_t sequence_number;
    uint32_t checksum;
    
    WALEntry() = default;
    WALEntry(WALEntryType t, const MultiLevelKey& k, const DataEntry& d, uint64_t seq)
        : type(t), key(k), data(d), sequence_number(seq), checksum(0) {
        calculate_checksum();
    }
    
    void calculate_checksum();
    bool verify_checksum() const;
    std::string serialize() const;
    static Result<WALEntry> deserialize(const std::string& data);
};

// Write-Ahead Log implementation
class WAL {
public:
    explicit WAL(const std::string& wal_file);
    ~WAL();
    
    Result<void> open();
    Result<void> close();
    bool is_open() const { return is_open_; }
    
    // WAL operations
    Result<void> append(const WALEntry& entry);
    Result<void> sync();
    Result<void> truncate(uint64_t sequence_number);
    
    // Recovery
    Result<std::vector<WALEntry>> replay_from(uint64_t sequence_number = 0);
    
    // Statistics
    uint64_t current_sequence_number() const { return sequence_number_; }
    size_t file_size() const;
    
private:
    std::string wal_file_;
    std::unique_ptr<std::fstream> file_;
    std::mutex mutex_;
    std::atomic<bool> is_open_;
    std::atomic<uint64_t> sequence_number_;
    
    Result<void> write_entry(const WALEntry& entry);
    Result<WALEntry> read_entry();
    uint32_t calculate_crc32(const void* data, size_t length) const;
};

} // namespace storage
} // namespace cache