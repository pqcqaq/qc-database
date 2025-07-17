# 分布式缓存数据库测试客户端 Makefile

# 编译器和标志
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
INCLUDES = -I../include
LIBS = -lyaml-cpp -lpthread

# 源文件和目标
CPP_TEST_SRC = cpp_client_test.cpp
CPP_TEST_TARGET = cpp_client_test

# Python相关
PYTHON = python3
PIP = pip3

# 默认目标
.PHONY: all clean install-deps run-tests run-python-tests run-cpp-tests run-performance-tests

all: $(CPP_TEST_TARGET)

# 编译C++测试客户端
$(CPP_TEST_TARGET): $(CPP_TEST_SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

# 安装Python依赖
install-python-deps:
	$(PIP) install aiohttp pyyaml matplotlib pandas numpy psutil

# 安装C++依赖 (Ubuntu/Debian)
install-cpp-deps:
	sudo apt-get update
	sudo apt-get install -y libyaml-cpp-dev build-essential

# 安装所有依赖
install-deps: install-python-deps install-cpp-deps

# 运行所有测试
run-tests: run-python-tests run-performance-tests

# 运行Python基础测试
run-python-tests:
	@echo "运行Python基础功能测试..."
	$(PYTHON) client_tests.py --config test_config.yaml --test all

# 运行Python性能测试
run-performance-tests:
	@echo "运行性能测试..."
	$(PYTHON) performance_test.py --config test_config.yaml --test-type all

# 运行C++测试
run-cpp-tests: $(CPP_TEST_TARGET)
	@echo "运行C++客户端测试..."
	./$(CPP_TEST_TARGET) --config test_config.yaml

# 运行快速测试（仅基础功能，用于CI）
run-quick-tests:
	@echo "运行快速测试..."
	$(PYTHON) client_tests.py --config test_config.yaml --test basic

# 运行压力测试
run-stress-tests:
	@echo "运行压力测试..."
	$(PYTHON) client_tests.py --config test_config.yaml --test stress

# 运行集群健康检查
run-health-check:
	@echo "运行集群健康检查..."
	$(PYTHON) client_tests.py --config test_config.yaml --test cluster

# 生成测试报告
generate-reports: run-tests
	@echo "生成综合测试报告..."
	@echo "基础功能测试报告: cache_test_report_*.json"
	@echo "性能测试报告: performance_report.json"
	@echo "性能图表: ./charts/"

# 清理生成的文件
clean:
	rm -f $(CPP_TEST_TARGET)
	rm -f *.log
	rm -f cache_test_report_*.json
	rm -f performance_report.json
	rm -f cpp_test_report.md
	rm -rf ./charts/
	rm -rf ./test_reports/

# 检查数据库服务状态
check-services:
	@echo "检查数据库服务状态..."
	@curl -f http://localhost:8080/api/v1/ping 2>/dev/null && echo "✅ 负载均衡器正常" || echo "❌ 负载均衡器异常"
	@curl -f http://localhost:8081/api/v1/ping 2>/dev/null && echo "✅ 节点1正常" || echo "❌ 节点1异常"
	@curl -f http://localhost:8082/api/v1/ping 2>/dev/null && echo "✅ 节点2正常" || echo "❌ 节点2异常"
	@curl -f http://localhost:8083/api/v1/ping 2>/dev/null && echo "✅ 节点3正常" || echo "❌ 节点3异常"

# 设置测试环境
setup-test-env:
	@echo "设置测试环境..."
	mkdir -p ./test_reports
	mkdir -p ./charts
	@echo "测试环境设置完成"

# 运行完整的测试套件（包括性能测试）
run-full-test-suite: setup-test-env check-services
	@echo "开始运行完整测试套件..."
	@echo "1. 运行基础功能测试..."
	$(PYTHON) client_tests.py --config test_config.yaml --test all --output ./test_reports/basic_tests.json
	@echo "2. 运行性能测试..."
	$(PYTHON) performance_test.py --config test_config.yaml --output ./test_reports/performance_report.json --charts ./charts
	@echo "3. 运行C++客户端测试..."
	$(MAKE) run-cpp-tests || echo "C++测试需要先编译"
	@echo "完整测试套件运行完成!"
	@echo "查看报告: ./test_reports/"

# Docker环境测试
run-docker-tests:
	@echo "在Docker环境中运行测试..."
	docker-compose exec cache-node1 curl -f http://localhost:8081/api/v1/ping || echo "节点1检查失败"
	docker-compose exec cache-node2 curl -f http://localhost:8082/api/v1/ping || echo "节点2检查失败"
	docker-compose exec cache-node3 curl -f http://localhost:8083/api/v1/ping || echo "节点3检查失败"

# 监控系统性能
monitor-performance:
	@echo "监控系统性能 (按 Ctrl+C 停止)..."
	while true; do \
		echo "时间: $$(date)"; \
		echo "CPU: $$(top -bn1 | grep "Cpu(s)" | awk '{print $$2}' | cut -d'%' -f1)%"; \
		echo "内存: $$(free | grep Mem | awk '{printf("%.1f%%", $$3/$$2 * 100.0)}')"; \
		echo "---"; \
		sleep 5; \
	done

# 生成测试配置文件模板
generate-config-template:
	@echo "生成测试配置文件模板..."
	@cat > test_config_template.yaml << 'EOF'
# 分布式缓存数据库测试配置文件模板

# 服务器配置
servers:
  - "http://localhost:8081"  # cache-node1
  - "http://localhost:8082"  # cache-node2  
  - "http://localhost:8083"  # cache-node3

# 负载均衡器地址
load_balancer: "http://localhost:8080"

# 连接配置
timeout: 30
retry_count: 3

# 基本测试配置
test_duration_seconds: 60
concurrent_clients: 10
data_size_bytes: 1024
key_prefix: "test"

# 压力测试配置
stress_test_ops: 10000      # 总操作数
stress_test_threads: 50     # 并发线程数

# 数据限制配置
max_key_size: 256           # 最大键长度（字节）
max_value_size: 4096        # 最大值长度（字节）

# 性能测试配置
performance_test:
  warmup_seconds: 30
  test_duration_seconds: 300
  min_concurrent_users: 1
  max_concurrent_users: 100
  user_step: 10
  read_ratio: 0.7
  write_ratio: 0.2
  delete_ratio: 0.1
  target_p99_latency_ms: 100.0
  target_min_qps: 1000.0
EOF
	@echo "配置文件模板已生成: test_config_template.yaml"

# 帮助信息
help:
	@echo "分布式缓存数据库测试客户端 Makefile"
	@echo ""
	@echo "可用的目标:"
	@echo "  all                    - 编译所有测试程序"
	@echo "  install-deps          - 安装所有依赖"
	@echo "  install-python-deps   - 安装Python依赖"
	@echo "  install-cpp-deps      - 安装C++依赖"
	@echo "  run-tests             - 运行所有测试"
	@echo "  run-python-tests      - 运行Python基础测试"
	@echo "  run-cpp-tests         - 运行C++测试"
	@echo "  run-performance-tests - 运行性能测试"
	@echo "  run-quick-tests       - 运行快速测试"
	@echo "  run-stress-tests      - 运行压力测试"
	@echo "  run-health-check      - 运行集群健康检查"
	@echo "  run-full-test-suite   - 运行完整测试套件"
	@echo "  check-services        - 检查数据库服务状态"
	@echo "  setup-test-env        - 设置测试环境"
	@echo "  monitor-performance   - 监控系统性能"
	@echo "  generate-config-template - 生成配置文件模板"
	@echo "  clean                 - 清理生成的文件"
	@echo "  help                  - 显示此帮助信息"
	@echo ""
	@echo "使用示例:"
	@echo "  make install-deps     # 安装依赖"
	@echo "  make check-services   # 检查服务状态"
	@echo "  make run-tests        # 运行测试"
	@echo "  make run-full-test-suite # 运行完整测试套件"