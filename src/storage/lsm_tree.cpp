#include "cache/storage/lsm_tree.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>

namespace cache {
namespace storage {

LSMTreeEngine::LSMTreeEngine(const std::string& data_dir, 
                             const std::map<std::string, std::string>& config)
    : data_dir_(data_dir), is_open_(false), stop_background_(false),
      total_reads_(0), cache_hits_(0) {
    
    // 解析配置
    memtable_size_limit_ = config::MEMTABLE_SIZE_LIMIT;
    max_sstables_ = 10;
    enable_compression_ = false;
    
    auto it = config.find("memtable_size_mb");
    if (it != config.end()) {
        memtable_size_limit_ = std::stoull(it->second) * 1024 * 1024;
    }
    
    it = config.find("max_sstables");
    if (it != config.end()) {
        max_sstables_ = std::stoull(it->second);
    }
    
    it = config.find("enable_compression");
    if (it != config.end()) {
        enable_compression_ = (it->second == "true");
    }
    
    // 初始化组件
    memtable_ = std::make_unique<MemTable>();
    wal_ = std::make_unique<WAL>(data_dir_ + "/wal.log");
}

LSMTreeEngine::~LSMTreeEngine() {
    if (is_open_) {
        close();
    }
}

Result<void> LSMTreeEngine::open() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (is_open_) {
        return Result<void>(Status::OK);
    }
    
    try {
        // 确保数据目录存在
        std::filesystem::create_directories(data_dir_);
        
        // 打开WAL
        auto wal_result = wal_->open();
        if (!wal_result.is_ok()) {
            return wal_result;
        }
        
        // 从WAL恢复数据
        auto recovery_result = recover_from_wal();
        if (!recovery_result.is_ok()) {
            return recovery_result;
        }
        
        // 加载现有的SSTable
        auto load_result = load_sstables();
        if (!load_result.is_ok()) {
            return load_result;
        }
        
        // 启动后台线程
        stop_background_ = false;
        background_thread_ = std::thread(&LSMTreeEngine::background_work, this);
        
        is_open_ = true;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to open LSM tree: " + std::string(e.what()));
    }
}

Result<void> LSMTreeEngine::close() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::OK);
    }
    
    try {
        // 停止后台线程
        stop_background_ = true;
        flush_cv_.notify_all();
        
        lock.unlock();
        if (background_thread_.joinable()) {
            background_thread_.join();
        }
        lock.lock();
        
        // 刷写当前内存表
        if (memtable_ && memtable_->size() > 0) {
            auto flush_result = flush_memtable();
            if (!flush_result.is_ok()) {
                // 记录错误但继续关闭
            }
        }
        
        // 关闭WAL
        if (wal_) {
            wal_->close();
        }
        
        is_open_ = false;
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to close LSM tree: " + std::string(e.what()));
    }
}

bool LSMTreeEngine::is_open() const {
    return is_open_;
}

Result<DataEntry> LSMTreeEngine::get(const MultiLevelKey& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<DataEntry>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    total_reads_++;
    
    // 1. 首先检查当前内存表
    auto memtable_result = get_from_memtable(key);
    if (memtable_result.is_ok()) {
        cache_hits_++;
        return memtable_result;
    }
    
    // 2. 检查不可变内存表
    if (immutable_memtable_) {
        auto immutable_result = immutable_memtable_->get(key);
        if (immutable_result.is_ok()) {
            cache_hits_++;
            return immutable_result;
        }
    }
    
    // 3. 检查SSTable（从新到旧）
    auto sstable_result = get_from_sstables(key);
    if (sstable_result.is_ok()) {
        return sstable_result;
    }
    
    return Result<DataEntry>(Status::NOT_FOUND, "Key not found");
}

Result<void> LSMTreeEngine::put(const MultiLevelKey& key, const DataEntry& entry) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    // 写WAL
    WALEntry wal_entry(WALEntryType::PUT, key, entry, 0);
    auto wal_result = wal_->append(wal_entry);
    if (!wal_result.is_ok()) {
        return wal_result;
    }
    
    // 写内存表
    auto put_result = memtable_->put(key, entry);
    if (!put_result.is_ok()) {
        return put_result;
    }
    
    // 检查是否需要刷盘
    if (memtable_->should_flush()) {
        flush_cv_.notify_one();
    }
    
    return Result<void>(Status::OK);
}

Result<void> LSMTreeEngine::put_if_version_match(const MultiLevelKey& key, 
                                                 const DataEntry& entry, 
                                                 Version expected_version) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    // 检查当前版本
    auto current_result = get(key);
    if (current_result.is_ok()) {
        if (current_result.data.version != expected_version) {
            return Result<void>(Status::VERSION_CONFLICT, "Version mismatch");
        }
    } else if (expected_version != 0) {
        return Result<void>(Status::VERSION_CONFLICT, "Key does not exist but expected version is not 0");
    }
    
    // 创建新版本的entry
    DataEntry new_entry = entry;
    new_entry.version = expected_version + 1;
    
    return put(key, new_entry);
}

Result<void> LSMTreeEngine::remove(const MultiLevelKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    // 创建删除标记
    DataEntry delete_entry;
    delete_entry.deleted = true;
    delete_entry.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch());
    
    // 写WAL
    WALEntry wal_entry(WALEntryType::DELETE, key, delete_entry, 0);
    auto wal_result = wal_->append(wal_entry);
    if (!wal_result.is_ok()) {
        return wal_result;
    }
    
    // 在内存表中插入删除标记
    return memtable_->remove(key);
}

Result<void> LSMTreeEngine::remove_if_version_match(const MultiLevelKey& key, 
                                                   Version expected_version) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    // 检查当前版本
    auto current_result = get(key);
    if (!current_result.is_ok()) {
        return Result<void>(Status::NOT_FOUND, "Key not found");
    }
    
    if (current_result.data.version != expected_version) {
        return Result<void>(Status::VERSION_CONFLICT, "Version mismatch");
    }
    
    return remove(key);
}

Result<std::vector<std::pair<MultiLevelKey, DataEntry>>> 
LSMTreeEngine::range_scan(const RangeQuery& query) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(
            Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    std::map<MultiLevelKey, DataEntry> merged_results;
    
    // 1. 从内存表收集数据
    auto memtable_result = memtable_->range_scan(query);
    if (memtable_result.is_ok()) {
        for (const auto& [key, entry] : memtable_result.data) {
            merged_results[key] = entry;
        }
    }
    
    // 2. 从不可变内存表收集数据
    if (immutable_memtable_) {
        auto immutable_result = immutable_memtable_->range_scan(query);
        if (immutable_result.is_ok()) {
            for (const auto& [key, entry] : immutable_result.data) {
                if (merged_results.find(key) == merged_results.end()) {
                    merged_results[key] = entry;
                }
            }
        }
    }
    
    // 3. 从SSTable收集数据（按从新到旧的顺序）
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        auto sstable_result = (*it)->range_scan(query);
        if (sstable_result.is_ok()) {
            for (const auto& [key, entry] : sstable_result.data) {
                if (merged_results.find(key) == merged_results.end()) {
                    merged_results[key] = entry;
                }
            }
        }
    }
    
    // 4. 过滤删除的键并应用限制
    std::vector<std::pair<MultiLevelKey, DataEntry>> final_results;
    uint32_t count = 0;
    
    for (const auto& [key, entry] : merged_results) {
        if (count >= query.limit) {
            break;
        }
        
        if (!entry.deleted) {
            final_results.emplace_back(key, entry);
            count++;
        }
    }
    
    return Result<std::vector<std::pair<MultiLevelKey, DataEntry>>>(Status::OK, final_results);
}

Result<std::vector<DataEntry>> 
LSMTreeEngine::multi_get(const std::vector<MultiLevelKey>& keys) {
    std::vector<DataEntry> results;
    results.reserve(keys.size());
    
    for (const auto& key : keys) {
        auto result = get(key);
        if (result.is_ok()) {
            results.push_back(result.data);
        } else {
            results.emplace_back(); // 空的DataEntry表示未找到
        }
    }
    
    return Result<std::vector<DataEntry>>(Status::OK, results);
}

Result<void> LSMTreeEngine::multi_put(const std::vector<std::pair<MultiLevelKey, DataEntry>>& entries) {
    for (const auto& [key, entry] : entries) {
        auto result = put(key, entry);
        if (!result.is_ok()) {
            return result;
        }
    }
    
    return Result<void>(Status::OK);
}

Result<void> LSMTreeEngine::flush() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    // 强制刷写当前内存表
    auto flush_result = flush_memtable();
    if (!flush_result.is_ok()) {
        return flush_result;
    }
    
    // 同步WAL
    return wal_->sync();
}

Result<void> LSMTreeEngine::compact() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    return compact_sstables();
}

Result<void> LSMTreeEngine::backup(const std::string& backup_path) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    if (!is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "LSM tree is not open");
    }
    
    try {
        // 创建备份目录
        std::filesystem::create_directories(backup_path);
        
        // 备份WAL
        std::filesystem::copy_file(data_dir_ + "/wal.log", 
                                 backup_path + "/wal.log",
                                 std::filesystem::copy_options::overwrite_existing);
        
        // 备份所有SSTable
        for (const auto& sstable : sstables_) {
            std::filesystem::path src_path(sstable->filename());
            std::filesystem::path dst_path = std::filesystem::path(backup_path) / src_path.filename();
            std::filesystem::copy_file(src_path, dst_path,
                                     std::filesystem::copy_options::overwrite_existing);
        }
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to backup: " + std::string(e.what()));
    }
}

Result<void> LSMTreeEngine::restore(const std::string& backup_path) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    if (is_open_) {
        return Result<void>(Status::STORAGE_ERROR, "Cannot restore while LSM tree is open");
    }
    
    try {
        // 清空当前数据目录
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        
        // 恢复所有文件
        for (const auto& entry : std::filesystem::directory_iterator(backup_path)) {
            if (entry.is_regular_file()) {
                std::filesystem::copy_file(entry.path(), 
                                         std::filesystem::path(data_dir_) / entry.path().filename());
            }
        }
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to restore: " + std::string(e.what()));
    }
}

size_t LSMTreeEngine::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t total_size = 0;
    
    if (memtable_) {
        total_size += memtable_->size();
    }
    
    if (immutable_memtable_) {
        total_size += immutable_memtable_->size();
    }
    
    for (const auto& sstable : sstables_) {
        total_size += sstable->size();
    }
    
    return total_size;
}

size_t LSMTreeEngine::memory_usage() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    size_t total_memory = 0;
    
    if (memtable_) {
        total_memory += memtable_->memory_usage();
    }
    
    if (immutable_memtable_) {
        total_memory += immutable_memtable_->memory_usage();
    }
    
    return total_memory;
}

double LSMTreeEngine::cache_hit_ratio() const {
    uint64_t total = total_reads_.load();
    uint64_t hits = cache_hits_.load();
    
    return total > 0 ? static_cast<double>(hits) / total : 0.0;
}

// 私有方法实现

void LSMTreeEngine::background_work() {
    while (!stop_background_) {
        std::unique_lock<std::mutex> lock(flush_mutex_);
        
        // 等待刷盘信号或超时
        flush_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return stop_background_ || (memtable_ && memtable_->should_flush());
        });
        
        if (stop_background_) {
            break;
        }
        
        lock.unlock();
        
        // 执行后台任务
        {
            std::unique_lock<std::shared_mutex> write_lock(mutex_);
            
            if (memtable_ && memtable_->should_flush()) {
                flush_memtable();
            }
            
            if (need_compaction()) {
                compact_sstables();
            }
        }
    }
}

Result<void> LSMTreeEngine::flush_memtable() {
    if (!memtable_ || memtable_->size() == 0) {
        return Result<void>(Status::OK);
    }
    
    try {
        // 1. 将当前内存表设为不可变
        if (immutable_memtable_) {
            // 如果已有不可变内存表，先刷写它
            auto sstable_filename = generate_sstable_filename();
            auto sstable = std::make_unique<SSTable>(sstable_filename);
            
            auto data = immutable_memtable_->get_all_data();
            auto write_result = sstable->write(data);
            if (!write_result.is_ok()) {
                return write_result;
            }
            
            sstables_.push_back(std::move(sstable));
        }
        
        immutable_memtable_ = std::move(memtable_);
        memtable_ = std::make_unique<MemTable>();
        
        // 2. 将不可变内存表写入SSTable
        auto sstable_filename = generate_sstable_filename();
        auto sstable = std::make_unique<SSTable>(sstable_filename);
        
        auto data = immutable_memtable_->get_all_data();
        auto write_result = sstable->write(data);
        if (!write_result.is_ok()) {
            return write_result;
        }
        
        sstables_.push_back(std::move(sstable));
        immutable_memtable_.reset();
        
        // 3. 写入WAL检查点
        WALEntry checkpoint_entry(WALEntryType::CHECKPOINT, MultiLevelKey(), DataEntry(), 0);
        wal_->append(checkpoint_entry);
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to flush memtable: " + std::string(e.what()));
    }
}

Result<void> LSMTreeEngine::compact_sstables() {
    if (sstables_.size() < 2) {
        return Result<void>(Status::OK);
    }
    
    try {
        // 简单的压缩策略：合并最旧的两个SSTable
        auto sstable1 = std::move(sstables_[0]);
        auto sstable2 = std::move(sstables_[1]);
        sstables_.erase(sstables_.begin(), sstables_.begin() + 2);
        
        // 读取两个SSTable的所有数据
        std::map<MultiLevelKey, DataEntry> merged_data;
        
        // 这里简化处理，实际应该用归并排序
        // 读取第一个SSTable的数据...
        // 读取第二个SSTable的数据...
        // 合并数据...
        
        // 创建新的合并后的SSTable
        auto new_sstable_filename = generate_sstable_filename();
        auto new_sstable = std::make_unique<SSTable>(new_sstable_filename);
        
        auto write_result = new_sstable->write(merged_data);
        if (!write_result.is_ok()) {
            return write_result;
        }
        
        sstables_.insert(sstables_.begin(), std::move(new_sstable));
        
        // 删除旧文件
        std::filesystem::remove(sstable1->filename());
        std::filesystem::remove(sstable2->filename());
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to compact SSTables: " + std::string(e.what()));
    }
}

Result<void> LSMTreeEngine::recover_from_wal() {
    auto replay_result = wal_->replay_from(0);
    if (!replay_result.is_ok()) {
        return Result<void>(replay_result.status, replay_result.error_message);
    }
    
    // 重放WAL条目到内存表
    for (const auto& entry : replay_result.data) {
        switch (entry.type) {
            case WALEntryType::PUT:
                memtable_->put(entry.key, entry.data);
                break;
            case WALEntryType::DELETE:
                memtable_->remove(entry.key);
                break;
            case WALEntryType::CHECKPOINT:
                // 检查点，可以安全地截断WAL
                break;
            default:
                break;
        }
    }
    
    return Result<void>(Status::OK);
}

Result<void> LSMTreeEngine::load_sstables() {
    try {
        if (!std::filesystem::exists(data_dir_)) {
            return Result<void>(Status::OK);
        }
        
        std::vector<std::string> sstable_files;
        
        for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".sst") {
                sstable_files.push_back(entry.path().string());
            }
        }
        
        // 按文件名排序（假设文件名包含时间戳）
        std::sort(sstable_files.begin(), sstable_files.end());
        
        for (const auto& filename : sstable_files) {
            auto sstable = std::make_unique<SSTable>(filename);
            auto load_result = sstable->load_index();
            if (load_result.is_ok()) {
                sstables_.push_back(std::move(sstable));
            }
        }
        
        return Result<void>(Status::OK);
        
    } catch (const std::exception& e) {
        return Result<void>(Status::STORAGE_ERROR, "Failed to load SSTables: " + std::string(e.what()));
    }
}

Result<DataEntry> LSMTreeEngine::get_from_memtable(const MultiLevelKey& key) {
    return memtable_->get(key);
}

Result<DataEntry> LSMTreeEngine::get_from_sstables(const MultiLevelKey& key) {
    // 从最新的SSTable开始查找
    for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
        auto result = (*it)->read(key);
        if (result.is_ok()) {
            return result;
        }
    }
    
    return Result<DataEntry>(Status::NOT_FOUND, "Key not found in SSTables");
}

std::string LSMTreeEngine::generate_sstable_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << data_dir_ << "/sstable_"
        << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count()
        << ".sst";
    
    return oss.str();
}

bool LSMTreeEngine::need_compaction() const {
    return sstables_.size() > max_sstables_;
}

} // namespace storage
} // namespace cache