# 分布式缓存数据库测试客户端

这是一个完整的测试套件，用于对分布式缓存数据库进行全面测试，包括基本功能测试、压力测试、性能基准测试等。

## 📁 文件结构

```
tests/
├── client_tests.py          # Python基础功能测试客户端
├── performance_test.py      # Python性能测试工具  
├── cpp_client_test.cpp      # C++测试客户端
├── test_config.yaml         # 测试配置文件
├── Makefile                 # 构建和运行脚本
└── README.md               # 本文档
```

## 🚀 快速开始

### 1. 环境准备

确保分布式缓存数据库服务已经启动：

```bash
# 在项目根目录下启动服务
docker-compose up -d

# 验证服务状态
make check-services
```

### 2. 安装依赖

```bash
# 安装所有依赖（Python + C++）
make install-deps

# 或者分别安装
make install-python-deps  # 安装Python依赖
make install-cpp-deps     # 安装C++依赖
```

### 3. 运行测试

```bash
# 运行完整测试套件
make run-full-test-suite

# 或者运行特定类型的测试
make run-quick-tests      # 快速基础功能测试
make run-stress-tests     # 压力测试
make run-performance-tests # 性能测试
```

## 🧪 测试类型

### 1. 基础功能测试 (`client_tests.py`)

测试数据库的核心功能：

- **基本操作测试**: PUT、GET、DELETE操作的正确性
- **批量操作测试**: 多键批量操作的一致性
- **集群健康检查**: 检查所有节点和负载均衡器状态
- **大数据测试**: 测试不同大小数据的处理能力
- **压力测试**: 高并发场景下的稳定性

#### 运行方法

```bash
# 运行所有基础测试
python3 client_tests.py --config test_config.yaml --test all

# 运行特定测试
python3 client_tests.py --test basic     # 仅基本操作
python3 client_tests.py --test cluster   # 仅集群健康检查
python3 client_tests.py --test stress    # 仅压力测试

# 指定输出文件
python3 client_tests.py --output my_test_report.json
```

### 2. 性能测试 (`performance_test.py`)

专业的性能基准测试工具：

- **负载测试**: 逐步增加并发用户数，测试系统容量
- **延迟基准测试**: 测量各种操作的响应时间分布
- **吞吐量测试**: 测量系统的QPS和数据吞吐量
- **系统资源监控**: 实时监控CPU、内存等系统资源使用情况

#### 运行方法

```bash
# 运行完整性能测试
python3 performance_test.py --config test_config.yaml

# 运行特定类型性能测试
python3 performance_test.py --test-type load     # 仅负载测试
python3 performance_test.py --test-type latency  # 仅延迟测试

# 指定输出位置
python3 performance_test.py --output perf_report.json --charts ./my_charts
```

#### 性能指标

- **QPS (Queries Per Second)**: 每秒查询数
- **延迟分布**: P50, P95, P99, P999延迟
- **成功率**: 操作成功的百分比
- **吞吐量**: 数据传输速率 (MB/s)
- **系统资源**: CPU、内存使用率

### 3. C++测试客户端 (`cpp_client_test.cpp`)

使用系统原生API的C++测试客户端：

- 直接调用C++缓存客户端API
- 测试API的正确性和性能
- 多线程压力测试

#### 运行方法

```bash
# 编译并运行C++测试
make run-cpp-tests

# 或者手动编译运行
make cpp_client_test
./cpp_client_test --config test_config.yaml --output cpp_report.md
```

## ⚙️ 配置文件

### 配置文件结构 (`test_config.yaml`)

```yaml
# 服务器配置
servers:
  - "http://localhost:8081"  # cache-node1
  - "http://localhost:8082"  # cache-node2  
  - "http://localhost:8083"  # cache-node3

load_balancer: "http://localhost:8080"

# 基本测试配置
timeout: 30
retry_count: 3
test_duration_seconds: 60
concurrent_clients: 10
data_size_bytes: 1024
key_prefix: "test"

# 压力测试配置
stress_test_ops: 10000      # 总操作数
stress_test_threads: 50     # 并发线程数

# 性能测试配置
performance_test:
  warmup_seconds: 30
  test_duration_seconds: 300  # 5分钟
  min_concurrent_users: 1
  max_concurrent_users: 100
  user_step: 10
  read_ratio: 0.7   # 70%读操作
  write_ratio: 0.2  # 20%写操作
  delete_ratio: 0.1 # 10%删除操作
  target_p99_latency_ms: 100.0
  target_min_qps: 1000.0
```

### 生成配置模板

```bash
make generate-config-template
```

## 📊 测试报告

### 报告类型

1. **JSON格式报告**: 详细的测试数据和指标
2. **Markdown格式报告**: 可读性强的测试总结
3. **性能图表**: PNG格式的性能可视化图表

### 报告内容

- **测试概要**: 总测试数、成功率、执行时间
- **详细结果**: 每个测试的具体数据和错误信息
- **性能指标**: QPS、延迟分布、吞吐量
- **图表可视化**: 性能趋势、延迟分布图

### 示例报告输出

```
============================================================
分布式缓存数据库测试报告
============================================================
总测试数: 5
成功: 5 | 失败: 0
成功率: 100.00%
============================================================
✅ cluster_health: 1250.25ms
✅ basic_operations: 156.78ms
✅ batch_operations: 289.45ms
✅ large_data: 445.67ms
✅ stress_test: 45678.90ms
============================================================
```

## 🔧 高级用法

### 1. 自定义测试场景

你可以修改配置文件来定制测试场景：

```yaml
# 银行场景测试配置
performance_test:
  test_duration_seconds: 1800  # 30分钟长期测试
  max_concurrent_users: 500    # 高并发模拟
  read_ratio: 0.8             # 银行系统读多写少
  write_ratio: 0.15
  delete_ratio: 0.05
  target_p99_latency_ms: 50   # 严格延迟要求
```

### 2. 集成到CI/CD

```bash
# 在CI/CD管道中运行快速测试
make run-quick-tests

# 检查返回值
if [ $? -eq 0 ]; then
    echo "测试通过"
else
    echo "测试失败"
    exit 1
fi
```

### 3. 监控和告警

```bash
# 实时监控系统性能
make monitor-performance

# 在后台运行长期测试并监控
nohup make run-performance-tests > perf_test.log 2>&1 &
```

### 4. 故障注入测试

修改配置文件启用故障注入：

```yaml
fault_injection:
  enabled: true
  scenarios:
    - name: "node_failure"
      probability: 0.01
      duration_seconds: 30
```

## 🐛 故障排除

### 常见问题

1. **连接失败**
   ```bash
   # 检查服务状态
   make check-services
   
   # 检查网络连接
   curl http://localhost:8080/api/v1/ping
   ```

2. **依赖安装失败**
   ```bash
   # Ubuntu/Debian
   sudo apt-get update
   sudo apt-get install python3-pip build-essential libyaml-cpp-dev
   
   # CentOS/RHEL
   sudo yum install python3-pip gcc-c++ yaml-cpp-devel
   ```

3. **性能测试内存不足**
   ```bash
   # 减少并发数
   # 在test_config.yaml中调整
   max_concurrent_users: 50  # 从100减少到50
   ```

4. **C++编译错误**
   ```bash
   # 检查编译器版本
   g++ --version  # 需要支持C++17
   
   # 安装依赖
   make install-cpp-deps
   ```

### 日志文件

- `cache_test.log`: Python测试客户端日志
- `perf_test.log`: 性能测试详细日志
- `test_reports/`: 所有测试报告存储目录

## 📈 性能基准

### 目标性能指标

基于README中提到的系统设计目标：

- **延迟性能**: 99.9%操作在100ms内完成
- **吞吐量**: 单核读≥2000 QPS，写≥1000 TPS
- **可用性**: 99.9%可用性保证
- **扩展性**: 线性扩展至10节点，扩展比≥0.7

### 性能测试结果示例

```
性能基准:
  峰值QPS: 15,432.67
  最佳P99延迟: 23.45ms
  最优并发数: 80

目标达成情况:
  QPS目标 (1000): ✅ 达成
  P99延迟目标 (100ms): ✅ 达成
```

## 🤝 贡献指南

### 添加新测试

1. 在相应的测试文件中添加新的测试方法
2. 更新配置文件支持新的测试参数
3. 在Makefile中添加相应的目标
4. 更新本README文档

### 提交规范

- 确保所有测试通过：`make run-quick-tests`
- 添加适当的注释和文档
- 遵循现有的代码风格

## 📞 支持

如果在使用过程中遇到问题：

1. 查看故障排除部分
2. 检查日志文件
3. 运行 `make help` 查看所有可用命令
4. 提交Issue并附上详细的错误信息和环境信息

---

**注意**: 这个测试套件设计用于开发和测试环境。在生产环境中运行大规模性能测试前，请确保有适当的资源和权限。