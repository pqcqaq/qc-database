#include "cache/hotspot/hotspot_manager.h"
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace cache {
namespace hotspot {

// DefaultHotspotManager 实现

DefaultHotspotManager::DefaultHotspotManager() 
    : is_running_(false), stop_threads_(false), total_requests_(0) {
}

DefaultHotspotManager::~DefaultHotspotManager() {
    if (is_running_) {
        stop();
    }
}

Result<void> DefaultHotspotManager::start(const HotspotConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    if (is_running_) {
        return Result<void>(Status::OK);
    }
    
    config_ = config;
    stop_threads_ = false;
    
    // 启动后台线程
    detection_thread_ = std::thread(&DefaultHotspotManager::detection_loop, this);
    processing_thread_ = std::thread(&DefaultHotspotManager::processing_loop, this);
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
    
    if (detection_thread_.joinable()) {
        detection_thread_.join();
    }
    
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
    
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
    is_running_ = false;
    return Result<void>(Status::OK);
}

bool DefaultHotspotManager::is_running() const {
    return is_running_;
}

void DefaultHotspotManager::record_access(const MultiLevelKey& key, const AccessInfo& access_info) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    total_requests_++;
    
    // 更新key统计信息
    auto& stats = key_stats_[key];
    stats.key = key;
    stats.access_count++;
    stats.last_access = access_info.timestamp;
    stats.total_size += access_info.size;
    
    // 记录地理位置分布
    if (!access_info.location.empty()) {
        stats.location_counts[access_info.location]++;
    }
    
    // 记录操作类型分布
    stats.operation_counts[access_info.operation]++;
    
    // 更新时间窗口统计
    update_time_window_stats(key, access_info);
    
    // 更新访问模式
    update_access_pattern(key, access_info);
    
    // 实时热点检测
    if (config_.enable_realtime_detection) {
        check_instant_hotspot(key, stats);
    }
}

std::vector<HotspotInfo> DefaultHotspotManager::get_hotspots() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<HotspotInfo> result;
    for (const auto& [key, hotspot] : detected_hotspots_) {
        if (hotspot.is_active) {
            result.push_back(hotspot);
        }
    }
    
    // 按热度排序
    std::sort(result.begin(), result.end(), [](const HotspotInfo& a, const HotspotInfo& b) {
        return a.heat_score > b.heat_score;
    });
    
    return result;
}

bool DefaultHotspotManager::is_hotspot(const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = detected_hotspots_.find(key);
    return it != detected_hotspots_.end() && it->second.is_active;
}

double DefaultHotspotManager::get_heat_score(const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = detected_hotspots_.find(key);
    if (it != detected_hotspots_.end()) {
        return it->second.heat_score;
    }
    
    return 0.0;
}

Result<void> DefaultHotspotManager::handle_hotspot(const MultiLevelKey& key, HandlingStrategy strategy) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    
    auto it = detected_hotspots_.find(key);
    if (it == detected_hotspots_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Key is not a hotspot: " + key.to_string());
    }
    
    auto& hotspot = it->second;
    auto result = apply_handling_strategy(hotspot, strategy);
    
    if (result.is_ok()) {
        hotspot.applied_strategies.push_back(strategy);
        
        // 记录处理效果
        HandlingResult handling_result;
        handling_result.key = key;
        handling_result.strategy = strategy;
        handling_result.applied_at = std::chrono::system_clock::now();
        handling_result.success = true;
        
        handling_results_[key].push_back(handling_result);
        
        notify_hotspot_handled(key, strategy);
    }
    
    return result;
}

std::vector<HandlingStrategy> DefaultHotspotManager::suggest_strategies(const MultiLevelKey& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = detected_hotspots_.find(key);
    if (it == detected_hotspots_.end()) {
        return {};
    }
    
    const auto& hotspot = it->second;
    return analyze_optimal_strategies(hotspot);
}

void DefaultHotspotManager::set_event_handler(std::shared_ptr<HotspotEventHandler> handler) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    event_handler_ = handler;
}

void DefaultHotspotManager::update_config(const HotspotConfig& config) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    config_ = config;
}

HotspotConfig DefaultHotspotManager::get_config() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return config_;
}

std::vector<KeyStats> DefaultHotspotManager::get_top_keys(size_t count) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    std::vector<KeyStats> result;
    for (const auto& [key, stats] : key_stats_) {
        result.push_back(stats);
    }
    
    // 按访问次数排序
    std::sort(result.begin(), result.end(), [](const KeyStats& a, const KeyStats& b) {
        return a.access_count > b.access_count;
    });
    
    if (result.size() > count) {
        result.resize(count);
    }
    
    return result;
}

HotspotSummary DefaultHotspotManager::get_summary() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    HotspotSummary summary;
    summary.total_keys = key_stats_.size();
    summary.total_requests = total_requests_;
    summary.active_hotspots = 0;
    summary.handled_hotspots = 0;
    
    for (const auto& [key, hotspot] : detected_hotspots_) {
        if (hotspot.is_active) {
            summary.active_hotspots++;
        }
        if (!hotspot.applied_strategies.empty()) {
            summary.handled_hotspots++;
        }
    }
    
    return summary;
}

Result<void> DefaultHotspotManager::force_check() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    return perform_detection();
}

// 私有方法实现

void DefaultHotspotManager::detection_loop() {
    while (!stop_threads_) {
        try {
            std::lock_guard<std::shared_mutex> lock(mutex_);
            perform_detection();
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
        
        std::this_thread::sleep_for(config_.detection_interval);
    }
}

void DefaultHotspotManager::processing_loop() {
    while (!stop_threads_) {
        try {
            std::lock_guard<std::shared_mutex> lock(mutex_);
            process_hotspots();
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
        
        std::this_thread::sleep_for(config_.processing_interval);
    }
}

void DefaultHotspotManager::cleanup_loop() {
    while (!stop_threads_) {
        try {
            std::lock_guard<std::shared_mutex> lock(mutex_);
            cleanup_expired_data();
        } catch (const std::exception& e) {
            // 记录异常但继续运行
        }
        
        std::this_thread::sleep_for(config_.cleanup_interval);
    }
}

Result<void> DefaultHotspotManager::perform_detection() {
    auto now = std::chrono::system_clock::now();
    
    // 计算全局平均访问频率
    double global_avg_frequency = calculate_global_average_frequency();
    
    for (auto& [key, stats] : key_stats_) {
        // 频率检测
        bool frequency_hotspot = detect_frequency_hotspot(stats, global_avg_frequency);
        
        // 地理分布检测
        bool geo_hotspot = detect_geographic_hotspot(stats);
        
        // 访问模式检测
        bool pattern_hotspot = detect_pattern_hotspot(stats);
        
        // 时间集中度检测
        bool temporal_hotspot = detect_temporal_hotspot(stats);
        
        bool is_hotspot = frequency_hotspot || geo_hotspot || pattern_hotspot || temporal_hotspot;
        
        if (is_hotspot) {
            auto existing_it = detected_hotspots_.find(key);
            if (existing_it == detected_hotspots_.end()) {
                // 新热点
                HotspotInfo hotspot;
                hotspot.key = key;
                hotspot.detected_at = now;
                hotspot.last_updated = now;
                hotspot.is_active = true;
                hotspot.heat_score = calculate_heat_score(stats);
                hotspot.hotspot_type = determine_hotspot_type(frequency_hotspot, geo_hotspot, 
                                                             pattern_hotspot, temporal_hotspot);
                
                detected_hotspots_[key] = hotspot;
                notify_hotspot_detected(key, hotspot);
                
            } else {
                // 更新现有热点
                auto& hotspot = existing_it->second;
                hotspot.last_updated = now;
                hotspot.heat_score = calculate_heat_score(stats);
                hotspot.is_active = true;
            }
        } else {
            // 检查是否需要降级热点
            auto existing_it = detected_hotspots_.find(key);
            if (existing_it != detected_hotspots_.end()) {
                auto& hotspot = existing_it->second;
                auto inactive_duration = now - hotspot.last_updated;
                
                if (inactive_duration > config_.hotspot_cool_down_time) {
                    hotspot.is_active = false;
                    notify_hotspot_cooled_down(key);
                }
            }
        }
    }
    
    return Result<void>(Status::OK);
}

void DefaultHotspotManager::process_hotspots() {
    if (!config_.enable_auto_handling) {
        return;
    }
    
    for (auto& [key, hotspot] : detected_hotspots_) {
        if (!hotspot.is_active) {
            continue;
        }
        
        // 分析最佳处理策略
        auto strategies = analyze_optimal_strategies(hotspot);
        
        // 应用未尝试过的策略
        for (auto strategy : strategies) {
            auto applied_it = std::find(hotspot.applied_strategies.begin(), 
                                       hotspot.applied_strategies.end(), strategy);
            
            if (applied_it == hotspot.applied_strategies.end()) {
                auto result = apply_handling_strategy(hotspot, strategy);
                if (result.is_ok()) {
                    hotspot.applied_strategies.push_back(strategy);
                    
                    HandlingResult handling_result;
                    handling_result.key = key;
                    handling_result.strategy = strategy;
                    handling_result.applied_at = std::chrono::system_clock::now();
                    handling_result.success = true;
                    
                    handling_results_[key].push_back(handling_result);
                    notify_hotspot_handled(key, strategy);
                    
                    break; // 一次只应用一个策略
                }
            }
        }
    }
}

void DefaultHotspotManager::cleanup_expired_data() {
    auto now = std::chrono::system_clock::now();
    
    // 清理过期的key统计
    auto key_it = key_stats_.begin();
    while (key_it != key_stats_.end()) {
        auto inactive_duration = now - key_it->second.last_access;
        if (inactive_duration > config_.stats_retention_time) {
            key_it = key_stats_.erase(key_it);
        } else {
            ++key_it;
        }
    }
    
    // 清理过期的热点信息
    auto hotspot_it = detected_hotspots_.begin();
    while (hotspot_it != detected_hotspots_.end()) {
        if (!hotspot_it->second.is_active) {
            auto inactive_duration = now - hotspot_it->second.last_updated;
            if (inactive_duration > config_.hotspot_retention_time) {
                hotspot_it = detected_hotspots_.erase(hotspot_it);
                continue;
            }
        }
        ++hotspot_it;
    }
    
    // 清理处理结果历史
    auto result_it = handling_results_.begin();
    while (result_it != handling_results_.end()) {
        auto& results = result_it->second;
        results.erase(std::remove_if(results.begin(), results.end(),
            [now, this](const HandlingResult& result) {
                auto age = now - result.applied_at;
                return age > config_.result_retention_time;
            }), results.end());
        
        if (results.empty()) {
            result_it = handling_results_.erase(result_it);
        } else {
            ++result_it;
        }
    }
}

void DefaultHotspotManager::update_time_window_stats(const MultiLevelKey& key, const AccessInfo& access_info) {
    auto& window_stats = time_window_stats_[key];
    auto now = std::chrono::system_clock::now();
    
    // 更新时间窗口
    auto window_duration = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()) / config_.time_window_size.count();
    size_t window_index = window_duration % config_.max_time_windows;
    
    window_stats[window_index].window_start = now;
    window_stats[window_index].access_count++;
    window_stats[window_index].total_size += access_info.size;
}

void DefaultHotspotManager::update_access_pattern(const MultiLevelKey& key, const AccessInfo& access_info) {
    auto& pattern = access_patterns_[key];
    
    // 更新访问间隔
    if (!pattern.access_times.empty()) {
        auto last_access = pattern.access_times.back();
        auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            access_info.timestamp - last_access);
        pattern.access_intervals.push_back(interval);
        
        // 保持最近的访问间隔记录
        if (pattern.access_intervals.size() > config_.max_pattern_history) {
            pattern.access_intervals.erase(pattern.access_intervals.begin());
        }
    }
    
    pattern.access_times.push_back(access_info.timestamp);
    
    // 保持最近的访问时间记录
    if (pattern.access_times.size() > config_.max_pattern_history) {
        pattern.access_times.erase(pattern.access_times.begin());
    }
}

void DefaultHotspotManager::check_instant_hotspot(const MultiLevelKey& key, const KeyStats& stats) {
    // 实时热点检测：短时间内大量访问
    auto now = std::chrono::system_clock::now();
    auto recent_accesses = count_recent_accesses(key, config_.instant_detection_window);
    
    if (recent_accesses > config_.instant_hotspot_threshold) {
        auto it = detected_hotspots_.find(key);
        if (it == detected_hotspots_.end()) {
            HotspotInfo hotspot;
            hotspot.key = key;
            hotspot.detected_at = now;
            hotspot.last_updated = now;
            hotspot.is_active = true;
            hotspot.heat_score = calculate_heat_score(stats);
            hotspot.hotspot_type = HotspotType::INSTANT;
            
            detected_hotspots_[key] = hotspot;
            notify_hotspot_detected(key, hotspot);
        }
    }
}

double DefaultHotspotManager::calculate_global_average_frequency() const {
    if (key_stats_.empty()) {
        return 0.0;
    }
    
    double total_frequency = 0.0;
    for (const auto& [key, stats] : key_stats_) {
        total_frequency += stats.access_count;
    }
    
    return total_frequency / key_stats_.size();
}

bool DefaultHotspotManager::detect_frequency_hotspot(const KeyStats& stats, double global_avg) const {
    return stats.access_count > global_avg * config_.frequency_threshold_multiplier;
}

bool DefaultHotspotManager::detect_geographic_hotspot(const KeyStats& stats) const {
    if (stats.location_counts.empty()) {
        return false;
    }
    
    // 检查是否某个地理位置的访问过于集中
    size_t max_location_count = 0;
    size_t total_accesses = 0;
    
    for (const auto& [location, count] : stats.location_counts) {
        max_location_count = std::max(max_location_count, count);
        total_accesses += count;
    }
    
    if (total_accesses == 0) {
        return false;
    }
    
    double concentration_ratio = static_cast<double>(max_location_count) / total_accesses;
    return concentration_ratio > config_.geo_concentration_threshold;
}

bool DefaultHotspotManager::detect_pattern_hotspot(const KeyStats& stats) const {
    auto it = access_patterns_.find(stats.key);
    if (it == access_patterns_.end() || it->second.access_intervals.empty()) {
        return false;
    }
    
    const auto& pattern = it->second;
    
    // 检查访问间隔的标准差
    double mean_interval = 0.0;
    for (const auto& interval : pattern.access_intervals) {
        mean_interval += interval.count();
    }
    mean_interval /= pattern.access_intervals.size();
    
    double variance = 0.0;
    for (const auto& interval : pattern.access_intervals) {
        double diff = interval.count() - mean_interval;
        variance += diff * diff;
    }
    variance /= pattern.access_intervals.size();
    double stddev = std::sqrt(variance);
    
    // 如果访问间隔很规律（标准差小），可能是定期访问的热点
    double cv = stddev / mean_interval; // 变异系数
    return cv < config_.pattern_regularity_threshold;
}

bool DefaultHotspotManager::detect_temporal_hotspot(const KeyStats& stats) const {
    auto it = time_window_stats_.find(stats.key);
    if (it == time_window_stats_.end()) {
        return false;
    }
    
    const auto& windows = it->second;
    
    // 计算时间窗口访问的集中度
    size_t max_window_accesses = 0;
    size_t total_accesses = 0;
    
    for (const auto& [window_idx, window_stat] : windows) {
        max_window_accesses = std::max(max_window_accesses, window_stat.access_count);
        total_accesses += window_stat.access_count;
    }
    
    if (total_accesses == 0) {
        return false;
    }
    
    double temporal_concentration = static_cast<double>(max_window_accesses) / total_accesses;
    return temporal_concentration > config_.temporal_concentration_threshold;
}

double DefaultHotspotManager::calculate_heat_score(const KeyStats& stats) const {
    double frequency_score = static_cast<double>(stats.access_count) / total_requests_;
    
    // 时效性得分
    auto now = std::chrono::system_clock::now();
    auto time_since_last_access = now - stats.last_access;
    double recency_score = 1.0 / (1.0 + std::chrono::duration_cast<std::chrono::minutes>(
        time_since_last_access).count());
    
    // 地理集中度得分
    double geo_score = 0.0;
    if (!stats.location_counts.empty()) {
        size_t max_location_count = 0;
        size_t total_accesses = 0;
        for (const auto& [location, count] : stats.location_counts) {
            max_location_count = std::max(max_location_count, count);
            total_accesses += count;
        }
        if (total_accesses > 0) {
            geo_score = static_cast<double>(max_location_count) / total_accesses;
        }
    }
    
    // 综合得分
    return frequency_score * 0.5 + recency_score * 0.3 + geo_score * 0.2;
}

HotspotType DefaultHotspotManager::determine_hotspot_type(bool frequency, bool geo, 
                                                         bool pattern, bool temporal) const {
    if (frequency && geo) return HotspotType::FREQUENCY_GEO;
    if (frequency && pattern) return HotspotType::FREQUENCY_PATTERN;
    if (frequency && temporal) return HotspotType::FREQUENCY_TEMPORAL;
    if (frequency) return HotspotType::FREQUENCY;
    if (geo) return HotspotType::GEOGRAPHIC;
    if (pattern) return HotspotType::PATTERN;
    if (temporal) return HotspotType::TEMPORAL;
    return HotspotType::UNKNOWN;
}

std::vector<HandlingStrategy> DefaultHotspotManager::analyze_optimal_strategies(const HotspotInfo& hotspot) const {
    std::vector<HandlingStrategy> strategies;
    
    auto stats_it = key_stats_.find(hotspot.key);
    if (stats_it == key_stats_.end()) {
        return strategies;
    }
    
    const auto& stats = stats_it->second;
    
    // 基于热点类型和访问模式推荐策略
    switch (hotspot.hotspot_type) {
        case HotspotType::FREQUENCY:
        case HotspotType::FREQUENCY_GEO:
        case HotspotType::FREQUENCY_PATTERN:
        case HotspotType::FREQUENCY_TEMPORAL:
            // 高频访问热点，优先复制和负载均衡
            strategies.push_back(HandlingStrategy::REPLICATION);
            strategies.push_back(HandlingStrategy::LOAD_BALANCING);
            strategies.push_back(HandlingStrategy::CACHING);
            break;
            
        case HotspotType::GEOGRAPHIC:
            // 地理集中热点，优先分区和负载均衡
            strategies.push_back(HandlingStrategy::PARTITIONING);
            strategies.push_back(HandlingStrategy::LOAD_BALANCING);
            break;
            
        case HotspotType::PATTERN:
            // 模式化热点，可以预测和缓存
            strategies.push_back(HandlingStrategy::CACHING);
            strategies.push_back(HandlingStrategy::REPLICATION);
            break;
            
        case HotspotType::TEMPORAL:
            // 时间集中热点，优先限流和负载均衡
            strategies.push_back(HandlingStrategy::RATE_LIMITING);
            strategies.push_back(HandlingStrategy::LOAD_BALANCING);
            break;
            
        case HotspotType::INSTANT:
            // 瞬时热点，立即限流
            strategies.push_back(HandlingStrategy::RATE_LIMITING);
            strategies.push_back(HandlingStrategy::CACHING);
            break;
            
        default:
            // 通用策略
            strategies.push_back(HandlingStrategy::CACHING);
            strategies.push_back(HandlingStrategy::LOAD_BALANCING);
            break;
    }
    
    // 排序策略（基于效果和成本）
    std::sort(strategies.begin(), strategies.end(), [this](HandlingStrategy a, HandlingStrategy b) {
        return get_strategy_priority(a) > get_strategy_priority(b);
    });
    
    return strategies;
}

Result<void> DefaultHotspotManager::apply_handling_strategy(HotspotInfo& hotspot, HandlingStrategy strategy) {
    // 这里是策略应用的简化实现
    // 实际应用中需要与存储引擎、负载均衡器等组件交互
    
    switch (strategy) {
        case HandlingStrategy::REPLICATION:
            return apply_replication_strategy(hotspot);
        case HandlingStrategy::PARTITIONING:
            return apply_partitioning_strategy(hotspot);
        case HandlingStrategy::CACHING:
            return apply_caching_strategy(hotspot);
        case HandlingStrategy::LOAD_BALANCING:
            return apply_load_balancing_strategy(hotspot);
        case HandlingStrategy::RATE_LIMITING:
            return apply_rate_limiting_strategy(hotspot);
        default:
            return Result<void>::error(Status::NOT_IMPLEMENTED, "Strategy not implemented: " + std::to_string(static_cast<int>(strategy)));
    }
}

Result<void> DefaultHotspotManager::apply_replication_strategy(HotspotInfo& hotspot) {
    // 复制策略：增加数据副本数量
    size_t current_replicas = get_current_replica_count(hotspot.key);
    size_t target_replicas = std::min(current_replicas + config_.replication_increment, 
                                     config_.max_replicas);
    
    if (target_replicas > current_replicas) {
        // 触发副本增加
        // 这里需要与存储引擎交互
        hotspot.current_replicas = target_replicas;
        return Result<void>(Status::OK);
    }
    
    return Result<void>::error(Status::NO_CHANGE, "Already at optimal replica count");
}

Result<void> DefaultHotspotManager::apply_partitioning_strategy(HotspotInfo& hotspot) {
    // 分区策略：重新分区以分散负载
    auto stats_it = key_stats_.find(hotspot.key);
    if (stats_it == key_stats_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Key stats not found");
    }
    
    const auto& stats = stats_it->second;
    
    // 基于地理分布重新分区
    if (!stats.location_counts.empty()) {
        // 这里需要与分区管理器交互
        return Result<void>(Status::OK);
    }
    
    return Result<void>::error(Status::INVALID_ARGUMENT, "No geographic distribution data for partitioning");
}

Result<void> DefaultHotspotManager::apply_caching_strategy(HotspotInfo& hotspot) {
    // 缓存策略：增加缓存层或优化缓存配置
    
    // 设置更高的缓存优先级
    hotspot.cache_priority = CachePriority::HIGH;
    
    // 延长缓存TTL
    hotspot.cache_ttl = config_.hotspot_cache_ttl;
    
    return Result<void>(Status::OK);
}

Result<void> DefaultHotspotManager::apply_load_balancing_strategy(HotspotInfo& hotspot) {
    // 负载均衡策略：调整负载分布
    
    // 增加权重分散
    hotspot.load_balancing_weight = config_.hotspot_lb_weight;
    
    // 这里需要与负载均衡器交互，调整路由策略
    
    return Result<void>(Status::OK);
}

Result<void> DefaultHotspotManager::apply_rate_limiting_strategy(HotspotInfo& hotspot) {
    // 限流策略：对热点key应用限流
    
    auto stats_it = key_stats_.find(hotspot.key);
    if (stats_it == key_stats_.end()) {
        return Result<void>::error(Status::NOT_FOUND, "Key stats not found");
    }
    
    const auto& stats = stats_it->second;
    
    // 计算限流阈值
    double rate_limit = stats.access_count / 60.0 * config_.rate_limit_factor; // 每秒请求数
    hotspot.rate_limit = std::max(rate_limit, config_.min_rate_limit);
    
    return Result<void>(Status::OK);
}

size_t DefaultHotspotManager::count_recent_accesses(const MultiLevelKey& key, std::chrono::milliseconds window) const {
    auto it = access_patterns_.find(key);
    if (it == access_patterns_.end()) {
        return 0;
    }
    
    const auto& pattern = it->second;
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - window;
    
    size_t count = 0;
    for (auto access_time : pattern.access_times) {
        if (access_time >= cutoff) {
            count++;
        }
    }
    
    return count;
}

size_t DefaultHotspotManager::get_current_replica_count(const MultiLevelKey& key) const {
    // 简化实现，返回默认值
    // 实际实现需要查询存储引擎
    return config_.default_replicas;
}

int DefaultHotspotManager::get_strategy_priority(HandlingStrategy strategy) const {
    // 策略优先级评分
    switch (strategy) {
        case HandlingStrategy::RATE_LIMITING: return 100; // 最高优先级，立即生效
        case HandlingStrategy::CACHING: return 90;
        case HandlingStrategy::LOAD_BALANCING: return 80;
        case HandlingStrategy::REPLICATION: return 70;
        case HandlingStrategy::PARTITIONING: return 60; // 最低优先级，成本最高
        default: return 0;
    }
}

void DefaultHotspotManager::notify_hotspot_detected(const MultiLevelKey& key, const HotspotInfo& hotspot) {
    if (event_handler_) {
        event_handler_->on_hotspot_detected(key, hotspot);
    }
}

void DefaultHotspotManager::notify_hotspot_handled(const MultiLevelKey& key, HandlingStrategy strategy) {
    if (event_handler_) {
        event_handler_->on_hotspot_handled(key, strategy);
    }
}

void DefaultHotspotManager::notify_hotspot_cooled_down(const MultiLevelKey& key) {
    if (event_handler_) {
        event_handler_->on_hotspot_cooled_down(key);
    }
}

// HotspotPredictor 实现

HotspotPredictor::HotspotPredictor(std::shared_ptr<HotspotManager> manager) 
    : manager_(manager), prediction_accuracy_(0.0) {
}

std::vector<HotspotPrediction> HotspotPredictor::predict_hotspots(std::chrono::seconds horizon) const {
    std::vector<HotspotPrediction> predictions;
    
    if (!manager_) {
        return predictions;
    }
    
    // 获取当前热点和统计信息
    auto current_hotspots = manager_->get_hotspots();
    auto top_keys = manager_->get_top_keys(100);
    
    auto now = std::chrono::system_clock::now();
    auto prediction_time = now + horizon;
    
    // 基于趋势分析预测
    for (const auto& key_stats : top_keys) {
        HotspotPrediction prediction;
        prediction.key = key_stats.key;
        prediction.predicted_at = now;
        prediction.prediction_time = prediction_time;
        
        // 计算访问趋势
        double trend_score = calculate_trend_score(key_stats);
        
        // 计算周期性得分
        double periodicity_score = calculate_periodicity_score(key_stats);
        
        // 综合预测概率
        prediction.probability = std::min(1.0, (trend_score + periodicity_score) / 2.0);
        prediction.confidence = calculate_confidence(key_stats);
        
        if (prediction.probability > 0.5) { // 阈值可配置
            predictions.push_back(prediction);
        }
    }
    
    // 按概率排序
    std::sort(predictions.begin(), predictions.end(), 
        [](const HotspotPrediction& a, const HotspotPrediction& b) {
            return a.probability > b.probability;
        });
    
    return predictions;
}

double HotspotPredictor::get_prediction_accuracy() const {
    return prediction_accuracy_;
}

void HotspotPredictor::update_accuracy(const std::vector<HotspotPrediction>& predictions,
                                      const std::vector<MultiLevelKey>& actual_hotspots) {
    if (predictions.empty()) {
        return;
    }
    
    size_t correct_predictions = 0;
    
    for (const auto& prediction : predictions) {
        bool found = std::find(actual_hotspots.begin(), actual_hotspots.end(), 
                              prediction.key) != actual_hotspots.end();
        if (found) {
            correct_predictions++;
        }
    }
    
    double new_accuracy = static_cast<double>(correct_predictions) / predictions.size();
    
    // 使用指数移动平均更新精度
    const double alpha = 0.1;
    prediction_accuracy_ = (1.0 - alpha) * prediction_accuracy_ + alpha * new_accuracy;
}

double HotspotPredictor::calculate_trend_score(const KeyStats& stats) const {
    // 简化的趋势分析
    // 实际实现可以使用更复杂的时间序列分析
    
    auto now = std::chrono::system_clock::now();
    auto recent_window = now - std::chrono::hours(1);
    
    // 假设有按时间窗口的访问统计
    // 这里简化为基于最近访问时间的得分
    auto time_since_last_access = now - stats.last_access;
    auto hours_since_access = std::chrono::duration_cast<std::chrono::hours>(time_since_last_access).count();
    
    if (hours_since_access <= 1) {
        return 0.9; // 最近有访问，趋势良好
    } else if (hours_since_access <= 6) {
        return 0.6; // 中等趋势
    } else {
        return 0.2; // 趋势较弱
    }
}

double HotspotPredictor::calculate_periodicity_score(const KeyStats& stats) const {
    // 简化的周期性分析
    // 实际实现需要FFT或自相关分析
    
    // 基于操作类型分布判断周期性
    if (stats.operation_counts.empty()) {
        return 0.0;
    }
    
    size_t read_count = 0;
    size_t total_ops = 0;
    
    for (const auto& [op, count] : stats.operation_counts) {
        total_ops += count;
        if (op == OperationType::read) {
            read_count += count;
        }
    }
    
    if (total_ops == 0) {
        return 0.0;
    }
    
    double read_ratio = static_cast<double>(read_count) / total_ops;
    
    // 读多写少的key通常有更好的周期性
    return read_ratio;
}

double HotspotPredictor::calculate_confidence(const KeyStats& stats) const {
    // 基于数据质量计算置信度
    double data_quality = std::min(1.0, static_cast<double>(stats.access_count) / 100.0);
    
    auto now = std::chrono::system_clock::now();
    auto time_since_last_access = now - stats.last_access;
    double recency_factor = 1.0 / (1.0 + std::chrono::duration_cast<std::chrono::hours>(
        time_since_last_access).count());
    
    return data_quality * recency_factor;
}

// 工具函数实现
namespace hotspot_utils {

std::string hotspot_type_to_string(HotspotType type) {
    switch (type) {
        case HotspotType::FREQUENCY: return "Frequency";
        case HotspotType::GEOGRAPHIC: return "Geographic";
        case HotspotType::PATTERN: return "Pattern";
        case HotspotType::TEMPORAL: return "Temporal";
        case HotspotType::INSTANT: return "Instant";
        case HotspotType::FREQUENCY_GEO: return "Frequency+Geographic";
        case HotspotType::FREQUENCY_PATTERN: return "Frequency+Pattern";
        case HotspotType::FREQUENCY_TEMPORAL: return "Frequency+Temporal";
        default: return "Unknown";
    }
}

std::string handling_strategy_to_string(HandlingStrategy strategy) {
    switch (strategy) {
        case HandlingStrategy::REPLICATION: return "Replication";
        case HandlingStrategy::PARTITIONING: return "Partitioning";
        case HandlingStrategy::CACHING: return "Caching";
        case HandlingStrategy::LOAD_BALANCING: return "LoadBalancing";
        case HandlingStrategy::RATE_LIMITING: return "RateLimiting";
        default: return "Unknown";
    }
}

double calculate_hotspot_intensity(const KeyStats& stats, size_t total_requests) {
    if (total_requests == 0) {
        return 0.0;
    }
    
    double frequency_ratio = static_cast<double>(stats.access_count) / total_requests;
    
    // 地理集中度
    double geo_concentration = 0.0;
    if (!stats.location_counts.empty()) {
        size_t max_location_count = 0;
        size_t total_accesses = 0;
        for (const auto& [location, count] : stats.location_counts) {
            max_location_count = std::max(max_location_count, count);
            total_accesses += count;
        }
        if (total_accesses > 0) {
            geo_concentration = static_cast<double>(max_location_count) / total_accesses;
        }
    }
    
    // 综合强度
    return frequency_ratio * 0.7 + geo_concentration * 0.3;
}

std::vector<MultiLevelKey> find_correlated_hotspots(const std::vector<HotspotInfo>& hotspots,
                                                   double correlation_threshold) {
    std::vector<MultiLevelKey> correlated;
    
    // 简化的相关性分析
    // 实际实现需要更复杂的统计分析
    
    for (size_t i = 0; i < hotspots.size(); i++) {
        for (size_t j = i + 1; j < hotspots.size(); j++) {
            const auto& hotspot1 = hotspots[i];
            const auto& hotspot2 = hotspots[j];
            
            // 检查热点类型相似性
            if (hotspot1.hotspot_type == hotspot2.hotspot_type) {
                // 检查热度得分相似性
                double score_diff = std::abs(hotspot1.heat_score - hotspot2.heat_score);
                if (score_diff < correlation_threshold) {
                    correlated.push_back(hotspot1.key);
                    correlated.push_back(hotspot2.key);
                }
            }
        }
    }
    
    // 去重
    std::sort(correlated.begin(), correlated.end());
    correlated.erase(std::unique(correlated.begin(), correlated.end()), correlated.end());
    
    return correlated;
}

HotspotConfig create_default_hotspot_config() {
    HotspotConfig config;
    
    // 检测配置
    config.detection_interval = std::chrono::seconds(30);
    config.processing_interval = std::chrono::seconds(60);
    config.cleanup_interval = std::chrono::minutes(10);
    
    // 阈值配置
    config.frequency_threshold_multiplier = 2.0;
    config.geo_concentration_threshold = 0.7;
    config.pattern_regularity_threshold = 0.3;
    config.temporal_concentration_threshold = 0.6;
    config.instant_hotspot_threshold = 100;
    
    // 时间窗口配置
    config.time_window_size = std::chrono::minutes(5);
    config.max_time_windows = 12; // 1小时
    config.instant_detection_window = std::chrono::seconds(30);
    
    // 处理配置
    config.enable_auto_handling = true;
    config.enable_realtime_detection = true;
    config.hotspot_cool_down_time = std::chrono::minutes(30);
    
    // 策略配置
    config.replication_increment = 2;
    config.max_replicas = 5;
    config.default_replicas = 1;
    config.hotspot_cache_ttl = std::chrono::hours(1);
    config.hotspot_lb_weight = 0.5;
    config.rate_limit_factor = 0.8;
    config.min_rate_limit = 10.0;
    
    // 保留时间配置
    config.stats_retention_time = std::chrono::hours(24);
    config.hotspot_retention_time = std::chrono::hours(6);
    config.result_retention_time = std::chrono::hours(12);
    
    // 模式分析配置
    config.max_pattern_history = 100;
    
    return config;
}

} // namespace hotspot_utils

} // namespace hotspot
} // namespace cache