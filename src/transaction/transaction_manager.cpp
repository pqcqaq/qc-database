#include "cache/transaction/transaction_manager.h"
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <sstream>

namespace cache {
namespace transaction {

// DefaultLockManager 实现

DefaultLockManager::DefaultLockManager() : next_lock_id_(1) {
}

DefaultLockManager::~DefaultLockManager() {
}

Result<LockId> DefaultLockManager::acquire_lock(TransactionId tx_id, const MultiLevelKey& key, 
                                               LockType type, std::chrono::milliseconds timeout) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto now = std::chrono::system_clock::now();
    auto deadline = now + timeout;
    
    // 检查锁兼容性
    auto key_locks_it = key_locks_.find(key);
    if (key_locks_it != key_locks_.end()) {
        for (const auto& existing_lock_id : key_locks_it->second) {
            auto existing_lock_it = locks_.find(existing_lock_id);
            if (existing_lock_it != locks_.end()) {
                const auto& existing_lock = existing_lock_it->second;
                
                // 如果是同一个事务的锁，允许
                if (existing_lock.transaction_id == tx_id) {
                    continue;
                }
                
                // 检查锁兼容性
                if (!is_lock_compatible(existing_lock.type, type)) {
                    // 需要等待，更新等待图
                    wait_graph_[tx_id].insert(existing_lock.transaction_id);
                    
                    // 检查死锁
                    auto cycle = find_cycle_in_wait_graph(tx_id);
                    if (!cycle.empty()) {
                        wait_graph_[tx_id].erase(existing_lock.transaction_id);
                        return Result<LockId>::error(Status::DEADLOCK_DETECTED, "Deadlock detected involving transaction " + std::to_string(tx_id));
                    }
                    
                    // 等待锁释放
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    lock.lock();
                    
                    if (std::chrono::system_clock::now() > deadline) {
                        wait_graph_[tx_id].erase(existing_lock.transaction_id);
                        return Result<LockId>::error(Status::TIMEOUT, "Lock acquisition timeout for transaction " + std::to_string(tx_id));
                    }
                    
                    // 重新检查
                    return acquire_lock(tx_id, key, type, 
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::system_clock::now()));
                }
            }
        }
    }
    
    // 可以获取锁
    LockInfo lock_info;
    lock_info.lock_id = generate_lock_id();
    lock_info.type = type;
    lock_info.transaction_id = tx_id;
    lock_info.key = key;
    lock_info.acquired_at = now;
    lock_info.timeout = timeout;
    
    locks_[lock_info.lock_id] = lock_info;
    key_locks_[key].push_back(lock_info.lock_id);
    transaction_locks_[tx_id].insert(lock_info.lock_id);
    
    // 清理等待图
    if (wait_graph_.find(tx_id) != wait_graph_.end()) {
        wait_graph_.erase(tx_id);
    }
    
    return Result<LockId>(Status::OK, lock_info.lock_id);
}

Result<void> DefaultLockManager::release_lock(const LockId& lock_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto lock_it = locks_.find(lock_id);
    if (lock_it == locks_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Lock not found: " + lock_id);
    }
    
    const auto& lock_info = lock_it->second;
    
    // 从key_locks_中移除
    auto key_locks_it = key_locks_.find(lock_info.key);
    if (key_locks_it != key_locks_.end()) {
        auto& locks_vec = key_locks_it->second;
        locks_vec.erase(std::remove(locks_vec.begin(), locks_vec.end(), lock_id), locks_vec.end());
        
        if (locks_vec.empty()) {
            key_locks_.erase(key_locks_it);
        }
    }
    
    // 从transaction_locks_中移除
    auto tx_locks_it = transaction_locks_.find(lock_info.transaction_id);
    if (tx_locks_it != transaction_locks_.end()) {
        tx_locks_it->second.erase(lock_id);
        
        if (tx_locks_it->second.empty()) {
            transaction_locks_.erase(tx_locks_it);
        }
    }
    
    // 移除锁
    locks_.erase(lock_it);
    
    return Result<void>(Status::OK);
}

Result<void> DefaultLockManager::release_all_locks(TransactionId tx_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto tx_locks_it = transaction_locks_.find(tx_id);
    if (tx_locks_it == transaction_locks_.end()) {
        return Result<void>(Status::OK); // 没有锁需要释放
    }
    
    // 复制锁ID列表，因为release_lock会修改transaction_locks_
    auto lock_ids = tx_locks_it->second;
    
    for (const auto& lock_id : lock_ids) {
        auto release_result = release_lock(lock_id);
        if (!release_result.is_ok()) {
            // 记录错误但继续释放其他锁
        }
    }
    
    // 清理等待图
    wait_graph_.erase(tx_id);
    
    return Result<void>(Status::OK);
}

bool DefaultLockManager::is_locked(const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = key_locks_.find(key);
    return it != key_locks_.end() && !it->second.empty();
}

bool DefaultLockManager::can_acquire_lock(const MultiLevelKey& key, LockType type) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto key_locks_it = key_locks_.find(key);
    if (key_locks_it == key_locks_.end()) {
        return true; // 没有现有锁
    }
    
    for (const auto& lock_id : key_locks_it->second) {
        auto lock_it = locks_.find(lock_id);
        if (lock_it != locks_.end()) {
            if (!is_lock_compatible(lock_it->second.type, type)) {
                return false;
            }
        }
    }
    
    return true;
}

std::vector<LockInfo> DefaultLockManager::get_locks_by_transaction(TransactionId tx_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<LockInfo> result;
    
    auto tx_locks_it = transaction_locks_.find(tx_id);
    if (tx_locks_it != transaction_locks_.end()) {
        for (const auto& lock_id : tx_locks_it->second) {
            auto lock_it = locks_.find(lock_id);
            if (lock_it != locks_.end()) {
                result.push_back(lock_it->second);
            }
        }
    }
    
    return result;
}

std::vector<std::vector<TransactionId>> DefaultLockManager::detect_deadlocks() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<std::vector<TransactionId>> deadlocks;
    std::unordered_set<TransactionId> visited;
    
    for (const auto& [tx_id, _] : wait_graph_) {
        if (visited.find(tx_id) == visited.end()) {
            auto cycle = find_cycle_in_wait_graph(tx_id);
            if (!cycle.empty()) {
                deadlocks.push_back(cycle);
                for (auto id : cycle) {
                    visited.insert(id);
                }
            }
        }
    }
    
    return deadlocks;
}

Result<void> DefaultLockManager::resolve_deadlock(const std::vector<TransactionId>& deadlock_cycle) {
    if (deadlock_cycle.empty()) {
        return Result<void>::error(Status::INVALID_ARGUMENT, "Empty deadlock cycle");
    }
    
    // 选择牺牲事务（这里简单选择ID最大的）
    TransactionId victim = *std::max_element(deadlock_cycle.begin(), deadlock_cycle.end());
    
    // 释放牺牲事务的所有锁
    return release_all_locks(victim);
}

// 私有方法实现

std::string DefaultLockManager::generate_lock_id() {
    return "lock_" + std::to_string(next_lock_id_++);
}

bool DefaultLockManager::is_lock_compatible(LockType existing, LockType requested) const {
    // 锁兼容性矩阵
    switch (existing) {
        case LockType::SHARED:
            return requested == LockType::SHARED || requested == LockType::INTENTION_SHARED;
        case LockType::EXCLUSIVE:
            return false; // 排他锁与任何锁都不兼容
        case LockType::INTENTION_SHARED:
            return requested != LockType::EXCLUSIVE;
        case LockType::INTENTION_EXCLUSIVE:
            return requested == LockType::INTENTION_SHARED || requested == LockType::INTENTION_EXCLUSIVE;
        default:
            return false;
    }
}

std::vector<TransactionId> DefaultLockManager::find_cycle_in_wait_graph(TransactionId start) const {
    std::unordered_set<TransactionId> visited;
    std::unordered_set<TransactionId> rec_stack;
    std::vector<TransactionId> path;
    
    std::function<bool(TransactionId)> dfs = [&](TransactionId node) -> bool {
        visited.insert(node);
        rec_stack.insert(node);
        path.push_back(node);
        
        auto it = wait_graph_.find(node);
        if (it != wait_graph_.end()) {
            for (auto neighbor : it->second) {
                if (rec_stack.find(neighbor) != rec_stack.end()) {
                    // 找到环
                    auto cycle_start = std::find(path.begin(), path.end(), neighbor);
                    return true;
                }
                
                if (visited.find(neighbor) == visited.end()) {
                    if (dfs(neighbor)) {
                        return true;
                    }
                }
            }
        }
        
        rec_stack.erase(node);
        path.pop_back();
        return false;
    };
    
    if (dfs(start)) {
        return path;
    }
    
    return {};
}

void DefaultLockManager::cleanup_expired_locks() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto now = std::chrono::system_clock::now();
    std::vector<LockId> expired_locks;
    
    for (const auto& [lock_id, lock_info] : locks_) {
        if (lock_info.is_expired()) {
            expired_locks.push_back(lock_id);
        }
    }
    
    for (const auto& lock_id : expired_locks) {
        release_lock(lock_id);
    }
}

// DefaultTransactionManager 实现

DefaultTransactionManager::DefaultTransactionManager() 
    : next_transaction_id_(1), is_running_(false), stop_threads_(false),
      default_timeout_(std::chrono::milliseconds(30000)) {
    
    lock_manager_ = std::make_unique<DefaultLockManager>();
}

DefaultTransactionManager::~DefaultTransactionManager() {
    if (is_running_) {
        stop();
    }
}

Result<void> DefaultTransactionManager::start() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (is_running_) {
        return Result<void>(Status::OK);
    }
    
    stop_threads_ = false;
    
    // 启动后台线程
    cleanup_thread_ = std::thread(&DefaultTransactionManager::cleanup_loop, this);
    deadlock_detection_thread_ = std::thread(&DefaultTransactionManager::deadlock_detection_loop, this);
    
    is_running_ = true;
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::stop() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (!is_running_) {
        return Result<void>(Status::OK);
    }
    
    stop_threads_ = true;
    
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
    if (deadlock_detection_thread_.joinable()) {
        deadlock_detection_thread_.join();
    }
    
    is_running_ = false;
    return Result<void>(Status::OK);
}

bool DefaultTransactionManager::is_running() const {
    return is_running_;
}

Result<TransactionId> DefaultTransactionManager::begin_transaction(IsolationLevel isolation) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (!is_running_) {
        return Result<TransactionId>::error(Status::SERVICE_UNAVAILABLE, "Transaction manager is not running");
    }
    
    TransactionId tx_id = generate_transaction_id();
    
    DistributedTransaction tx;
    tx.transaction_id = tx_id;
    tx.state = TransactionState::ACTIVE;
    tx.isolation_level = isolation;
    tx.created_at = std::chrono::system_clock::now();
    tx.last_activity = tx.created_at;
    tx.timeout = default_timeout_;
    
    transactions_[tx_id] = tx;
    
    notify_transaction_started(tx_id);
    
    return Result<TransactionId>(Status::OK, tx_id);
}

Result<void> DefaultTransactionManager::commit_transaction(TransactionId tx_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    if (tx.state != TransactionState::ACTIVE) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction " + std::to_string(tx_id) + " is not in active state");
    }
    
    // 两阶段提交
    tx.state = TransactionState::PREPARING;
    tx.update_activity();
    
    // 阶段1：准备阶段
    auto prepare_result = send_prepare_messages(tx_id);
    if (!prepare_result.is_ok()) {
        tx.state = TransactionState::ABORTING;
        abort_transaction(tx_id);
        return prepare_result;
    }
    
    // 等待所有参与者的准备响应
    // 这里简化处理，假设所有参与者都准备好了
    bool all_prepared = true;
    for (const auto& [node_id, vote] : tx.prepare_votes) {
        if (!vote) {
            all_prepared = false;
            break;
        }
    }
    
    if (!all_prepared) {
        tx.state = TransactionState::ABORTING;
        send_abort_messages(tx_id);
        transactions_.erase(tx_it);
        return Result<void>::error(Status::TRANSACTION_ABORTED, "Some participants failed to prepare");
    }
    
    // 阶段2：提交阶段
    tx.state = TransactionState::COMMITTING;
    tx.update_activity();
    
    auto commit_result = send_commit_messages(tx_id);
    if (!commit_result.is_ok()) {
        // 提交阶段失败是严重问题，需要恢复机制
        return commit_result;
    }
    
    // 应用事务操作
    auto apply_result = apply_transaction_operations(tx);
    if (!apply_result.is_ok()) {
        return apply_result;
    }
    
    // 释放所有锁
    if (lock_manager_) {
        lock_manager_->release_all_locks(tx_id);
    }
    
    tx.state = TransactionState::COMMITTED;
    notify_transaction_committed(tx_id);
    
    // 清理事务
    transactions_.erase(tx_it);
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::abort_transaction(TransactionId tx_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    tx.state = TransactionState::ABORTING;
    tx.update_activity();
    
    // 回滚操作
    auto rollback_result = rollback_transaction_operations(tx);
    if (!rollback_result.is_ok()) {
        return rollback_result;
    }
    
    // 发送回滚消息给参与者
    send_abort_messages(tx_id);
    
    // 释放所有锁
    if (lock_manager_) {
        lock_manager_->release_all_locks(tx_id);
    }
    
    tx.state = TransactionState::ABORTED;
    notify_transaction_aborted(tx_id, "Transaction aborted by request");
    
    // 清理事务
    transactions_.erase(tx_it);
    
    return Result<void>(Status::OK);
}

Result<DataEntry> DefaultTransactionManager::read(TransactionId tx_id, const MultiLevelKey& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return Result<DataEntry>::error(validate_result.status, validate_result.error_message);
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    // 根据隔离级别获取读锁
    if (tx.isolation_level >= IsolationLevel::READ_COMMITTED) {
        auto lock_result = lock_manager_->acquire_lock(tx_id, key, LockType::SHARED, 
                                                      std::chrono::milliseconds(5000));
        if (!lock_result.is_ok()) {
            return Result<DataEntry>::error(lock_result.status, lock_result.error_message);
        }
        
        tx.held_locks.insert(lock_result.data);
    }
    
    // 记录读操作
    TransactionOperation op;
    op.type = OperationType::READ;
    op.key = key;
    op.timestamp = std::chrono::system_clock::now();
    
    tx.operations.push_back(op);
    tx.update_activity();
    
    // 这里应该从存储引擎读取数据
    // 简化处理，返回一个模拟的数据
    DataEntry entry;
    entry.value = "simulated_value_for_" + key.to_string();
    entry.version = 1;
    entry.timestamp = std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch());
    
    return Result<DataEntry>(Status::OK, entry);
}

Result<void> DefaultTransactionManager::write(TransactionId tx_id, const MultiLevelKey& key, const DataEntry& value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    // 获取排他锁
    auto lock_result = lock_manager_->acquire_lock(tx_id, key, LockType::EXCLUSIVE, 
                                                  std::chrono::milliseconds(5000));
    if (!lock_result.is_ok()) {
        return Result<void>::error(lock_result.status, lock_result.error_message);
    }
    
    tx.held_locks.insert(lock_result.data);
    
    // 记录写操作
    TransactionOperation op;
    op.type = OperationType::WRITE;
    op.key = key;
    op.new_value = value;
    op.timestamp = std::chrono::system_clock::now();
    
    tx.operations.push_back(op);
    tx.update_activity();
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::remove(TransactionId tx_id, const MultiLevelKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    // 获取排他锁
    auto lock_result = lock_manager_->acquire_lock(tx_id, key, LockType::EXCLUSIVE, 
                                                  std::chrono::milliseconds(5000));
    if (!lock_result.is_ok()) {
        return Result<void>::error(lock_result.status, lock_result.error_message);
    }
    
    tx.held_locks.insert(lock_result.data);
    
    // 记录删除操作
    TransactionOperation op;
    op.type = OperationType::DELETE;
    op.key = key;
    op.timestamp = std::chrono::system_clock::now();
    
    tx.operations.push_back(op);
    tx.update_activity();
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::lock(TransactionId tx_id, const MultiLevelKey& key, LockType type) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    // 获取锁
    auto lock_result = lock_manager_->acquire_lock(tx_id, key, type, 
                                                  std::chrono::milliseconds(5000));
    if (!lock_result.is_ok()) {
        return Result<void>::error(lock_result.status, lock_result.error_message);
    }
    
    tx.held_locks.insert(lock_result.data);
    
    // 记录锁操作
    TransactionOperation op;
    op.type = OperationType::LOCK;
    op.key = key;
    op.timestamp = std::chrono::system_clock::now();
    
    tx.operations.push_back(op);
    tx.update_activity();
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::prepare(TransactionId tx_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto validate_result = validate_transaction(tx_id);
    if (!validate_result.is_ok()) {
        return validate_result;
    }
    
    auto tx_it = transactions_.find(tx_id);
    DistributedTransaction& tx = tx_it->second;
    
    if (tx.state != TransactionState::ACTIVE) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction " + std::to_string(tx_id) + " is not in active state");
    }
    
    tx.state = TransactionState::PREPARED;
    tx.update_activity();
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::handle_tpc_message(const TpcMessage& message) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    switch (message.type) {
        case TwoPhaseCommitMessage::PREPARE:
            handle_prepare_message(message);
            break;
        case TwoPhaseCommitMessage::PREPARE_OK:
        case TwoPhaseCommitMessage::PREPARE_ABORT:
            handle_prepare_response(message);
            break;
        case TwoPhaseCommitMessage::COMMIT:
            handle_commit_message(message);
            break;
        case TwoPhaseCommitMessage::ABORT:
            handle_abort_message(message);
            break;
        default:
            return Result<void>::error(Status::INVALID_ARGUMENT, "Unknown TPC message type");
    }
    
    return Result<void>(Status::OK);
}

TransactionState DefaultTransactionManager::get_transaction_state(TransactionId tx_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = transactions_.find(tx_id);
    if (it == transactions_.end()) {
        return TransactionState::ABORTED; // 不存在的事务视为已中止
    }
    
    return it->second.state;
}

std::vector<TransactionId> DefaultTransactionManager::get_active_transactions() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<TransactionId> active_txs;
    for (const auto& [tx_id, tx] : transactions_) {
        if (tx.state == TransactionState::ACTIVE) {
            active_txs.push_back(tx_id);
        }
    }
    
    return active_txs;
}

DistributedTransaction DefaultTransactionManager::get_transaction_info(TransactionId tx_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = transactions_.find(tx_id);
    if (it != transactions_.end()) {
        return it->second;
    }
    
    return DistributedTransaction{}; // 返回空的事务信息
}

void DefaultTransactionManager::set_event_handler(std::shared_ptr<TransactionEventHandler> handler) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    event_handler_ = handler;
}

void DefaultTransactionManager::set_lock_manager(std::shared_ptr<LockManager> lock_manager) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    lock_manager_ = lock_manager;
}

void DefaultTransactionManager::set_default_timeout(std::chrono::milliseconds timeout) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    default_timeout_ = timeout;
}

Result<void> DefaultTransactionManager::recover_transactions() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    // 从持久化存储恢复事务信息
    // 这里简化处理，实际实现需要从WAL或其他持久化机制恢复
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::cleanup_expired_transactions() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto now = std::chrono::system_clock::now();
    std::vector<TransactionId> expired_txs;
    
    for (const auto& [tx_id, tx] : transactions_) {
        if (tx.is_expired()) {
            expired_txs.push_back(tx_id);
        }
    }
    
    for (auto tx_id : expired_txs) {
        abort_transaction(tx_id);
    }
    
    return Result<void>(Status::OK);
}

// 私有方法实现

void DefaultTransactionManager::cleanup_loop() {
    while (!stop_threads_) {
        try {
            cleanup_expired_transactions();
            
            if (lock_manager_) {
                // 清理过期的锁
                // lock_manager_->cleanup_expired_locks();
            }
            
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

void DefaultTransactionManager::deadlock_detection_loop() {
    while (!stop_threads_) {
        try {
            if (lock_manager_) {
                auto deadlocks = lock_manager_->detect_deadlocks();
                
                for (const auto& deadlock : deadlocks) {
                    notify_deadlock_detected(deadlock);
                    
                    // 自动解决死锁
                    auto resolve_result = lock_manager_->resolve_deadlock(deadlock);
                    if (!resolve_result.is_ok()) {
                        // 记录解决死锁的错误
                    }
                }
            }
            
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

TransactionId DefaultTransactionManager::generate_transaction_id() {
    return next_transaction_id_++;
}

Result<void> DefaultTransactionManager::validate_transaction(TransactionId tx_id) const {
    auto it = transactions_.find(tx_id);
    if (it == transactions_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Transaction " + std::to_string(tx_id) + " not found");
    }
    
    const auto& tx = it->second;
    if (tx.is_expired()) {
        return Result<void>::error(Status::TIMEOUT, "Transaction " + std::to_string(tx_id) + " has expired");
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::apply_transaction_operations(const DistributedTransaction& tx) {
    // 应用事务的所有操作
    for (const auto& op : tx.operations) {
        switch (op.type) {
            case OperationType::WRITE:
                // 写入存储引擎
                break;
            case OperationType::DELETE:
                // 从存储引擎删除
                break;
            default:
                break;
        }
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::rollback_transaction_operations(const DistributedTransaction& tx) {
    // 回滚事务的所有操作（逆序执行）
    for (auto it = tx.operations.rbegin(); it != tx.operations.rend(); ++it) {
        const auto& op = *it;
        
        switch (op.type) {
            case OperationType::WRITE:
                // 恢复旧值
                break;
            case OperationType::DELETE:
                // 恢复删除的数据
                break;
            default:
                break;
        }
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::send_prepare_messages(TransactionId tx_id) {
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Transaction not found");
    }
    
    const auto& tx = tx_it->second;
    
    for (const auto& node : tx.participant_nodes) {
        TpcMessage message;
        message.type = TwoPhaseCommitMessage::PREPARE;
        message.transaction_id = tx_id;
        message.to_node = node;
        message.timestamp = std::chrono::system_clock::now();
        
        // 发送消息给参与者节点
        // 这里简化处理，实际需要通过网络发送
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::send_commit_messages(TransactionId tx_id) {
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Transaction not found");
    }
    
    const auto& tx = tx_it->second;
    
    for (const auto& node : tx.participant_nodes) {
        TpcMessage message;
        message.type = TwoPhaseCommitMessage::COMMIT;
        message.transaction_id = tx_id;
        message.to_node = node;
        message.timestamp = std::chrono::system_clock::now();
        
        // 发送消息给参与者节点
        // 这里简化处理，实际需要通过网络发送
    }
    
    return Result<void>(Status::OK);
}

Result<void> DefaultTransactionManager::send_abort_messages(TransactionId tx_id) {
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Transaction not found");
    }
    
    const auto& tx = tx_it->second;
    
    for (const auto& node : tx.participant_nodes) {
        TpcMessage message;
        message.type = TwoPhaseCommitMessage::ABORT;
        message.transaction_id = tx_id;
        message.to_node = node;
        message.timestamp = std::chrono::system_clock::now();
        
        // 发送消息给参与者节点
        // 这里简化处理，实际需要通过网络发送
    }
    
    return Result<void>(Status::OK);
}

void DefaultTransactionManager::handle_prepare_message(const TpcMessage& message) {
    // 作为参与者处理准备消息
    auto tx_id = message.transaction_id;
    
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        // 发送PREPARE_ABORT响应
        return;
    }
    
    auto& tx = tx_it->second;
    
    // 检查事务是否可以提交
    bool can_commit = true;
    
    if (can_commit) {
        tx.state = TransactionState::PREPARED;
        // 发送PREPARE_OK响应
    } else {
        tx.state = TransactionState::ABORTING;
        // 发送PREPARE_ABORT响应
    }
}

void DefaultTransactionManager::handle_prepare_response(const TpcMessage& message) {
    // 作为协调者处理准备响应
    auto tx_id = message.transaction_id;
    
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return;
    }
    
    auto& tx = tx_it->second;
    
    bool vote = (message.type == TwoPhaseCommitMessage::PREPARE_OK);
    tx.prepare_votes[message.from_node] = vote;
}

void DefaultTransactionManager::handle_commit_message(const TpcMessage& message) {
    // 作为参与者处理提交消息
    auto tx_id = message.transaction_id;
    
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return;
    }
    
    auto& tx = tx_it->second;
    
    // 应用事务操作
    apply_transaction_operations(tx);
    
    tx.state = TransactionState::COMMITTED;
    
    // 发送COMMIT_OK响应
}

void DefaultTransactionManager::handle_abort_message(const TpcMessage& message) {
    // 作为参与者处理回滚消息
    auto tx_id = message.transaction_id;
    
    auto tx_it = transactions_.find(tx_id);
    if (tx_it == transactions_.end()) {
        return;
    }
    
    auto& tx = tx_it->second;
    
    // 回滚事务操作
    rollback_transaction_operations(tx);
    
    tx.state = TransactionState::ABORTED;
    
    // 发送ABORT_OK响应
}

void DefaultTransactionManager::notify_transaction_started(TransactionId tx_id) {
    if (event_handler_) {
        event_handler_->on_transaction_started(tx_id);
    }
}

void DefaultTransactionManager::notify_transaction_committed(TransactionId tx_id) {
    if (event_handler_) {
        event_handler_->on_transaction_committed(tx_id);
    }
}

void DefaultTransactionManager::notify_transaction_aborted(TransactionId tx_id, const std::string& reason) {
    if (event_handler_) {
        event_handler_->on_transaction_aborted(tx_id, reason);
    }
}

void DefaultTransactionManager::notify_deadlock_detected(const std::vector<TransactionId>& deadlock_cycle) {
    if (event_handler_) {
        event_handler_->on_deadlock_detected(deadlock_cycle);
    }
}

// TransactionContext 实现

TransactionContext::TransactionContext(std::shared_ptr<TransactionManager> manager, TransactionId tx_id)
    : manager_(manager), transaction_id_(tx_id), is_committed_(false), is_aborted_(false) {
}

TransactionContext::~TransactionContext() {
    if (!is_committed_ && !is_aborted_) {
        // 自动回滚未提交的事务
        abort();
    }
}

Result<DataEntry> TransactionContext::read(const MultiLevelKey& key) {
    if (is_committed_ || is_aborted_) {
        return Result<DataEntry>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    return manager_->read(transaction_id_, key);
}

Result<void> TransactionContext::write(const MultiLevelKey& key, const DataEntry& value) {
    if (is_committed_ || is_aborted_) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    return manager_->write(transaction_id_, key, value);
}

Result<void> TransactionContext::remove(const MultiLevelKey& key) {
    if (is_committed_ || is_aborted_) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    return manager_->remove(transaction_id_, key);
}

Result<void> TransactionContext::lock(const MultiLevelKey& key, LockType type) {
    if (is_committed_ || is_aborted_) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    return manager_->lock(transaction_id_, key, type);
}

Result<void> TransactionContext::commit() {
    if (is_committed_ || is_aborted_) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    auto result = manager_->commit_transaction(transaction_id_);
    if (result.is_ok()) {
        is_committed_ = true;
    }
    
    return result;
}

Result<void> TransactionContext::abort() {
    if (is_committed_ || is_aborted_) {
        return Result<void>::error(Status::INVALID_STATE, "Transaction is already finished");
    }
    
    auto result = manager_->abort_transaction(transaction_id_);
    if (result.is_ok()) {
        is_aborted_ = true;
    }
    
    return result;
}

TransactionState TransactionContext::get_state() const {
    return manager_->get_transaction_state(transaction_id_);
}

} // namespace transaction
} // namespace cache