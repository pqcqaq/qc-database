#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>

namespace cache {
namespace transaction {

// 事务ID类型
using TransactionId = uint64_t;
using LockId = std::string;

// 事务状态
enum class TransactionState {
    ACTIVE,        // 活跃状态
    PREPARING,     // 准备阶段
    PREPARED,      // 已准备
    COMMITTING,    // 提交中
    COMMITTED,     // 已提交
    ABORTING,      // 回滚中
    ABORTED,       // 已回滚
    TIMEOUT        // 超时
};

// 操作类型
enum class OperationType {
    READ,
    WRITE,
    DELETE,
    LOCK
};

// 隔离级别
enum class IsolationLevel {
    READ_UNCOMMITTED,  // 读未提交
    READ_COMMITTED,    // 读已提交  
    REPEATABLE_READ,   // 可重复读
    SERIALIZABLE       // 可串行化
};

// 锁类型
enum class LockType {
    SHARED,      // 共享锁(读锁)
    EXCLUSIVE,   // 排他锁(写锁)
    INTENTION_SHARED,   // 意向共享锁
    INTENTION_EXCLUSIVE // 意向排他锁
};

// 事务操作记录
struct TransactionOperation {
    OperationType type;
    MultiLevelKey key;
    DataEntry old_value;  // 用于回滚
    DataEntry new_value;
    Version version;
    std::chrono::system_clock::time_point timestamp;
    
    TransactionOperation() : type(OperationType::READ), version(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// 锁信息
struct LockInfo {
    LockId lock_id;
    LockType type;
    TransactionId transaction_id;
    MultiLevelKey key;
    std::chrono::system_clock::time_point acquired_at;
    std::chrono::milliseconds timeout;
    
    LockInfo() : transaction_id(0) {
        acquired_at = std::chrono::system_clock::now();
    }
    
    bool is_expired() const {
        auto now = std::chrono::system_clock::now();
        return (now - acquired_at) > timeout;
    }
};

// 分布式事务信息
struct DistributedTransaction {
    TransactionId transaction_id;
    NodeId coordinator_node;
    std::unordered_set<NodeId> participant_nodes;
    TransactionState state;
    IsolationLevel isolation_level;
    
    std::vector<TransactionOperation> operations;
    std::unordered_set<LockId> held_locks;
    
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_activity;
    std::chrono::milliseconds timeout;
    
    // 两阶段提交状态
    std::unordered_map<NodeId, bool> prepare_votes;  // 各节点的准备投票
    std::unordered_map<NodeId, bool> commit_acks;    // 各节点的提交确认
    
    DistributedTransaction() : transaction_id(0), state(TransactionState::ACTIVE),
                              isolation_level(IsolationLevel::READ_COMMITTED) {
        auto now = std::chrono::system_clock::now();
        created_at = now;
        last_activity = now;
        timeout = std::chrono::milliseconds(30000); // 30秒超时
    }
    
    bool is_expired() const {
        auto now = std::chrono::system_clock::now();
        return (now - last_activity) > timeout;
    }
    
    void update_activity() {
        last_activity = std::chrono::system_clock::now();
    }
};

// 两阶段提交消息类型
enum class TwoPhaseCommitMessage {
    PREPARE,      // 准备消息
    PREPARE_OK,   // 准备成功
    PREPARE_ABORT,// 准备失败
    COMMIT,       // 提交消息
    COMMIT_OK,    // 提交成功
    ABORT,        // 回滚消息
    ABORT_OK      // 回滚成功
};

// 两阶段提交消息
struct TpcMessage {
    TwoPhaseCommitMessage type;
    TransactionId transaction_id;
    NodeId from_node;
    NodeId to_node;
    std::string payload;
    std::chrono::system_clock::time_point timestamp;
    
    TpcMessage() : transaction_id(0) {
        timestamp = std::chrono::system_clock::now();
    }
};

// 事务管理器回调接口
class TransactionEventHandler {
public:
    virtual ~TransactionEventHandler() = default;
    
    virtual void on_transaction_started(TransactionId tx_id) = 0;
    virtual void on_transaction_committed(TransactionId tx_id) = 0;
    virtual void on_transaction_aborted(TransactionId tx_id, const std::string& reason) = 0;
    virtual void on_deadlock_detected(const std::vector<TransactionId>& deadlock_cycle) = 0;
    virtual void on_lock_timeout(TransactionId tx_id, const LockId& lock_id) = 0;
};

// 分布式锁管理器
class LockManager {
public:
    virtual ~LockManager() = default;
    
    // 锁操作
    virtual Result<LockId> acquire_lock(TransactionId tx_id, const MultiLevelKey& key, 
                                       LockType type, std::chrono::milliseconds timeout) = 0;
    virtual Result<void> release_lock(const LockId& lock_id) = 0;
    virtual Result<void> release_all_locks(TransactionId tx_id) = 0;
    
    // 锁查询
    virtual bool is_locked(const MultiLevelKey& key) const = 0;
    virtual bool can_acquire_lock(const MultiLevelKey& key, LockType type) const = 0;
    virtual std::vector<LockInfo> get_locks_by_transaction(TransactionId tx_id) const = 0;
    
    // 死锁检测
    virtual std::vector<std::vector<TransactionId>> detect_deadlocks() = 0;
    virtual Result<void> resolve_deadlock(const std::vector<TransactionId>& deadlock_cycle) = 0;
};

// 分布式事务管理器接口
class TransactionManager {
public:
    virtual ~TransactionManager() = default;
    
    // 生命周期
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // 事务操作
    virtual Result<TransactionId> begin_transaction(IsolationLevel isolation = IsolationLevel::READ_COMMITTED) = 0;
    virtual Result<void> commit_transaction(TransactionId tx_id) = 0;
    virtual Result<void> abort_transaction(TransactionId tx_id) = 0;
    
    // 数据操作
    virtual Result<DataEntry> read(TransactionId tx_id, const MultiLevelKey& key) = 0;
    virtual Result<void> write(TransactionId tx_id, const MultiLevelKey& key, const DataEntry& value) = 0;
    virtual Result<void> remove(TransactionId tx_id, const MultiLevelKey& key) = 0;
    
    // 锁操作
    virtual Result<void> lock(TransactionId tx_id, const MultiLevelKey& key, LockType type) = 0;
    
    // 两阶段提交
    virtual Result<void> prepare(TransactionId tx_id) = 0;
    virtual Result<void> handle_tpc_message(const TpcMessage& message) = 0;
    
    // 状态查询
    virtual TransactionState get_transaction_state(TransactionId tx_id) const = 0;
    virtual std::vector<TransactionId> get_active_transactions() const = 0;
    virtual DistributedTransaction get_transaction_info(TransactionId tx_id) const = 0;
    
    // 配置
    virtual void set_event_handler(std::shared_ptr<TransactionEventHandler> handler) = 0;
    virtual void set_lock_manager(std::shared_ptr<LockManager> lock_manager) = 0;
    virtual void set_default_timeout(std::chrono::milliseconds timeout) = 0;
    
    // 恢复
    virtual Result<void> recover_transactions() = 0;
    virtual Result<void> cleanup_expired_transactions() = 0;
};

// 默认锁管理器实现
class DefaultLockManager : public LockManager {
public:
    DefaultLockManager();
    ~DefaultLockManager() override;
    
    // LockManager 接口实现
    Result<LockId> acquire_lock(TransactionId tx_id, const MultiLevelKey& key, 
                               LockType type, std::chrono::milliseconds timeout) override;
    Result<void> release_lock(const LockId& lock_id) override;
    Result<void> release_all_locks(TransactionId tx_id) override;
    
    bool is_locked(const MultiLevelKey& key) const override;
    bool can_acquire_lock(const MultiLevelKey& key, LockType type) const override;
    std::vector<LockInfo> get_locks_by_transaction(TransactionId tx_id) const override;
    
    std::vector<std::vector<TransactionId>> detect_deadlocks() override;
    Result<void> resolve_deadlock(const std::vector<TransactionId>& deadlock_cycle) override;
    
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<LockId, LockInfo> locks_;
    std::unordered_map<MultiLevelKey, std::vector<LockId>, MultiLevelKeyHash> key_locks_;
    std::unordered_map<TransactionId, std::unordered_set<LockId>> transaction_locks_;
    
    // 等待图用于死锁检测
    std::unordered_map<TransactionId, std::unordered_set<TransactionId>> wait_graph_;
    
    std::atomic<uint64_t> next_lock_id_;
    
    // 私有方法
    std::string generate_lock_id();
    bool is_lock_compatible(LockType existing, LockType requested) const;
    void update_wait_graph();
    std::vector<TransactionId> find_cycle_in_wait_graph(TransactionId start) const;
    void cleanup_expired_locks();
};

// 默认事务管理器实现
class DefaultTransactionManager : public TransactionManager {
public:
    DefaultTransactionManager();
    ~DefaultTransactionManager() override;
    
    // TransactionManager 接口实现
    Result<void> start() override;
    Result<void> stop() override;
    bool is_running() const override;
    
    Result<TransactionId> begin_transaction(IsolationLevel isolation = IsolationLevel::READ_COMMITTED) override;
    Result<void> commit_transaction(TransactionId tx_id) override;
    Result<void> abort_transaction(TransactionId tx_id) override;
    
    Result<DataEntry> read(TransactionId tx_id, const MultiLevelKey& key) override;
    Result<void> write(TransactionId tx_id, const MultiLevelKey& key, const DataEntry& value) override;
    Result<void> remove(TransactionId tx_id, const MultiLevelKey& key) override;
    
    Result<void> lock(TransactionId tx_id, const MultiLevelKey& key, LockType type) override;
    
    Result<void> prepare(TransactionId tx_id) override;
    Result<void> handle_tpc_message(const TpcMessage& message) override;
    
    TransactionState get_transaction_state(TransactionId tx_id) const override;
    std::vector<TransactionId> get_active_transactions() const override;
    DistributedTransaction get_transaction_info(TransactionId tx_id) const override;
    
    void set_event_handler(std::shared_ptr<TransactionEventHandler> handler) override;
    void set_lock_manager(std::shared_ptr<LockManager> lock_manager) override;
    void set_default_timeout(std::chrono::milliseconds timeout) override;
    
    Result<void> recover_transactions() override;
    Result<void> cleanup_expired_transactions() override;
    
private:
    std::shared_ptr<LockManager> lock_manager_;
    std::shared_ptr<TransactionEventHandler> event_handler_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<TransactionId, DistributedTransaction> transactions_;
    std::atomic<TransactionId> next_transaction_id_;
    std::atomic<bool> is_running_;
    
    std::chrono::milliseconds default_timeout_;
    
    // 后台线程
    std::thread cleanup_thread_;
    std::thread deadlock_detection_thread_;
    std::atomic<bool> stop_threads_;
    
    // 私有方法
    void cleanup_loop();
    void deadlock_detection_loop();
    
    TransactionId generate_transaction_id();
    Result<void> validate_transaction(TransactionId tx_id) const;
    Result<void> apply_transaction_operations(const DistributedTransaction& tx);
    Result<void> rollback_transaction_operations(const DistributedTransaction& tx);
    
    // 两阶段提交实现
    Result<void> send_prepare_messages(TransactionId tx_id);
    Result<void> send_commit_messages(TransactionId tx_id);
    Result<void> send_abort_messages(TransactionId tx_id);
    
    void handle_prepare_message(const TpcMessage& message);
    void handle_prepare_response(const TpcMessage& message);
    void handle_commit_message(const TpcMessage& message);
    void handle_abort_message(const TpcMessage& message);
    
    // 事件通知
    void notify_transaction_started(TransactionId tx_id);
    void notify_transaction_committed(TransactionId tx_id);
    void notify_transaction_aborted(TransactionId tx_id, const std::string& reason);
    void notify_deadlock_detected(const std::vector<TransactionId>& deadlock_cycle);
};

// 事务上下文 - 用于客户端API
class TransactionContext {
public:
    TransactionContext(std::shared_ptr<TransactionManager> manager, TransactionId tx_id);
    ~TransactionContext();
    
    // 数据操作
    Result<DataEntry> read(const MultiLevelKey& key);
    Result<void> write(const MultiLevelKey& key, const DataEntry& value);
    Result<void> remove(const MultiLevelKey& key);
    
    // 锁操作
    Result<void> lock(const MultiLevelKey& key, LockType type);
    
    // 事务控制
    Result<void> commit();
    Result<void> abort();
    
    // 状态查询
    TransactionId get_id() const { return transaction_id_; }
    TransactionState get_state() const;
    
private:
    std::shared_ptr<TransactionManager> manager_;
    TransactionId transaction_id_;
    bool is_committed_;
    bool is_aborted_;
};

} // namespace transaction
} // namespace cache