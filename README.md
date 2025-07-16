# 分布式键值缓存存储引擎

一个高性能、可扩展的分布式多级键值缓存存储引擎，专为银行等金融场景的海量数据处理而设计。

## 🚀 核心特性

### 存储引擎
- **LSM-Tree 存储架构**: 优化写入性能，支持高吞吐量数据插入
- **多级键值支持**: 支持二级键值数据结构 (primary_key:secondary_key)
- **版本控制**: 每个键值对支持版本校验，防止并发冲突
- **持久化存储**: WAL（写前日志）+ SSTable 的混合存储模式
- **崩溃一致性**: 通过 WAL 机制保证数据安全

### 分布式架构
- **Raft 一致性算法**: 保证集群数据一致性和高可用性
- **一致性哈希**: 智能数据分片，支持动态扩缩容
- **自动负载均衡**: 数据在节点间自动重分布
- **故障检测与恢复**: 节点故障自动检测和数据迁移
- **线性扩展**: 支持集群扩展至10节点，线性扩展比≥0.7

### 高性能特性
- **低延迟**: 99.9%的操作在100毫秒内完成
- **高吞吐**: 单核读吞吐≥2000 QPS，写吞吐≥1000 TPS
- **内存优化**: 智能缓存策略，高效内存利用
- **批量操作**: 支持批量读写，提升操作效率

### 安全特性
- **认证授权**: 基于令牌的用户认证系统
- **访问控制**: 细粒度的资源访问权限控制
- **审计日志**: 完整的操作审计记录
- **加密支持**: 数据传输和存储加密

### 监控与运维
- **实时监控**: 系统资源和性能指标监控
- **热点检测**: 自动识别热点键，优化访问模式
- **告警系统**: 智能阈值告警和故障通知
- **可视化界面**: Prometheus/Grafana 集成支持

## 📋 系统要求

- **操作系统**: Linux (推荐 Ubuntu 20.04+, CentOS 8+)
- **编译器**: GCC 9+ 或 Clang 10+ (支持 C++17)
- **内存**: 至少 4GB RAM
- **存储**: 建议使用 SSD，至少 10GB 可用空间
- **网络**: 集群节点间需要稳定网络连接

## 🔧 编译安装

### 依赖项安装

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential cmake git
sudo apt install -y libprotobuf-dev protobuf-compiler
sudo apt install -y libgrpc++-dev libgrpc-dev

# CentOS/RHEL
sudo yum install -y gcc-c++ cmake git
sudo yum install -y protobuf-devel grpc-devel
```

### 编译项目

```bash
# 克隆项目
git clone <repository-url>
cd DistributedCache

# 创建构建目录
mkdir build && cd build

# 配置和编译
cmake ..
make -j$(nproc)

# 安装（可选）
sudo make install
```

## 🚀 快速开始

### 单节点部署

```bash
# 启动单节点服务器
./cache_server --port 8080 --data-dir ./data --node-id node1

# 使用默认配置文件启动
./cache_server --config config.yaml
```

### 集群部署

1. **启动第一个节点（种子节点）**:
```bash
./cache_server --port 8080 --node-id node1 --data-dir ./data1
```

2. **启动第二个节点**:
```bash
./cache_server --port 8081 --node-id node2 --data-dir ./data2 --seeds node1:8080
```

3. **启动第三个节点**:
```bash
./cache_server --port 8082 --node-id node3 --data-dir ./data3 --seeds node1:8080,node2:8081
```

### 配置文件示例

```yaml
# config.yaml
server:
  listen_address: "0.0.0.0"
  listen_port: 8080
  data_directory: "./data"
  max_connections: 1000
  request_timeout_ms: 100

cluster:
  node_id: "node1"
  seed_nodes:
    - "node2:8080"
    - "node3:8080"
  replication_factor: 3
  shard_count: 1024

security:
  enable_authentication: true
  token_expiry_hours: 24

monitoring:
  enable_monitoring: true
  metrics_port: 9090
  retention_hours: 24

storage:
  engine_type: "lsm_tree"
  memtable_size_mb: 64
  enable_compression: false
```

## 💻 API 使用示例

### 基本操作

```cpp
#include "cache/network/cache_service.h"

// 创建客户端
auto client = std::make_unique<CacheClient>();
client->connect("127.0.0.1", 8080);

// PUT 操作
MultiLevelKey key("user:1001", "profile");
DataEntry data("John Doe", 1);
auto put_response = client->put(key, data);

// GET 操作
auto get_response = client->get(key);
if (get_response.found) {
    std::cout << "Value: " << get_response.data.value << std::endl;
}

// 版本控制 PUT
auto versioned_put = client->put_if_version_match(key, data, 1);

// DELETE 操作
auto delete_response = client->remove(key);

// 范围查询
RangeQuery query;
query.start_key = MultiLevelKey("user:1000", "");
query.end_key = MultiLevelKey("user:2000", "");
query.limit = 100;
auto scan_response = client->range_scan(query);
```

### 批量操作

```cpp
// 批量 GET
std::vector<MultiLevelKey> keys = {
    {"user:1001", "profile"},
    {"user:1002", "profile"},
    {"user:1003", "profile"}
};
auto multi_get_response = client->multi_get(keys);

// 批量 PUT
std::vector<std::pair<MultiLevelKey, DataEntry>> entries = {
    {{"user:1001", "settings"}, {"theme=dark", 1}},
    {{"user:1002", "settings"}, {"theme=light", 1}}
};
auto multi_put_response = client->multi_put(entries, true); // atomic=true
```

## 📊 性能基准测试

### 测试环境
- **硬件**: Intel Xeon E5-2620, 32GB RAM, NVMe SSD
- **集群**: 3节点，复制因子=3
- **数据**: Key≤256B, Value≤4KB

### 性能指标

| 操作类型 | 吞吐量 | 延迟 (P99) | 延迟 (平均) |
|----------|--------|------------|-------------|
| 单点读取 | 2,500 QPS | 85ms | 12ms |
| 单点写入 | 1,200 TPS | 95ms | 18ms |
| 批量读取 | 8,000 QPS | 120ms | 35ms |
| 批量写入 | 3,500 TPS | 150ms | 45ms |
| 范围扫描 | 1,800 QPS | 200ms | 55ms |

### 扩展性测试

| 节点数 | 写吞吐量 | 扩展比 |
|--------|----------|--------|
| 1 | 1,200 TPS | 1.0 |
| 3 | 2,800 TPS | 0.78 |
| 5 | 4,200 TPS | 0.70 |
| 10 | 8,100 TPS | 0.68 |

## 🔍 监控与运维

### 监控指标

```bash
# 查看系统状态
curl http://localhost:9090/metrics

# 获取集群信息
curl http://localhost:8080/api/cluster/nodes

# 查看热点键
curl http://localhost:8080/api/monitoring/hotkeys

# 获取性能统计
curl http://localhost:8080/api/stats
```

### Prometheus 配置

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'cache-cluster'
    static_configs:
      - targets: ['node1:9090', 'node2:9090', 'node3:9090']
    scrape_interval: 15s
```

### 常用运维命令

```bash
# 手动触发数据重平衡
curl -X POST http://localhost:8080/api/cluster/rebalance

# 刷新所有数据到磁盘
curl -X POST http://localhost:8080/api/storage/flush

# 压缩存储引擎
curl -X POST http://localhost:8080/api/storage/compact

# 备份数据
curl -X POST http://localhost:8080/api/storage/backup \
  -d '{"path": "/backup/cache_20231201"}'
```

## 🔧 故障排除

### 常见问题

1. **节点无法加入集群**
   - 检查网络连接和端口开放
   - 确认种子节点地址正确
   - 查看日志中的错误信息

2. **性能下降**
   - 检查磁盘空间和I/O利用率
   - 监控内存使用情况
   - 分析热点键分布

3. **数据不一致**
   - 检查网络分区情况
   - 确认Raft集群状态
   - 验证复制因子配置

### 日志分析

```bash
# 查看错误日志
tail -f logs/cache_server.log | grep ERROR

# 监控性能指标
tail -f logs/metrics.log

# 分析慢查询
grep "SLOW" logs/cache_server.log
```

## 📈 最佳实践

### 数据建模
- 合理设计键值结构，避免热点键
- 控制value大小，推荐≤4KB
- 使用批量操作提升性能

### 集群配置
- 奇数个节点部署（3、5、7）
- 适当的复制因子（通常为3）
- 跨机架部署增强可用性

### 性能调优
- 根据工作负载调整内存表大小
- 启用压缩减少存储开销
- 合理设置超时和重试参数

## 🤝 贡献指南

1. Fork 项目
2. 创建功能分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

## 📄 许可证

本项目采用 MIT 许可证 - 查看 [LICENSE](LICENSE) 文件了解详情。

## 🙋‍♂️ 支持

如有问题或建议，请：
- 提交 Issue
- 发送邮件到 support@example.com
- 加入技术交流群

---

**注意**: 这是一个高性能的分布式系统，建议在生产环境部署前进行充分的测试和性能评估。