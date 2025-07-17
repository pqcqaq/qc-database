#include <filesystem>
#include <iostream>

#include "cache/storage/lsm_tree.h"
#include "cache/storage/storage_engine.h"

namespace cache {
namespace storage {

std::unique_ptr<StorageEngine> StorageEngineFactory::create_engine(
    EngineType type, const std::string& data_dir,
    const std::map<std::string, std::string>& config) {
    // Ensure data directory exists
    std::filesystem::create_directories(data_dir);

    switch (type) {
        case EngineType::LSM_TREE:
            return std::make_unique<LSMTreeEngine>(data_dir, config);

        case EngineType::B_PLUS_TREE:
            // B+ tree implementation would go here
            // For now, fall back to LSM tree
            std::cerr << "B+ tree engine not implemented, using LSM tree\n";
            return std::make_unique<LSMTreeEngine>(data_dir, config);

        case EngineType::HASH_TABLE:
            // Hash table implementation would go here
            // For now, fall back to LSM tree
            std::cerr << "Hash table engine not implemented, using LSM tree\n";
            return std::make_unique<LSMTreeEngine>(data_dir, config);

        default:
            throw std::invalid_argument("Unknown storage engine type");
    }
}

}  // namespace storage
}  // namespace cache