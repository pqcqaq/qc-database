#include "cache/hotspot/hotspot_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace cache {
namespace hotspot {

// Simple DefaultHotspotHandler implementation
class DefaultHotspotHandler : public HotspotHandler {
   public:
    Result<void> apply_strategy(const MultiLevelKey& key,
                                HotspotStrategy strategy) override {
        // Simple implementation - in a real system this would integrate with
        // the cache system
        return Result<void>(Status::OK);
    }

    Result<void> remove_strategy(const MultiLevelKey& key,
                                 HotspotStrategy strategy) override {
        return Result<void>(Status::OK);
    }

    std::vector<HotspotStrategy> get_applied_strategies(
        const MultiLevelKey& key) const override {
        return {};
    }

    bool is_strategy_applied(const MultiLevelKey& key,
                             HotspotStrategy strategy) const override {
        return false;
    }

    double evaluate_strategy_effectiveness(
        const MultiLevelKey& key, HotspotStrategy strategy) const override {
        return 1.0;
    }
};

// DefaultHotspotManager 实现

DefaultHotspotManager::DefaultHotspotManager()
    : is_running_(false), stop_threads_(false) {
    access_recorder_ = std::make_unique<DefaultAccessRecorder>();
    hotspot_handler_ = std::make_unique<DefaultHotspotHandler>();
}

DefaultHotspotManager::~DefaultHotspotManager() {
    if (is_running_) {
        stop();
    }
}

Result<void> DefaultHotspotManager::start(
    const HotspotDetectionConfig& detection_config,
    const HotspotHandlingConfig& handling_config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (is_running_) {
        return Result<void>(Status::OK);
    }

    detection_config_ = detection_config;
    handling_config_ = handling_config;

    stop_threads_ = false;

    // 启动后台线程
    detection_thread_ =
        std::thread(&DefaultHotspotManager::detection_loop, this);
    handling_thread_ = std::thread(&DefaultHotspotManager::handling_loop, this);
    cleanup_thread_ = std::thread(&DefaultHotspotManager::cleanup_loop, this);

    is_running_ = true;

    return Result<void>(Status::OK);
}

Result<void> DefaultHotspotManager::stop() {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    if (!is_running_) {
        return Result<void>(Status::OK);
    }

    stop_threads_ = true;

    // 等待线程结束
    if (detection_thread_.joinable()) {
        detection_thread_.join();
    }
    if (handling_thread_.joinable()) {
        handling_thread_.join();
    }
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }

    is_running_ = false;

    return Result<void>(Status::OK);
}

bool DefaultHotspotManager::is_running() const { return is_running_; }

void DefaultHotspotManager::record_access(const MultiLevelKey& key,
                                          OperationType op_type,
                                          const NodeId& node_id) {
    if (!is_running_) {
        return;
    }

    if (access_recorder_) {
        access_recorder_->record_access(key, op_type, node_id);
    }
}

std::vector<HotspotKeyInfo> DefaultHotspotManager::detect_hotspots() {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<HotspotKeyInfo> result;

    if (!access_recorder_) {
        return result;
    }

    // 获取访问统计并检测热点
    auto top_keys =
        access_recorder_->get_top_keys(1000);  // Get top accessed keys

    for (const auto& key : top_keys) {
        HotspotKeyInfo key_info = access_recorder_->get_key_stats(key);
        key_info.level = calculate_hotspot_level(key_info);

        if (key_info.level != HotspotLevel::NORMAL) {
            result.push_back(key_info);
        }
    }

    return result;
}

std::vector<HotspotKeyInfo> DefaultHotspotManager::get_current_hotspots()
    const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<HotspotKeyInfo> result;
    result.reserve(current_hotspots_.size());

    for (const auto& [key, hotspot] : current_hotspots_) {
        result.push_back(hotspot);
    }

    return result;
}

HotspotLevel DefaultHotspotManager::get_hotspot_level(
    const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = current_hotspots_.find(key);
    if (it != current_hotspots_.end()) {
        return it->second.level;
    }

    return HotspotLevel::NORMAL;
}

Result<void> DefaultHotspotManager::handle_hotspot(
    const MultiLevelKey& key, const std::vector<HotspotStrategy>& strategies) {
    if (!hotspot_handler_) {
        return Result<void>::error(Status::NOT_IMPLEMENTED,
                                   "Hotspot handler not available");
    }

    auto strategies_to_use = strategies;
    if (strategies_to_use.empty()) {
        // 使用默认策略
        strategies_to_use = {HotspotStrategy::REPLICATION,
                             HotspotStrategy::LOAD_BALANCING};
    }

    for (auto strategy : strategies_to_use) {
        auto result = apply_strategy_internal(key, strategy);
        if (result.is_ok()) {
            notify_strategy_applied(key, strategy);
            return Result<void>(Status::OK);
        }
    }

    return Result<void>::error(Status::STORAGE_ERROR,
                               "All hotspot handling strategies failed");
}

Result<void> DefaultHotspotManager::resolve_hotspot(const MultiLevelKey& key) {
    std::lock_guard<std::shared_mutex> lock(mutex_);

    auto it = current_hotspots_.find(key);
    if (it != current_hotspots_.end()) {
        current_hotspots_.erase(it);
        notify_hotspot_resolved(key);
    }

    return Result<void>(Status::OK);
}

std::vector<MultiLevelKey> DefaultHotspotManager::get_top_accessed_keys(
    size_t limit) const {
    if (!access_recorder_) {
        return {};
    }

    return access_recorder_->get_top_keys(limit);
}

HotspotKeyInfo DefaultHotspotManager::get_key_info(
    const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = current_hotspots_.find(key);
    if (it != current_hotspots_.end()) {
        return it->second;
    }

    // 如果不是当前热点，从访问记录器获取信息
    HotspotKeyInfo info;
    if (access_recorder_) {
        info = access_recorder_->get_key_stats(key);
    } else {
        info.key = key;
        info.level = HotspotLevel::NORMAL;
        info.total_access_count = 0;
    }

    return info;
}

std::vector<HotspotEvent> DefaultHotspotManager::get_recent_events(
    size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<HotspotEvent> result;
    auto events_copy = recent_events_;

    size_t count = 0;
    while (!events_copy.empty() && count < limit) {
        result.push_back(events_copy.front());
        events_copy.pop();
        count++;
    }

    return result;
}

// 私有方法实现

void DefaultHotspotManager::detection_loop() {
    while (!stop_threads_) {
        try {
            auto detected = detect_hotspots();

            std::lock_guard<std::shared_mutex> lock(mutex_);

            // 更新当前热点
            for (const auto& hotspot : detected) {
                current_hotspots_[hotspot.key] = hotspot;
                notify_hotspot_detected(hotspot);
            }

        } catch (const std::exception& e) {
            // 日志记录错误
        }

        std::this_thread::sleep_for(detection_config_.detection_window);
    }
}

void DefaultHotspotManager::handling_loop() {
    while (!stop_threads_) {
        try {
            std::vector<HotspotKeyInfo> hotspots_to_handle;

            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                for (const auto& [key, hotspot] : current_hotspots_) {
                    if (hotspot.level >= HotspotLevel::HOT) {
                        hotspots_to_handle.push_back(hotspot);
                    }
                }
            }

            for (const auto& hotspot : hotspots_to_handle) {
                handle_hotspot(hotspot.key);
            }

        } catch (const std::exception& e) {
            // 日志记录错误
        }

        std::this_thread::sleep_for(handling_config_.rate_limit_window);
    }
}

void DefaultHotspotManager::cleanup_loop() {
    while (!stop_threads_) {
        try {
            auto now = std::chrono::system_clock::now();
            std::lock_guard<std::shared_mutex> lock(mutex_);

            // 清理过期的热点
            auto it = current_hotspots_.begin();
            while (it != current_hotspots_.end()) {
                auto time_diff =
                    std::chrono::duration_cast<std::chrono::minutes>(
                        now - it->second.last_access)
                        .count();

                if (time_diff >
                    std::chrono::duration_cast<std::chrono::minutes>(
                        detection_config_.cooling_period)
                        .count()) {
                    notify_hotspot_resolved(it->first);
                    it = current_hotspots_.erase(it);
                } else {
                    ++it;
                }
            }

            // 清理过期的事件
            // (使用固定大小限制，因为配置中没有max_events_history字段)
            while (recent_events_.size() > 1000) {
                recent_events_.pop();
            }

        } catch (const std::exception& e) {
            // 日志记录错误
        }

        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
}

HotspotLevel DefaultHotspotManager::calculate_hotspot_level(
    const HotspotKeyInfo& key_info) const {
    // 简化的热点等级计算
    if (key_info.total_access_count >= detection_config_.critical_threshold) {
        return HotspotLevel::CRITICAL;
    } else if (key_info.total_access_count >= detection_config_.hot_threshold) {
        return HotspotLevel::HOT;
    } else if (key_info.total_access_count >=
               detection_config_.warm_threshold) {
        return HotspotLevel::WARM;
    }

    return HotspotLevel::NORMAL;
}

AccessPattern DefaultHotspotManager::detect_access_pattern(
    const HotspotKeyInfo& key_info) const {
    // 简化的访问模式检测
    return AccessPattern::BURST;  // 默认突发模式
}

std::vector<HotspotStrategy> DefaultHotspotManager::select_strategies(
    const HotspotKeyInfo& hotspot) const {
    std::vector<HotspotStrategy> strategies;

    switch (hotspot.level) {
        case HotspotLevel::CRITICAL:
            strategies = {HotspotStrategy::REPLICATION,
                          HotspotStrategy::PARTITIONING,
                          HotspotStrategy::LOAD_BALANCING};
            break;
        case HotspotLevel::HOT:
            strategies = {HotspotStrategy::REPLICATION,
                          HotspotStrategy::LOAD_BALANCING};
            break;
        case HotspotLevel::WARM:
            strategies = {HotspotStrategy::LOAD_BALANCING};
            break;
        default:
            break;
    }

    return strategies;
}

Result<void> DefaultHotspotManager::apply_strategy_internal(
    const MultiLevelKey& key, HotspotStrategy strategy) {
    if (!hotspot_handler_) {
        return Result<void>::error(Status::NOT_IMPLEMENTED,
                                   "Hotspot handler not available");
    }

    return hotspot_handler_->apply_strategy(key, strategy);
}

void DefaultHotspotManager::add_event(const HotspotEvent& event) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    recent_events_.push(event);

    if (event_handler_) {
        // Call appropriate method based on event type
        switch (event.type) {
            case HotspotEvent::HOTSPOT_DETECTED:
                // We need the hotspot info, so this should be called from the
                // specific notify methods
                break;
            case HotspotEvent::HOTSPOT_RESOLVED:
                event_handler_->on_hotspot_resolved(event.key);
                break;
            case HotspotEvent::STRATEGY_APPLIED:
                event_handler_->on_strategy_applied(event.key, event.strategy);
                break;
            default:
                break;
        }
    }
}

void DefaultHotspotManager::notify_hotspot_detected(
    const HotspotKeyInfo& hotspot) {
    HotspotEvent event;
    event.type = HotspotEvent::HOTSPOT_DETECTED;
    event.key = hotspot.key;
    event.level = hotspot.level;
    event.timestamp = std::chrono::system_clock::now();
    event.details = "Hotspot detected for key: " + hotspot.key.to_string();

    if (event_handler_) {
        event_handler_->on_hotspot_detected(hotspot);
    }

    add_event(event);
}

void DefaultHotspotManager::notify_hotspot_resolved(const MultiLevelKey& key) {
    HotspotEvent event;
    event.type = HotspotEvent::HOTSPOT_RESOLVED;
    event.key = key;
    event.level = HotspotLevel::NORMAL;
    event.timestamp = std::chrono::system_clock::now();
    event.details = "Hotspot resolved for key: " + key.to_string();

    add_event(event);
}

void DefaultHotspotManager::notify_strategy_applied(const MultiLevelKey& key,
                                                    HotspotStrategy strategy) {
    HotspotEvent event;
    event.type = HotspotEvent::STRATEGY_APPLIED;
    event.key = key;
    event.strategy = strategy;
    event.timestamp = std::chrono::system_clock::now();
    event.details = "Strategy applied for key: " + key.to_string();

    add_event(event);
}

void DefaultHotspotManager::notify_strategy_failed(const MultiLevelKey& key,
                                                   HotspotStrategy strategy,
                                                   const std::string& reason) {
    HotspotEvent event;
    event.type = HotspotEvent::STRATEGY_REMOVED;  // Use available enum value
                                                  // instead of STRATEGY_FAILED
    event.key = key;
    event.strategy = strategy;
    event.timestamp = std::chrono::system_clock::now();
    event.details =
        "Strategy failed for key: " + key.to_string() + ", reason: " + reason;

    if (event_handler_) {
        event_handler_->on_strategy_failed(key, strategy, reason);
    }

    add_event(event);
}

// 工具函数实现
namespace hotspot_utils {

std::string hotspot_level_to_string(HotspotLevel level) {
    switch (level) {
        case HotspotLevel::NORMAL:
            return "NORMAL";
        case HotspotLevel::WARM:
            return "WARM";
        case HotspotLevel::HOT:
            return "HOT";
        case HotspotLevel::CRITICAL:
            return "CRITICAL";
        default:
            return "UNKNOWN";
    }
}

HotspotLevel string_to_hotspot_level(const std::string& str) {
    if (str == "NORMAL") return HotspotLevel::NORMAL;
    if (str == "WARM") return HotspotLevel::WARM;
    if (str == "HOT") return HotspotLevel::HOT;
    if (str == "CRITICAL") return HotspotLevel::CRITICAL;
    return HotspotLevel::NORMAL;
}

std::string strategy_to_string(HotspotStrategy strategy) {
    switch (strategy) {
        case HotspotStrategy::REPLICATION:
            return "REPLICATION";
        case HotspotStrategy::PARTITIONING:
            return "PARTITIONING";
        case HotspotStrategy::CACHING:
            return "CACHING";
        case HotspotStrategy::LOAD_BALANCING:
            return "LOAD_BALANCING";
        case HotspotStrategy::RATE_LIMITING:
            return "RATE_LIMITING";
        default:
            return "UNKNOWN";
    }
}

HotspotStrategy string_to_strategy(const std::string& str) {
    if (str == "REPLICATION") return HotspotStrategy::REPLICATION;
    if (str == "PARTITIONING") return HotspotStrategy::PARTITIONING;
    if (str == "CACHING") return HotspotStrategy::CACHING;
    if (str == "LOAD_BALANCING") return HotspotStrategy::LOAD_BALANCING;
    if (str == "RATE_LIMITING") return HotspotStrategy::RATE_LIMITING;
    return HotspotStrategy::REPLICATION;
}

std::string pattern_to_string(AccessPattern pattern) {
    switch (pattern) {
        case AccessPattern::READ_HEAVY:
            return "READ_HEAVY";
        case AccessPattern::WRITE_HEAVY:
            return "WRITE_HEAVY";
        case AccessPattern::MIXED:
            return "MIXED";
        case AccessPattern::BURST:
            return "BURST";
        case AccessPattern::PERIODIC:
            return "PERIODIC";
        default:
            return "UNKNOWN";
    }
}

AccessPattern string_to_pattern(const std::string& str) {
    if (str == "READ_HEAVY") return AccessPattern::READ_HEAVY;
    if (str == "WRITE_HEAVY") return AccessPattern::WRITE_HEAVY;
    if (str == "MIXED") return AccessPattern::MIXED;
    if (str == "BURST") return AccessPattern::BURST;
    if (str == "PERIODIC") return AccessPattern::PERIODIC;
    return AccessPattern::MIXED;
}

double calculate_load_balance_factor(
    const std::unordered_map<NodeId, uint64_t>& node_loads) {
    if (node_loads.empty()) return 1.0;

    double sum = 0.0;
    double sum_squares = 0.0;

    for (const auto& [node_id, load] : node_loads) {
        sum += load;
        sum_squares += load * load;
    }

    double mean = sum / node_loads.size();
    double variance = (sum_squares / node_loads.size()) - (mean * mean);

    return variance / (mean * mean);
}

std::vector<NodeId> find_overloaded_nodes(
    const std::unordered_map<NodeId, uint64_t>& node_loads, double threshold) {
    std::vector<NodeId> overloaded;

    double total_load = 0.0;
    for (const auto& [node_id, load] : node_loads) {
        total_load += load;
    }

    double average_load = total_load / node_loads.size();
    double overload_threshold = average_load * threshold;

    for (const auto& [node_id, load] : node_loads) {
        if (load > overload_threshold) {
            overloaded.push_back(node_id);
        }
    }

    return overloaded;
}

double calculate_access_entropy(const std::vector<uint64_t>& access_counts) {
    if (access_counts.empty()) return 0.0;

    uint64_t total = 0;
    for (auto count : access_counts) {
        total += count;
    }

    if (total == 0) return 0.0;

    double entropy = 0.0;
    for (auto count : access_counts) {
        if (count > 0) {
            double probability = static_cast<double>(count) / total;
            entropy -= probability * std::log2(probability);
        }
    }

    return entropy;
}

bool is_power_law_distribution(const std::vector<uint64_t>& access_counts) {
    // 简化的幂律分布检测
    if (access_counts.size() < 10) return false;

    auto sorted_counts = access_counts;
    std::sort(sorted_counts.rbegin(), sorted_counts.rend());

    // 检查前20%的项是否占据80%以上的访问量
    size_t top_20_percent = std::max(1UL, sorted_counts.size() / 5);
    uint64_t top_sum = 0;
    uint64_t total_sum = 0;

    for (size_t i = 0; i < top_20_percent; ++i) {
        top_sum += sorted_counts[i];
    }

    for (auto count : sorted_counts) {
        total_sum += count;
    }

    return total_sum > 0 && (static_cast<double>(top_sum) / total_sum) >= 0.8;
}

}  // namespace hotspot_utils

}  // namespace hotspot
}  // namespace cache