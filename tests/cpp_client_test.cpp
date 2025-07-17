#include "cache/network/cache_service.h"
#include "cache/common/types.h"
#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>
#include <random>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <future>
#include <atomic>
#include <iomanip>

using namespace cache;
using namespace cache::network;

struct TestConfig {
    std::vector<std::string> servers;
    std::string load_balancer;
    int timeout_ms = 30000;
    int retry_count = 3;
    
    // 测试配置
    int test_duration_seconds = 60;
    int concurrent_clients = 10;
    int data_size_bytes = 1024;
    std::string key_prefix = "test";
    
    // 压力测试配置
    int stress_test_ops = 10000;
    int stress_test_threads = 50;
    
    // 数据限制
    int max_key_size = 256;
    int max_value_size = 4096;
    
    static TestConfig load_from_file(const std::string& filename) {
        TestConfig config;
        try {
            YAML::Node yaml = YAML::LoadFile(filename);
            
            if (yaml["servers"]) {
                for (const auto& server : yaml["servers"]) {
                    config.servers.push_back(server.as<std::string>());
                }
            }
            
            if (yaml["load_balancer"]) {
                config.load_balancer = yaml["load_balancer"].as<std::string>();
            }
            
            if (yaml["timeout"]) config.timeout_ms = yaml["timeout"].as<int>() * 1000;
            if (yaml["retry_count"]) config.retry_count = yaml["retry_count"].as<int>();
            if (yaml["test_duration_seconds"]) config.test_duration_seconds = yaml["test_duration_seconds"].as<int>();
            if (yaml["concurrent_clients"]) config.concurrent_clients = yaml["concurrent_clients"].as<int>();
            if (yaml["data_size_bytes"]) config.data_size_bytes = yaml["data_size_bytes"].as<int>();
            if (yaml["key_prefix"]) config.key_prefix = yaml["key_prefix"].as<std::string>();
            if (yaml["stress_test_ops"]) config.stress_test_ops = yaml["stress_test_ops"].as<int>();
            if (yaml["stress_test_threads"]) config.stress_test_threads = yaml["stress_test_threads"].as<int>();
            if (yaml["max_key_size"]) config.max_key_size = yaml["max_key_size"].as<int>();
            if (yaml["max_value_size"]) config.max_value_size = yaml["max_value_size"].as<int>();
            
        } catch (const std::exception& e) {
            std::cerr << "Error loading config: " << e.what() << std::endl;
        }
        
        return config;
    }
};

struct TestResult {
    std::string test_name;
    bool success;
    double duration_ms;
    std::string error_message;
    std::map<std::string, std::string> metrics;
    
    TestResult(const std::string& name, bool success, double duration, const std::string& error = "")
        : test_name(name), success(success), duration_ms(duration), error_message(error) {}
};

struct StressTestMetrics {
    std::atomic<uint64_t> total_operations{0};
    std::atomic<uint64_t> successful_operations{0};
    std::atomic<uint64_t> failed_operations{0};
    std::vector<double> latencies_ms;
    std::mutex latencies_mutex;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    
    void add_latency(double latency_ms) {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        latencies_ms.push_back(latency_ms);
    }
    
    double get_success_rate() const {
        uint64_t total = total_operations.load();
        if (total == 0) return 0.0;
        return static_cast<double>(successful_operations.load()) / total;
    }
    
    double get_qps() const {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        if (duration.count() == 0) return 0.0;
        return static_cast<double>(total_operations.load()) * 1000.0 / duration.count();
    }
    
    double get_average_latency() const {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        if (latencies_ms.empty()) return 0.0;
        double sum = 0.0;
        for (double latency : latencies_ms) {
            sum += latency;
        }
        return sum / latencies_ms.size();
    }
    
    double get_percentile_latency(double percentile) const {
        std::lock_guard<std::mutex> lock(latencies_mutex);
        if (latencies_ms.empty()) return 0.0;
        
        auto sorted_latencies = latencies_ms;
        std::sort(sorted_latencies.begin(), sorted_latencies.end());
        
        size_t index = static_cast<size_t>(percentile * sorted_latencies.size());
        if (index >= sorted_latencies.size()) index = sorted_latencies.size() - 1;
        
        return sorted_latencies[index];
    }
};

class CacheTestClient {
private:
    std::shared_ptr<CacheClient> client_;
    TestConfig config_;
    std::mt19937 rng_;
    
public:
    CacheTestClient(const TestConfig& config) 
        : config_(config), rng_(std::random_device{}()) {
        // 这里需要根据实际的CacheClient实现来创建客户端
        // client_ = std::make_shared<CacheClientImpl>();
    }
    
    std::string generate_random_string(size_t length) {
        const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);
        
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dis(rng_)];
        }
        return result;
    }
    
    std::string generate_test_key(const std::string& prefix, int id) {
        return prefix + "_" + std::to_string(id) + "_" + generate_random_string(8);
    }
    
    DataEntry generate_test_data(size_t size) {
        DataEntry data;
        data.data = generate_random_string(size);
        data.timestamp = std::chrono::system_clock::now();
        data.version = 1;
        return data;
    }
    
    TestResult test_basic_operations() {
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            // 连接到负载均衡器
            auto connect_result = client_->connect(config_.load_balancer, 8080);
            if (!connect_result.is_ok()) {
                return TestResult("basic_operations", false, 0.0, 
                    "Failed to connect: " + connect_result.get_error().message);
            }
            
            // 生成测试数据
            MultiLevelKey test_key;
            test_key.parts = {config_.key_prefix, "basic", generate_random_string(8)};
            
            DataEntry test_data = generate_test_data(config_.data_size_bytes);
            
            // 1. PUT操作
            auto put_response = client_->put(test_key, test_data);
            if (put_response.status != Status::OK) {
                return TestResult("basic_operations", false, 0.0, 
                    "PUT failed: " + put_response.error_message);
            }
            
            // 2. GET操作
            auto get_response = client_->get(test_key);
            if (get_response.status != Status::OK) {
                return TestResult("basic_operations", false, 0.0, 
                    "GET failed: " + get_response.error_message);
            }
            
            if (!get_response.found) {
                return TestResult("basic_operations", false, 0.0, "Data not found after PUT");
            }
            
            if (get_response.data.data != test_data.data) {
                return TestResult("basic_operations", false, 0.0, "Data mismatch");
            }
            
            // 3. DELETE操作
            auto delete_response = client_->remove(test_key);
            if (delete_response.status != Status::OK) {
                return TestResult("basic_operations", false, 0.0, 
                    "DELETE failed: " + delete_response.error_message);
            }
            
            // 4. 验证删除
            auto verify_response = client_->get(test_key);
            if (verify_response.status != Status::OK) {
                return TestResult("basic_operations", false, 0.0, 
                    "GET after DELETE failed: " + verify_response.error_message);
            }
            
            if (verify_response.found) {
                return TestResult("basic_operations", false, 0.0, "Key still exists after DELETE");
            }
            
            client_->disconnect();
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            return TestResult("basic_operations", true, duration.count());
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            return TestResult("basic_operations", false, duration.count(), e.what());
        }
    }
    
    TestResult test_batch_operations() {
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            auto connect_result = client_->connect(config_.load_balancer, 8080);
            if (!connect_result.is_ok()) {
                return TestResult("batch_operations", false, 0.0, 
                    "Failed to connect: " + connect_result.get_error().message);
            }
            
            // 准备批量数据
            std::vector<std::pair<MultiLevelKey, DataEntry>> entries;
            std::vector<MultiLevelKey> keys;
            
            for (int i = 0; i < 10; ++i) {
                MultiLevelKey key;
                key.parts = {config_.key_prefix, "batch", std::to_string(i), generate_random_string(8)};
                DataEntry data = generate_test_data(config_.data_size_bytes);
                
                entries.emplace_back(key, data);
                keys.push_back(key);
            }
            
            // 1. 批量PUT
            auto multi_put_response = client_->multi_put(entries, false);
            if (multi_put_response.status != Status::OK) {
                return TestResult("batch_operations", false, 0.0, 
                    "MULTI_PUT failed: " + multi_put_response.error_message);
            }
            
            // 2. 批量GET
            auto multi_get_response = client_->multi_get(keys);
            if (multi_get_response.status != Status::OK) {
                return TestResult("batch_operations", false, 0.0, 
                    "MULTI_GET failed: " + multi_get_response.error_message);
            }
            
            // 3. 验证数据
            if (multi_get_response.results.size() != entries.size()) {
                return TestResult("batch_operations", false, 0.0, "Batch result count mismatch");
            }
            
            for (size_t i = 0; i < entries.size(); ++i) {
                bool found = false;
                for (const auto& result : multi_get_response.results) {
                    if (result.first == entries[i].first) {
                        if (result.second.data != entries[i].second.data) {
                            return TestResult("batch_operations", false, 0.0, 
                                "Batch data mismatch for key " + std::to_string(i));
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return TestResult("batch_operations", false, 0.0, 
                        "Key not found in batch result: " + std::to_string(i));
                }
            }
            
            // 4. 清理
            for (const auto& key : keys) {
                client_->remove(key);
            }
            
            client_->disconnect();
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            TestResult result("batch_operations", true, duration.count());
            result.metrics["keys_tested"] = std::to_string(entries.size());
            return result;
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            return TestResult("batch_operations", false, duration.count(), e.what());
        }
    }
    
    TestResult test_large_data() {
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            auto connect_result = client_->connect(config_.load_balancer, 8080);
            if (!connect_result.is_ok()) {
                return TestResult("large_data", false, 0.0, 
                    "Failed to connect: " + connect_result.get_error().message);
            }
            
            std::vector<int> test_sizes = {1024, 4096, 8192, 16384}; // 1KB, 4KB, 8KB, 16KB
            std::map<int, std::pair<double, double>> latencies; // size -> (put_latency, get_latency)
            
            for (int size : test_sizes) {
                if (size > config_.max_value_size) continue;
                
                MultiLevelKey test_key;
                test_key.parts = {config_.key_prefix, "large", std::to_string(size), generate_random_string(8)};
                DataEntry test_data = generate_test_data(size);
                
                // PUT测试
                auto put_start = std::chrono::steady_clock::now();
                auto put_response = client_->put(test_key, test_data);
                auto put_end = std::chrono::steady_clock::now();
                auto put_duration = std::chrono::duration_cast<std::chrono::microseconds>(put_end - put_start);
                
                if (put_response.status != Status::OK) {
                    return TestResult("large_data", false, 0.0, 
                        "PUT failed for size " + std::to_string(size) + ": " + put_response.error_message);
                }
                
                // GET测试
                auto get_start = std::chrono::steady_clock::now();
                auto get_response = client_->get(test_key);
                auto get_end = std::chrono::steady_clock::now();
                auto get_duration = std::chrono::duration_cast<std::chrono::microseconds>(get_end - get_start);
                
                if (get_response.status != Status::OK) {
                    return TestResult("large_data", false, 0.0, 
                        "GET failed for size " + std::to_string(size) + ": " + get_response.error_message);
                }
                
                if (!get_response.found || get_response.data.data != test_data.data) {
                    return TestResult("large_data", false, 0.0, 
                        "Data corruption for size " + std::to_string(size));
                }
                
                latencies[size] = {put_duration.count() / 1000.0, get_duration.count() / 1000.0};
                
                // 清理
                client_->remove(test_key);
            }
            
            client_->disconnect();
            
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            TestResult result("large_data", true, duration.count());
            for (const auto& pair : latencies) {
                result.metrics["size_" + std::to_string(pair.first) + "_put_latency_ms"] = 
                    std::to_string(pair.second.first);
                result.metrics["size_" + std::to_string(pair.first) + "_get_latency_ms"] = 
                    std::to_string(pair.second.second);
            }
            return result;
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            return TestResult("large_data", false, duration.count(), e.what());
        }
    }
    
    void stress_test_worker(int worker_id, int operation_count, StressTestMetrics& metrics) {
        try {
            auto client = std::make_shared<CacheClient>(); // 需要根据实际实现创建
            auto connect_result = client->connect(config_.load_balancer, 8080);
            if (!connect_result.is_ok()) {
                std::cerr << "Worker " << worker_id << " failed to connect" << std::endl;
                return;
            }
            
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> op_dis(0, 2); // 0=PUT, 1=GET, 2=DELETE
            std::uniform_int_distribution<> size_dis(100, config_.data_size_bytes);
            
            for (int i = 0; i < operation_count; ++i) {
                MultiLevelKey key;
                key.parts = {config_.key_prefix, "stress", std::to_string(worker_id), std::to_string(i)};
                
                auto start_time = std::chrono::steady_clock::now();
                bool success = false;
                
                int operation = op_dis(gen);
                try {
                    if (operation == 0) { // PUT
                        DataEntry data = generate_test_data(size_dis(gen));
                        auto response = client->put(key, data);
                        success = (response.status == Status::OK);
                    } else if (operation == 1) { // GET
                        auto response = client->get(key);
                        success = (response.status == Status::OK);
                    } else { // DELETE
                        auto response = client->remove(key);
                        success = (response.status == Status::OK);
                    }
                } catch (const std::exception& e) {
                    success = false;
                }
                
                auto end_time = std::chrono::steady_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                
                metrics.add_latency(latency.count() / 1000.0);
                metrics.total_operations++;
                
                if (success) {
                    metrics.successful_operations++;
                } else {
                    metrics.failed_operations++;
                }
            }
            
            client->disconnect();
            
        } catch (const std::exception& e) {
            std::cerr << "Stress test worker " << worker_id << " error: " << e.what() << std::endl;
        }
    }
    
    TestResult stress_test() {
        auto start_time = std::chrono::steady_clock::now();
        
        try {
            StressTestMetrics metrics;
            metrics.start_time = start_time;
            
            std::vector<std::thread> workers;
            int ops_per_worker = config_.stress_test_ops / config_.stress_test_threads;
            
            std::cout << "Starting stress test: " << config_.stress_test_ops 
                      << " operations with " << config_.stress_test_threads << " threads" << std::endl;
            
            // 启动工作线程
            for (int i = 0; i < config_.stress_test_threads; ++i) {
                workers.emplace_back(&CacheTestClient::stress_test_worker, this, i, ops_per_worker, std::ref(metrics));
            }
            
            // 等待所有线程完成
            for (auto& worker : workers) {
                worker.join();
            }
            
            metrics.end_time = std::chrono::steady_clock::now();
            
            TestResult result("stress_test", metrics.get_success_rate() >= 0.95, 
                std::chrono::duration_cast<std::chrono::milliseconds>(metrics.end_time - start_time).count());
            
            result.metrics["total_operations"] = std::to_string(metrics.total_operations.load());
            result.metrics["successful_operations"] = std::to_string(metrics.successful_operations.load());
            result.metrics["failed_operations"] = std::to_string(metrics.failed_operations.load());
            result.metrics["success_rate"] = std::to_string(metrics.get_success_rate());
            result.metrics["qps"] = std::to_string(metrics.get_qps());
            result.metrics["avg_latency_ms"] = std::to_string(metrics.get_average_latency());
            result.metrics["p99_latency_ms"] = std::to_string(metrics.get_percentile_latency(0.99));
            result.metrics["p999_latency_ms"] = std::to_string(metrics.get_percentile_latency(0.999));
            
            return result;
            
        } catch (const std::exception& e) {
            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            return TestResult("stress_test", false, duration.count(), e.what());
        }
    }
    
    std::vector<TestResult> run_all_tests() {
        std::vector<TestResult> results;
        
        std::cout << "=== 分布式缓存数据库测试套件 ===" << std::endl;
        
        // 基本操作测试
        std::cout << "运行基本操作测试..." << std::endl;
        auto basic_result = test_basic_operations();
        results.push_back(basic_result);
        std::cout << (basic_result.success ? "✅ 成功" : "❌ 失败") 
                  << " (" << basic_result.duration_ms << "ms)" << std::endl;
        if (!basic_result.success) {
            std::cout << "  错误: " << basic_result.error_message << std::endl;
        }
        
        // 批量操作测试
        std::cout << "运行批量操作测试..." << std::endl;
        auto batch_result = test_batch_operations();
        results.push_back(batch_result);
        std::cout << (batch_result.success ? "✅ 成功" : "❌ 失败") 
                  << " (" << batch_result.duration_ms << "ms)" << std::endl;
        if (!batch_result.success) {
            std::cout << "  错误: " << batch_result.error_message << std::endl;
        }
        
        // 大数据测试
        std::cout << "运行大数据测试..." << std::endl;
        auto large_data_result = test_large_data();
        results.push_back(large_data_result);
        std::cout << (large_data_result.success ? "✅ 成功" : "❌ 失败") 
                  << " (" << large_data_result.duration_ms << "ms)" << std::endl;
        if (!large_data_result.success) {
            std::cout << "  错误: " << large_data_result.error_message << std::endl;
        }
        
        // 压力测试
        std::cout << "运行压力测试..." << std::endl;
        auto stress_result = stress_test();
        results.push_back(stress_result);
        std::cout << (stress_result.success ? "✅ 成功" : "❌ 失败") 
                  << " (" << stress_result.duration_ms << "ms)" << std::endl;
        if (!stress_result.success) {
            std::cout << "  错误: " << stress_result.error_message << std::endl;
        }
        
        return results;
    }
    
    void generate_report(const std::vector<TestResult>& results, const std::string& output_file) {
        std::ofstream file(output_file);
        
        int successful_tests = 0;
        for (const auto& result : results) {
            if (result.success) successful_tests++;
        }
        
        double success_rate = static_cast<double>(successful_tests) / results.size();
        
        file << "# 分布式缓存数据库测试报告\n\n";
        file << "## 测试概要\n";
        file << "- 总测试数: " << results.size() << "\n";
        file << "- 成功测试数: " << successful_tests << "\n";
        file << "- 失败测试数: " << (results.size() - successful_tests) << "\n";
        file << "- 成功率: " << std::fixed << std::setprecision(2) << (success_rate * 100) << "%\n";
        file << "- 测试时间: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n\n";
        
        file << "## 测试详情\n\n";
        for (const auto& result : results) {
            file << "### " << result.test_name << "\n";
            file << "- 状态: " << (result.success ? "成功" : "失败") << "\n";
            file << "- 耗时: " << std::fixed << std::setprecision(2) << result.duration_ms << "ms\n";
            if (!result.success) {
                file << "- 错误: " << result.error_message << "\n";
            }
            if (!result.metrics.empty()) {
                file << "- 指标:\n";
                for (const auto& metric : result.metrics) {
                    file << "  - " << metric.first << ": " << metric.second << "\n";
                }
            }
            file << "\n";
        }
        
        file.close();
        
        std::cout << "\n" << "="*60 << std::endl;
        std::cout << "分布式缓存数据库测试报告" << std::endl;
        std::cout << "="*60 << std::endl;
        std::cout << "总测试数: " << results.size() << std::endl;
        std::cout << "成功: " << successful_tests << " | 失败: " << (results.size() - successful_tests) << std::endl;
        std::cout << "成功率: " << std::fixed << std::setprecision(2) << (success_rate * 100) << "%" << std::endl;
        std::cout << "="*60 << std::endl;
        std::cout << "测试报告已保存到: " << output_file << std::endl;
    }
};

int main(int argc, char* argv[]) {
    std::string config_file = "test_config.yaml";
    std::string output_file = "cpp_test_report.md";
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            config_file = argv[i + 1];
            i++;
        } else if (std::string(argv[i]) == "--output" && i + 1 < argc) {
            output_file = argv[i + 1];
            i++;
        }
    }
    
    try {
        // 加载配置
        TestConfig config;
        if (std::ifstream(config_file).good()) {
            config = TestConfig::load_from_file(config_file);
        } else {
            std::cerr << "配置文件不存在，使用默认配置" << std::endl;
            config.servers = {"http://localhost:8081", "http://localhost:8082", "http://localhost:8083"};
            config.load_balancer = "http://localhost:8080";
        }
        
        // 创建测试客户端
        CacheTestClient test_client(config);
        
        // 运行所有测试
        auto results = test_client.run_all_tests();
        
        // 生成报告
        test_client.generate_report(results, output_file);
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "测试执行失败: " << e.what() << std::endl;
        return 1;
    }
}