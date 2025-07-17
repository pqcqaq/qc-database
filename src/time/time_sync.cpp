#include "cache/time/time_sync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace cache {
namespace time {

// HybridLogicalClock 实现

std::string HybridLogicalClock::to_string() const {
    auto time_t = std::chrono::system_clock::to_time_t(physical_time);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S");
    oss << "_" << logical_time;
    return oss.str();
}

HybridLogicalClock HybridLogicalClock::from_string(const std::string& str) {
    HybridLogicalClock hlc;

    size_t pos = str.find_last_of('_');
    if (pos != std::string::npos) {
        std::string time_str = str.substr(0, pos);
        std::string logical_str = str.substr(pos + 1);

        hlc.logical_time = std::stoull(logical_str);

        std::tm tm = {};
        std::istringstream ss(time_str);
        ss >> std::get_time(&tm, "%Y%m%d_%H%M%S");
        hlc.physical_time =
            std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }

    return hlc;
}

// DefaultTimeService 实现

DefaultTimeService::DefaultTimeService()
    : is_running_(false),
      stop_threads_(false),
      local_offset_(0),
      logical_time_(0),
      sync_attempts_(0),
      sync_successes_(0),
      sync_failures_(0) {
    last_physical_time_ = std::chrono::system_clock::now();
}

DefaultTimeService::~DefaultTimeService() {
    if (is_running_) {
        stop();
    }
}

Result<void> DefaultTimeService::start(const TimeSyncConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (is_running_) {
        return Result<void>(Status::OK);
    }

    config_ = config;
    stop_threads_ = false;

    // 启动后台线程
    sync_thread_ = std::thread(&DefaultTimeService::sync_loop, this);
    if (config_.enable_ntp_sync) {
        ntp_thread_ = std::thread(&DefaultTimeService::ntp_sync_loop, this);
    }

    is_running_ = true;
    return Result<void>(Status::OK);
}

Result<void> DefaultTimeService::stop() {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (!is_running_) {
        return Result<void>(Status::OK);
    }

    stop_threads_ = true;

    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }

    if (ntp_thread_.joinable()) {
        ntp_thread_.join();
    }

    is_running_ = false;
    return Result<void>(Status::OK);
}

bool DefaultTimeService::is_running() const { return is_running_; }

PhysicalTimestamp DefaultTimeService::now() const {
    return std::chrono::system_clock::now();
}

PhysicalTimestamp DefaultTimeService::now_synchronized() const {
    auto current_time = std::chrono::system_clock::now();
    return current_time + local_offset_;
}

HybridLogicalClock DefaultTimeService::now_logical() {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (config_.enable_hybrid_logical_clock) {
        return increment_logical_clock();
    } else {
        HybridLogicalClock hlc;
        hlc.physical_time = now_synchronized();
        hlc.logical_time = 0;
        return hlc;
    }
}

HybridLogicalClock DefaultTimeService::update_logical_clock(
    const HybridLogicalClock& received) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (!config_.enable_hybrid_logical_clock) {
        return now_logical();
    }

    auto current_physical = now_synchronized();
    auto current_logical = logical_time_.load();

    // HLC更新规则
    LogicalTimestamp new_logical;
    PhysicalTimestamp new_physical;

    if (current_physical > received.physical_time) {
        // 本地物理时间更新
        new_physical = current_physical;
        new_logical = std::max(current_logical, received.logical_time) + 1;
    } else if (current_physical == received.physical_time) {
        // 相同物理时间，逻辑时间递增
        new_physical = current_physical;
        new_logical = std::max(current_logical, received.logical_time) + 1;
    } else {
        // 接收到的物理时间更新
        new_physical = received.physical_time;
        new_logical = received.logical_time + 1;

        // 检查时钟偏移是否过大
        auto offset = std::chrono::duration_cast<std::chrono::milliseconds>(
            received.physical_time - current_physical);

        if (offset > config_.max_clock_drift) {
            // 时钟偏移过大，记录但不更新物理时间
            new_physical = current_physical;
        }
    }

    logical_time_ = new_logical;
    last_physical_time_ = new_physical;

    HybridLogicalClock result;
    result.physical_time = new_physical;
    result.logical_time = new_logical;

    return result;
}

ClockOffset DefaultTimeService::get_offset(const NodeId& node_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = node_offsets_.find(node_id);
    if (it != node_offsets_.end()) {
        return it->second.offset;
    }

    return ClockOffset(0);
}

Result<void> DefaultTimeService::set_offset(const NodeId& node_id,
                                            ClockOffset offset) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    auto it = node_offsets_.find(node_id);
    if (it != node_offsets_.end()) {
        it->second.offset = offset;
        it->second.last_sync = std::chrono::system_clock::now();
        it->second.sync_count++;
    } else {
        ClockOffsetInfo info;
        info.node_id = node_id;
        info.offset = offset;
        info.last_sync = std::chrono::system_clock::now();
        info.sync_count = 1;
        node_offsets_[node_id] = info;
    }

    return Result<void>(Status::OK);
}

std::vector<ClockOffsetInfo> DefaultTimeService::get_all_offsets() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<ClockOffsetInfo> result;
    for (const auto& [node_id, info] : node_offsets_) {
        result.push_back(info);
    }

    return result;
}

Result<void> DefaultTimeService::sync_with_node(const NodeId& node_id) {
    sync_attempts_++;

    auto measure_result = measure_offset_with_node(node_id);
    if (!measure_result.is_ok()) {
        sync_failures_++;
        return Result<void>::error(measure_result.status,
                                   measure_result.error_message);
    }

    auto offset = measure_result.data;
    auto uncertainty = std::chrono::milliseconds(50);  // 简化的不确定性估算

    update_offset_with_sample(node_id, offset, uncertainty);

    sync_successes_++;

    if (event_handler_) {
        event_handler_->on_time_sync_completed(node_id, offset);
    }

    return Result<void>(Status::OK);
}

Result<void> DefaultTimeService::sync_with_ntp() {
    if (!config_.enable_ntp_sync || config_.ntp_servers.empty()) {
        return Result<void>::error(
            Status::INVALID_ARGUMENT,
            "NTP sync is not enabled or no servers configured");
    }

    for (const auto& server : config_.ntp_servers) {
        auto sync_result = sync_with_ntp_server(server);
        if (sync_result.is_ok()) {
            if (event_handler_) {
                event_handler_->on_ntp_sync_completed(sync_result.data);
            }
            return Result<void>(Status::OK);
        }
    }

    return Result<void>::error(Status::NETWORK_ERROR,
                               "Failed to sync with any NTP server");
}

Result<void> DefaultTimeService::handle_sync_message(
    const TimeSyncMessage& message) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    switch (message.type) {
        case TimeSyncMessageType::TIME_REQUEST:
            handle_time_request(message);
            break;
        case TimeSyncMessageType::TIME_RESPONSE:
            handle_time_response(message);
            break;
        case TimeSyncMessageType::OFFSET_UPDATE:
            handle_offset_update(message);
            break;
        case TimeSyncMessageType::SYNC_HEARTBEAT:
            // 处理同步心跳
            break;
        default:
            return Result<void>::error(Status::INVALID_ARGUMENT,
                                       "Unknown sync message type");
    }

    return Result<void>(Status::OK);
}

void DefaultTimeService::add_node(const NodeId& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    sync_nodes_.insert(node_id);
}

void DefaultTimeService::remove_node(const NodeId& node_id) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    sync_nodes_.erase(node_id);
    node_offsets_.erase(node_id);
}

std::vector<NodeId> DefaultTimeService::get_sync_nodes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return std::vector<NodeId>(sync_nodes_.begin(), sync_nodes_.end());
}

void DefaultTimeService::set_event_handler(
    std::shared_ptr<TimeSyncEventHandler> handler) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    event_handler_ = handler;
}

void DefaultTimeService::update_config(const TimeSyncConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    config_ = config;
}

TimeSyncConfig DefaultTimeService::get_config() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

std::chrono::milliseconds DefaultTimeService::get_estimated_uncertainty()
    const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (node_offsets_.empty()) {
        return std::chrono::milliseconds(1000);  // 默认不确定性
    }

    // 计算所有节点偏移的标准差作为不确定性
    std::vector<double> offsets;
    for (const auto& [node_id, info] : node_offsets_) {
        offsets.push_back(info.offset.count());
    }

    if (offsets.size() < 2) {
        return std::chrono::milliseconds(100);
    }

    double mean = 0.0;
    for (auto offset : offsets) {
        mean += offset;
    }
    mean /= offsets.size();

    double variance = 0.0;
    for (auto offset : offsets) {
        variance += (offset - mean) * (offset - mean);
    }
    variance /= (offsets.size() - 1);

    auto stddev = std::sqrt(variance);
    return std::chrono::milliseconds(static_cast<int64_t>(stddev));
}

double DefaultTimeService::get_sync_quality() const {
    auto uncertainty = get_estimated_uncertainty();
    auto attempts = sync_attempts_.load();
    auto successes = sync_successes_.load();

    if (attempts == 0) {
        return 0.0;
    }

    double success_rate = static_cast<double>(successes) / attempts;
    double uncertainty_factor = 1.0 / (1.0 + uncertainty.count() / 100.0);

    return success_rate * uncertainty_factor;
}

// 私有方法实现

void DefaultTimeService::sync_loop() {
    while (!stop_threads_) {
        try {
            // 定期与其他节点同步时间
            auto nodes = get_sync_nodes();
            for (const auto& node_id : nodes) {
                sync_with_node(node_id);
            }

            // 执行Berkeley算法
            if (nodes.size() >= config_.min_samples) {
                auto berkeley_result = berkeley_algorithm();
                if (berkeley_result.is_ok()) {
                    local_offset_ = berkeley_result.data;
                }
            }

            // 检测时钟漂移
            detect_clock_drift();

            // 清理过期的偏移记录
            cleanup_expired_offsets();

        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }

        std::this_thread::sleep_for(config_.sync_interval);
    }
}

void DefaultTimeService::ntp_sync_loop() {
    while (!stop_threads_) {
        try {
            sync_with_ntp();
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }

        std::this_thread::sleep_for(config_.ntp_sync_interval);
    }
}

Result<ClockOffset> DefaultTimeService::measure_offset_with_node(
    const NodeId& node_id) {
    // 简化的时间偏移测量
    // 实际实现需要通过网络与目标节点通信

    TimeSyncMessage request;
    request.type = TimeSyncMessageType::TIME_REQUEST;
    request.from_node = "local";  // 应该是本节点ID
    request.to_node = node_id;
    request.send_time = now();

    // 模拟网络延迟
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 模拟接收到响应
    TimeSyncMessage response;
    response.type = TimeSyncMessageType::TIME_RESPONSE;
    response.from_node = node_id;
    response.to_node = "local";
    response.receive_time = request.send_time + std::chrono::milliseconds(5);
    response.response_time = now();

    // 计算往返时间和偏移
    auto rtt = response.response_time - request.send_time;
    auto estimated_network_delay = rtt / 2;
    auto remote_time = response.receive_time + estimated_network_delay;
    auto local_time = now();

    auto offset = std::chrono::duration_cast<std::chrono::milliseconds>(
        remote_time - local_time);

    return Result<ClockOffset>(Status::OK, offset);
}

Result<ClockOffset> DefaultTimeService::sync_with_ntp_server(
    const std::string& ntp_server) {
    // 简化的NTP客户端实现
    // 实际实现需要完整的NTP协议

    auto result = query_ntp_server(ntp_server);
    if (result.is_ok()) {
        // 更新本地偏移
        auto filtered_offset = filter_offset(local_offset_, result.data);
        local_offset_ = filtered_offset;
    }

    return result;
}

void DefaultTimeService::update_offset_with_sample(
    const NodeId& node_id, ClockOffset sample,
    std::chrono::milliseconds uncertainty) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    auto it = node_offsets_.find(node_id);
    if (it != node_offsets_.end()) {
        auto& info = it->second;

        // 使用指数移动平均滤波
        auto filtered_offset = filter_offset(info.offset, sample);
        info.offset = filtered_offset;
        info.uncertainty = uncertainty;
        info.last_sync = std::chrono::system_clock::now();
        info.sync_count++;
        info.confidence = std::min(1.0, info.sync_count / 10.0);
    } else {
        ClockOffsetInfo info;
        info.node_id = node_id;
        info.offset = sample;
        info.uncertainty = uncertainty;
        info.last_sync = std::chrono::system_clock::now();
        info.sync_count = 1;
        info.confidence = 0.1;
        node_offsets_[node_id] = info;
    }
}

ClockOffset DefaultTimeService::filter_offset(ClockOffset current,
                                              ClockOffset new_sample) const {
    // 指数移动平均滤波
    double alpha = config_.offset_filter_alpha;
    auto filtered_ms =
        (1.0 - alpha) * current.count() + alpha * new_sample.count();
    return ClockOffset(static_cast<int64_t>(filtered_ms));
}

void DefaultTimeService::detect_clock_drift() {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    for (const auto& [node_id, info] : node_offsets_) {
        if (std::abs(info.offset.count()) > config_.max_clock_drift.count()) {
            if (event_handler_) {
                event_handler_->on_clock_drift_detected(node_id, info.offset);
            }
        }
    }
}

void DefaultTimeService::cleanup_expired_offsets() {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    auto it = node_offsets_.begin();

    while (it != node_offsets_.end()) {
        if (it->second.is_expired(config_.max_offset_age)) {
            it = node_offsets_.erase(it);
        } else {
            ++it;
        }
    }
}

Result<ClockOffset> DefaultTimeService::berkeley_algorithm() {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    if (node_offsets_.size() < config_.min_samples) {
        return Result<ClockOffset>::error(Status::INSUFFICIENT_DATA,
                                          "Not enough offset samples");
    }

    // 收集所有有效的偏移样本
    std::vector<ClockOffset> offsets;
    for (const auto& [node_id, info] : node_offsets_) {
        if (info.confidence > 0.5) {  // 只使用高置信度的样本
            offsets.push_back(info.offset);
        }
    }

    if (offsets.empty()) {
        return Result<ClockOffset>::error(Status::INSUFFICIENT_DATA,
                                          "No high-confidence offset samples");
    }

    // 计算中位数偏移（更鲁棒than平均值）
    std::sort(offsets.begin(), offsets.end());
    ClockOffset median_offset;

    if (offsets.size() % 2 == 0) {
        auto mid1 = offsets[offsets.size() / 2 - 1];
        auto mid2 = offsets[offsets.size() / 2];
        median_offset = ClockOffset((mid1.count() + mid2.count()) / 2);
    } else {
        median_offset = offsets[offsets.size() / 2];
    }

    return Result<ClockOffset>(Status::OK, median_offset);
}

Result<ClockOffset> DefaultTimeService::query_ntp_server(
    const std::string& server) {
    // 简化的NTP查询实现
    // 实际实现需要UDP套接字和NTP数据包格式

    try {
        // 模拟NTP查询延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 模拟NTP服务器返回的时间偏移
        // 实际情况下这里会是真实的网络通信
        auto simulated_offset =
            ClockOffset(std::rand() % 100 - 50);  // -50ms to +50ms

        return Result<ClockOffset>(Status::OK, simulated_offset);

    } catch (const std::exception& e) {
        return Result<ClockOffset>::error(
            Status::NETWORK_ERROR,
            "NTP query failed: " + std::string(e.what()));
    }
}

HybridLogicalClock DefaultTimeService::increment_logical_clock() {
    auto current_physical = now_synchronized();
    auto current_logical = logical_time_.load();

    HybridLogicalClock result;

    if (current_physical > last_physical_time_) {
        // 物理时间前进，重置逻辑时间
        result.physical_time = current_physical;
        result.logical_time = 0;
        logical_time_ = 0;
    } else {
        // 相同物理时间，递增逻辑时间
        result.physical_time = last_physical_time_;
        result.logical_time = current_logical + 1;
        logical_time_ = current_logical + 1;
    }

    last_physical_time_ = result.physical_time;

    return result;
}

void DefaultTimeService::update_logical_clock_from_physical() {
    auto current_physical = now_synchronized();

    if (current_physical > last_physical_time_) {
        last_physical_time_ = current_physical;
        logical_time_ = 0;
    }
}

void DefaultTimeService::handle_time_request(const TimeSyncMessage& message) {
    // 处理时间请求，返回当前时间
    TimeSyncMessage response;
    response.type = TimeSyncMessageType::TIME_RESPONSE;
    response.from_node = "local";  // 应该是本节点ID
    response.to_node = message.from_node;
    response.send_time = message.send_time;
    response.receive_time = now_synchronized();
    response.response_time = now_synchronized();

    // 发送响应（这里需要实际的网络发送逻辑）
}

void DefaultTimeService::handle_time_response(const TimeSyncMessage& message) {
    // 处理时间响应，计算偏移
    auto rtt = message.response_time - message.send_time;
    auto estimated_delay = rtt / 2;
    auto remote_time = message.receive_time + estimated_delay;
    auto local_time = now_synchronized();

    auto offset = std::chrono::duration_cast<std::chrono::milliseconds>(
        remote_time - local_time);
    auto uncertainty =
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt / 4);

    update_offset_with_sample(message.from_node, offset, uncertainty);
}

void DefaultTimeService::handle_offset_update(const TimeSyncMessage& message) {
    // 处理偏移更新消息
    update_offset_with_sample(message.from_node, message.estimated_offset,
                              std::chrono::milliseconds(50));
}

void DefaultTimeService::notify_clock_drift_detected(const NodeId& node_id,
                                                     ClockOffset drift) {
    if (event_handler_) {
        event_handler_->on_clock_drift_detected(node_id, drift);
    }
}

void DefaultTimeService::notify_time_sync_completed(const NodeId& node_id,
                                                    ClockOffset offset) {
    if (event_handler_) {
        event_handler_->on_time_sync_completed(node_id, offset);
    }
}

void DefaultTimeService::notify_time_sync_failed(const NodeId& node_id,
                                                 const std::string& reason) {
    if (event_handler_) {
        event_handler_->on_time_sync_failed(node_id, reason);
    }
}

void DefaultTimeService::notify_ntp_sync_completed(ClockOffset offset) {
    if (event_handler_) {
        event_handler_->on_ntp_sync_completed(offset);
    }
}

// TimestampOrdering 实现

TimestampOrdering::TimestampOrdering(std::shared_ptr<TimeService> time_service)
    : time_service_(time_service) {
    latest_timestamp_ = time_service_->now_logical();
}

HybridLogicalClock TimestampOrdering::generate_timestamp() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto new_timestamp = time_service_->now_logical();

    // 确保时间戳单调递增
    if (new_timestamp <= latest_timestamp_) {
        new_timestamp.logical_time = latest_timestamp_.logical_time + 1;
        new_timestamp.physical_time = latest_timestamp_.physical_time;
    }

    latest_timestamp_ = new_timestamp;
    timestamp_cv_.notify_all();

    return new_timestamp;
}

bool TimestampOrdering::is_before(const HybridLogicalClock& ts1,
                                  const HybridLogicalClock& ts2) const {
    return ts1 < ts2;
}

bool TimestampOrdering::is_concurrent(const HybridLogicalClock& ts1,
                                      const HybridLogicalClock& ts2) const {
    // 如果物理时间差很小且逻辑时间相近，认为是并发的
    auto time_diff =
        std::abs(std::chrono::duration_cast<std::chrono::milliseconds>(
                     ts1.physical_time - ts2.physical_time)
                     .count());
    auto logical_diff =
        std::abs(static_cast<int64_t>(ts1.logical_time - ts2.logical_time));

    return time_diff < 100 && logical_diff < 10;  // 100ms内且逻辑时间差小于10
}

Result<void> TimestampOrdering::wait_for_timestamp(
    const HybridLogicalClock& target_ts, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    bool result = timestamp_cv_.wait_until(lock, deadline, [this, &target_ts] {
        return latest_timestamp_ >= target_ts;
    });

    if (!result) {
        return Result<void>::error(Status::TIMEOUT,
                                   "Timeout waiting for timestamp");
    }

    return Result<void>(Status::OK);
}

// 工具函数实现
namespace time_utils {

std::string timestamp_to_string(PhysicalTimestamp timestamp) {
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  timestamp.time_since_epoch()) %
              1000;
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

PhysicalTimestamp timestamp_from_string(const std::string& str) {
    std::tm tm = {};
    std::istringstream ss(str);

    // 解析日期时间部分
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

    auto timestamp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    // 解析毫秒部分
    if (ss.peek() == '.') {
        ss.ignore(1);  // 跳过'.'
        int ms;
        ss >> ms;
        timestamp += std::chrono::milliseconds(ms);
    }

    return timestamp;
}

std::chrono::milliseconds time_difference(PhysicalTimestamp t1,
                                          PhysicalTimestamp t2) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t2);
}

double calculate_sync_quality(const std::vector<ClockOffsetInfo>& offsets,
                              std::chrono::milliseconds max_uncertainty) {
    if (offsets.empty()) {
        return 0.0;
    }

    double total_confidence = 0.0;
    double total_uncertainty_factor = 0.0;

    for (const auto& offset : offsets) {
        total_confidence += offset.confidence;

        double uncertainty_factor =
            1.0 - static_cast<double>(offset.uncertainty.count()) /
                      max_uncertainty.count();
        uncertainty_factor = std::max(0.0, uncertainty_factor);
        total_uncertainty_factor += uncertainty_factor;
    }

    double avg_confidence = total_confidence / offsets.size();
    double avg_uncertainty_factor = total_uncertainty_factor / offsets.size();

    return (avg_confidence + avg_uncertainty_factor) / 2.0;
}

ClockOffset calculate_median_offset(const std::vector<ClockOffset>& offsets) {
    if (offsets.empty()) {
        return ClockOffset(0);
    }

    auto sorted_offsets = offsets;
    std::sort(sorted_offsets.begin(), sorted_offsets.end());

    if (sorted_offsets.size() % 2 == 0) {
        auto mid1 = sorted_offsets[sorted_offsets.size() / 2 - 1];
        auto mid2 = sorted_offsets[sorted_offsets.size() / 2];
        return ClockOffset((mid1.count() + mid2.count()) / 2);
    } else {
        return sorted_offsets[sorted_offsets.size() / 2];
    }
}

std::chrono::milliseconds calculate_offset_stddev(
    const std::vector<ClockOffset>& offsets) {
    if (offsets.size() < 2) {
        return std::chrono::milliseconds(0);
    }

    double mean = 0.0;
    for (const auto& offset : offsets) {
        mean += offset.count();
    }
    mean /= offsets.size();

    double variance = 0.0;
    for (const auto& offset : offsets) {
        double diff = offset.count() - mean;
        variance += diff * diff;
    }
    variance /= (offsets.size() - 1);

    return std::chrono::milliseconds(static_cast<int64_t>(std::sqrt(variance)));
}

uint64_t physical_to_ntp_timestamp(PhysicalTimestamp timestamp) {
    // NTP时间戳从1900年1月1日开始
    auto unix_epoch = std::chrono::system_clock::from_time_t(0);
    auto ntp_epoch =
        unix_epoch - std::chrono::seconds(2208988800ULL);  // 1900-1970差异

    auto duration_since_ntp_epoch = timestamp - ntp_epoch;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        duration_since_ntp_epoch);
    auto fraction = duration_since_ntp_epoch - seconds;

    uint64_t ntp_seconds = seconds.count();
    uint64_t ntp_fraction =
        std::chrono::duration_cast<std::chrono::nanoseconds>(fraction).count() *
        (1ULL << 32) / 1000000000ULL;

    return (ntp_seconds << 32) | ntp_fraction;
}

PhysicalTimestamp ntp_to_physical_timestamp(uint64_t ntp_timestamp) {
    uint32_t seconds = (ntp_timestamp >> 32) & 0xFFFFFFFF;
    uint32_t fraction = ntp_timestamp & 0xFFFFFFFF;

    // 转换为Unix时间戳
    uint64_t unix_seconds = seconds - 2208988800ULL;
    uint64_t nanoseconds =
        (static_cast<uint64_t>(fraction) * 1000000000ULL) >> 32;

    auto unix_epoch = std::chrono::system_clock::from_time_t(0);
    return unix_epoch + std::chrono::seconds(unix_seconds) +
           std::chrono::nanoseconds(nanoseconds);
}

}  // namespace time_utils

}  // namespace time
}  // namespace cache