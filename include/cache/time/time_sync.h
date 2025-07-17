#pragma once

#include "cache/common/types.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>

namespace cache {
namespace time {

// 时间戳类型
using LogicalTimestamp = uint64_t;
using PhysicalTimestamp = std::chrono::system_clock::time_point;
using ClockOffset = std::chrono::milliseconds;

// 混合逻辑时钟(HLC)
struct HybridLogicalClock {
    PhysicalTimestamp physical_time;
    LogicalTimestamp logical_time;
    
    HybridLogicalClock() : logical_time(0) {
        physical_time = std::chrono::system_clock::now();
    }
    
    HybridLogicalClock(PhysicalTimestamp pt, LogicalTimestamp lt) 
        : physical_time(pt), logical_time(lt) {}
    
    // 比较操作
    bool operator<(const HybridLogicalClock& other) const {
        if (physical_time != other.physical_time) {
            return physical_time < other.physical_time;
        }
        return logical_time < other.logical_time;
    }
    
    bool operator==(const HybridLogicalClock& other) const {
        return physical_time == other.physical_time && logical_time == other.logical_time;
    }
    
    bool operator<=(const HybridLogicalClock& other) const {
        return *this < other || *this == other;
    }
    
    bool operator>=(const HybridLogicalClock& other) const {
        return !(*this < other);
    }
    
    // 序列化
    std::string to_string() const;
    static HybridLogicalClock from_string(const std::string& str);
};

// 时间同步消息类型
enum class TimeSyncMessageType {
    TIME_REQUEST,       // 时间请求
    TIME_RESPONSE,      // 时间响应
    OFFSET_UPDATE,      // 时钟偏移更新
    SYNC_HEARTBEAT      // 同步心跳
};

// 时间同步消息
struct TimeSyncMessage {
    TimeSyncMessageType type;
    NodeId from_node;
    NodeId to_node;
    PhysicalTimestamp send_time;      // 发送时间
    PhysicalTimestamp receive_time;   // 接收时间
    PhysicalTimestamp response_time;  // 响应时间
    ClockOffset estimated_offset;     // 估计的时钟偏移
    std::chrono::milliseconds round_trip_time; // 往返时间
    
    TimeSyncMessage() {
        send_time = std::chrono::system_clock::now();
        round_trip_time = std::chrono::milliseconds(0);
    }
};

// 时钟偏移信息
struct ClockOffsetInfo {
    NodeId node_id;
    ClockOffset offset;               // 相对于本地时钟的偏移
    std::chrono::milliseconds uncertainty; // 偏移的不确定性
    PhysicalTimestamp last_sync;      // 最后同步时间
    uint32_t sync_count;              // 同步次数
    double confidence;                // 置信度[0,1]
    
    ClockOffsetInfo() : offset(0), uncertainty(0), sync_count(0), confidence(0.0) {
        last_sync = std::chrono::system_clock::now();
    }
    
    bool is_expired(std::chrono::milliseconds max_age) const {
        auto now = std::chrono::system_clock::now();
        return (now - last_sync) > max_age;
    }
};

// 时钟同步配置
struct TimeSyncConfig {
    // 同步参数
    std::chrono::milliseconds sync_interval;        // 同步间隔
    std::chrono::milliseconds max_clock_drift;      // 最大时钟漂移
    std::chrono::milliseconds sync_timeout;         // 同步超时
    uint32_t min_samples;                           // 最小采样数
    
    // NTP相关参数
    bool enable_ntp_sync;                           // 启用NTP同步
    std::vector<std::string> ntp_servers;           // NTP服务器列表
    std::chrono::milliseconds ntp_sync_interval;    // NTP同步间隔
    
    // 算法参数
    double offset_filter_alpha;                     // 偏移滤波器α值
    std::chrono::milliseconds max_offset_age;       // 最大偏移年龄
    bool enable_hybrid_logical_clock;              // 启用混合逻辑时钟
    
    TimeSyncConfig() 
        : sync_interval(std::chrono::milliseconds(5000)),
          max_clock_drift(std::chrono::milliseconds(100)),
          sync_timeout(std::chrono::milliseconds(1000)),
          min_samples(3),
          enable_ntp_sync(false),
          ntp_sync_interval(std::chrono::milliseconds(300000)),
          offset_filter_alpha(0.125),
          max_offset_age(std::chrono::milliseconds(60000)),
          enable_hybrid_logical_clock(true) {
        ntp_servers = {"pool.ntp.org", "time.google.com"};
    }
};

// 时间同步事件处理器
class TimeSyncEventHandler {
public:
    virtual ~TimeSyncEventHandler() = default;
    
    virtual void on_clock_drift_detected(const NodeId& node_id, ClockOffset drift) = 0;
    virtual void on_time_sync_completed(const NodeId& node_id, ClockOffset offset) = 0;
    virtual void on_time_sync_failed(const NodeId& node_id, const std::string& reason) = 0;
    virtual void on_ntp_sync_completed(ClockOffset offset) = 0;
};

// 时间服务接口
class TimeService {
public:
    virtual ~TimeService() = default;
    
    // 生命周期
    virtual Result<void> start(const TimeSyncConfig& config) = 0;
    virtual Result<void> stop() = 0;
    virtual bool is_running() const = 0;
    
    // 时间获取
    virtual PhysicalTimestamp now() const = 0;                          // 当前物理时间
    virtual PhysicalTimestamp now_synchronized() const = 0;             // 同步后的时间
    virtual HybridLogicalClock now_logical() = 0;                      // 混合逻辑时钟
    virtual HybridLogicalClock update_logical_clock(const HybridLogicalClock& received) = 0;
    
    // 时钟偏移
    virtual ClockOffset get_offset(const NodeId& node_id) const = 0;
    virtual Result<void> set_offset(const NodeId& node_id, ClockOffset offset) = 0;
    virtual std::vector<ClockOffsetInfo> get_all_offsets() const = 0;
    
    // 时间同步
    virtual Result<void> sync_with_node(const NodeId& node_id) = 0;
    virtual Result<void> sync_with_ntp() = 0;
    virtual Result<void> handle_sync_message(const TimeSyncMessage& message) = 0;
    
    // 节点管理
    virtual void add_node(const NodeId& node_id) = 0;
    virtual void remove_node(const NodeId& node_id) = 0;
    virtual std::vector<NodeId> get_sync_nodes() const = 0;
    
    // 配置
    virtual void set_event_handler(std::shared_ptr<TimeSyncEventHandler> handler) = 0;
    virtual void update_config(const TimeSyncConfig& config) = 0;
    virtual TimeSyncConfig get_config() const = 0;
    
    // 统计信息
    virtual std::chrono::milliseconds get_estimated_uncertainty() const = 0;
    virtual double get_sync_quality() const = 0; // 同步质量评分[0,1]
};

// 默认时间服务实现
class DefaultTimeService : public TimeService {
public:
    DefaultTimeService();
    ~DefaultTimeService() override;
    
    // TimeService 接口实现
    Result<void> start(const TimeSyncConfig& config) override;
    Result<void> stop() override;
    bool is_running() const override;
    
    PhysicalTimestamp now() const override;
    PhysicalTimestamp now_synchronized() const override;
    HybridLogicalClock now_logical() override;
    HybridLogicalClock update_logical_clock(const HybridLogicalClock& received) override;
    
    ClockOffset get_offset(const NodeId& node_id) const override;
    Result<void> set_offset(const NodeId& node_id, ClockOffset offset) override;
    std::vector<ClockOffsetInfo> get_all_offsets() const override;
    
    Result<void> sync_with_node(const NodeId& node_id) override;
    Result<void> sync_with_ntp() override;
    Result<void> handle_sync_message(const TimeSyncMessage& message) override;
    
    void add_node(const NodeId& node_id) override;
    void remove_node(const NodeId& node_id) override;
    std::vector<NodeId> get_sync_nodes() const override;
    
    void set_event_handler(std::shared_ptr<TimeSyncEventHandler> handler) override;
    void update_config(const TimeSyncConfig& config) override;
    TimeSyncConfig get_config() const override;
    
    std::chrono::milliseconds get_estimated_uncertainty() const override;
    double get_sync_quality() const override;
    
private:
    TimeSyncConfig config_;
    std::shared_ptr<TimeSyncEventHandler> event_handler_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<NodeId, ClockOffsetInfo> node_offsets_;
    std::unordered_set<NodeId> sync_nodes_;
    
    // 时钟状态
    ClockOffset local_offset_;                  // 本地时钟偏移(相对于真实时间)
    std::atomic<LogicalTimestamp> logical_time_; // 逻辑时间计数器
    PhysicalTimestamp last_physical_time_;      // 上次物理时间
    
    std::atomic<bool> is_running_;
    
    // 后台线程
    std::thread sync_thread_;
    std::thread ntp_thread_;
    std::atomic<bool> stop_threads_;
    
    // 同步统计
    std::atomic<uint64_t> sync_attempts_;
    std::atomic<uint64_t> sync_successes_;
    std::atomic<uint64_t> sync_failures_;
    
    // 私有方法
    void sync_loop();
    void ntp_sync_loop();
    
    Result<ClockOffset> measure_offset_with_node(const NodeId& node_id);
    Result<ClockOffset> sync_with_ntp_server(const std::string& ntp_server);
    
    void update_offset_with_sample(const NodeId& node_id, ClockOffset sample, 
                                  std::chrono::milliseconds uncertainty);
    ClockOffset filter_offset(ClockOffset current, ClockOffset new_sample) const;
    
    void detect_clock_drift();
    void cleanup_expired_offsets();
    
    // Berkeley算法实现
    Result<ClockOffset> berkeley_algorithm();
    
    // NTP客户端实现
    Result<ClockOffset> query_ntp_server(const std::string& server);
    
    // 混合逻辑时钟实现
    HybridLogicalClock increment_logical_clock();
    void update_logical_clock_from_physical();
    
    // 消息处理
    void handle_time_request(const TimeSyncMessage& message);
    void handle_time_response(const TimeSyncMessage& message);
    void handle_offset_update(const TimeSyncMessage& message);
    
    // 事件通知
    void notify_clock_drift_detected(const NodeId& node_id, ClockOffset drift);
    void notify_time_sync_completed(const NodeId& node_id, ClockOffset offset);
    void notify_time_sync_failed(const NodeId& node_id, const std::string& reason);
    void notify_ntp_sync_completed(ClockOffset offset);
};

// 时间戳排序器 - 用于事务排序
class TimestampOrdering {
public:
    explicit TimestampOrdering(std::shared_ptr<TimeService> time_service);
    ~TimestampOrdering() = default;
    
    // 生成时间戳
    HybridLogicalClock generate_timestamp();
    
    // 时间戳比较
    bool is_before(const HybridLogicalClock& ts1, const HybridLogicalClock& ts2) const;
    bool is_concurrent(const HybridLogicalClock& ts1, const HybridLogicalClock& ts2) const;
    
    // 等待时间戳 - 用于等待依赖的事务
    Result<void> wait_for_timestamp(const HybridLogicalClock& target_ts, 
                                   std::chrono::milliseconds timeout);
    
private:
    std::shared_ptr<TimeService> time_service_;
    mutable std::mutex mutex_;
    std::condition_variable timestamp_cv_;
    HybridLogicalClock latest_timestamp_;
};

// 工具函数
namespace time_utils {
    // 时间戳转换
    std::string timestamp_to_string(PhysicalTimestamp timestamp);
    PhysicalTimestamp timestamp_from_string(const std::string& str);
    
    // 时间差计算
    std::chrono::milliseconds time_difference(PhysicalTimestamp t1, PhysicalTimestamp t2);
    
    // 时间同步质量评估
    double calculate_sync_quality(const std::vector<ClockOffsetInfo>& offsets,
                                 std::chrono::milliseconds max_uncertainty);
    
    // 偏移统计
    ClockOffset calculate_median_offset(const std::vector<ClockOffset>& offsets);
    std::chrono::milliseconds calculate_offset_stddev(const std::vector<ClockOffset>& offsets);
    
    // NTP时间戳转换
    uint64_t physical_to_ntp_timestamp(PhysicalTimestamp timestamp);
    PhysicalTimestamp ntp_to_physical_timestamp(uint64_t ntp_timestamp);
}

} // namespace time
} // namespace cache