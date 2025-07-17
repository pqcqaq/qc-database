#!/usr/bin/env python3
"""
分布式缓存数据库性能测试工具
专注于负载测试、延迟基准测试和吞吐量测量
"""

import asyncio
import aiohttp
import time
import random
import string
import statistics
import yaml
import argparse
import json
import logging
import matplotlib.pyplot as plt
import pandas as pd
from datetime import datetime
from typing import List, Dict, Tuple
from dataclasses import dataclass
import numpy as np
from concurrent.futures import ThreadPoolExecutor
import psutil
import os

# 配置日志
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

@dataclass
class PerformanceTestConfig:
    """性能测试配置"""
    servers: List[str]
    load_balancer: str
    
    # 负载测试配置
    warmup_seconds: int = 30
    test_duration_seconds: int = 300  # 5分钟
    ramp_up_seconds: int = 60  # 逐步增加负载
    
    # 并发配置
    min_concurrent_users: int = 1
    max_concurrent_users: int = 100
    user_step: int = 10
    
    # 数据配置
    key_size_bytes: int = 32
    value_size_bytes: int = 1024
    
    # 操作比例
    read_ratio: float = 0.7   # 70%读操作
    write_ratio: float = 0.2  # 20%写操作
    delete_ratio: float = 0.1 # 10%删除操作
    
    # 基准目标
    target_p99_latency_ms: float = 100.0
    target_p999_latency_ms: float = 200.0
    target_min_qps: float = 1000.0
    
    @classmethod
    def from_file(cls, config_file: str) -> 'PerformanceTestConfig':
        """从YAML文件加载配置"""
        with open(config_file, 'r', encoding='utf-8') as f:
            data = yaml.safe_load(f)
        
        config = cls(
            servers=data.get('servers', ['http://localhost:8081', 'http://localhost:8082', 'http://localhost:8083']),
            load_balancer=data.get('load_balancer', 'http://localhost:8080')
        )
        
        # 更新其他配置
        perf_config = data.get('performance_test', {})
        for key, value in perf_config.items():
            if hasattr(config, key):
                setattr(config, key, value)
                
        return config

@dataclass
class PerformanceMetrics:
    """性能指标"""
    timestamp: float
    concurrent_users: int
    operation_count: int
    success_count: int
    error_count: int
    total_bytes_sent: int
    total_bytes_received: int
    latencies_ms: List[float]
    
    @property
    def success_rate(self) -> float:
        if self.operation_count == 0:
            return 0.0
        return self.success_count / self.operation_count
    
    @property
    def error_rate(self) -> float:
        if self.operation_count == 0:
            return 0.0
        return self.error_count / self.operation_count
    
    @property
    def avg_latency_ms(self) -> float:
        return statistics.mean(self.latencies_ms) if self.latencies_ms else 0.0
    
    @property
    def p50_latency_ms(self) -> float:
        return statistics.median(self.latencies_ms) if self.latencies_ms else 0.0
    
    @property
    def p95_latency_ms(self) -> float:
        return statistics.quantile(self.latencies_ms, 0.95) if self.latencies_ms else 0.0
    
    @property
    def p99_latency_ms(self) -> float:
        return statistics.quantile(self.latencies_ms, 0.99) if self.latencies_ms else 0.0
    
    @property
    def p999_latency_ms(self) -> float:
        return statistics.quantile(self.latencies_ms, 0.999) if self.latencies_ms else 0.0
    
    @property
    def min_latency_ms(self) -> float:
        return min(self.latencies_ms) if self.latencies_ms else 0.0
    
    @property
    def max_latency_ms(self) -> float:
        return max(self.latencies_ms) if self.latencies_ms else 0.0

class PerformanceTestClient:
    """性能测试客户端"""
    
    def __init__(self, server_url: str, user_id: int):
        self.server_url = server_url.rstrip('/')
        self.user_id = user_id
        self.session = None
        self.operation_count = 0
        self.success_count = 0
        self.error_count = 0
        self.latencies = []
        self.bytes_sent = 0
        self.bytes_received = 0
        
    async def __aenter__(self):
        connector = aiohttp.TCPConnector(limit=100, limit_per_host=30)
        timeout = aiohttp.ClientTimeout(total=30, connect=5)
        self.session = aiohttp.ClientSession(connector=connector, timeout=timeout)
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        if self.session:
            await self.session.close()
    
    def generate_key(self, key_size: int) -> str:
        """生成指定大小的随机键"""
        return f"perf_test_{self.user_id}_" + ''.join(random.choices(string.ascii_letters + string.digits, k=key_size-20))
    
    def generate_value(self, value_size: int) -> str:
        """生成指定大小的随机值"""
        return ''.join(random.choices(string.ascii_letters + string.digits, k=value_size))
    
    async def put_operation(self, key: str, value: str) -> Tuple[bool, float, int, int]:
        """执行PUT操作"""
        start_time = time.time()
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            data = {"value": value}
            payload = json.dumps(data).encode('utf-8')
            
            async with self.session.put(url, data=payload, headers={'Content-Type': 'application/json'}) as response:
                response_data = await response.read()
                latency_ms = (time.time() - start_time) * 1000
                
                success = response.status in [200, 201]
                bytes_sent = len(payload)
                bytes_received = len(response_data)
                
                return success, latency_ms, bytes_sent, bytes_received
                
        except Exception as e:
            latency_ms = (time.time() - start_time) * 1000
            return False, latency_ms, 0, 0
    
    async def get_operation(self, key: str) -> Tuple[bool, float, int, int]:
        """执行GET操作"""
        start_time = time.time()
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            
            async with self.session.get(url) as response:
                response_data = await response.read()
                latency_ms = (time.time() - start_time) * 1000
                
                success = response.status in [200, 404]  # 404也算成功，表示键不存在
                bytes_sent = 0
                bytes_received = len(response_data)
                
                return success, latency_ms, bytes_sent, bytes_received
                
        except Exception as e:
            latency_ms = (time.time() - start_time) * 1000
            return False, latency_ms, 0, 0
    
    async def delete_operation(self, key: str) -> Tuple[bool, float, int, int]:
        """执行DELETE操作"""
        start_time = time.time()
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            
            async with self.session.delete(url) as response:
                response_data = await response.read()
                latency_ms = (time.time() - start_time) * 1000
                
                success = response.status in [200, 204, 404]  # 404也算成功
                bytes_sent = 0
                bytes_received = len(response_data)
                
                return success, latency_ms, bytes_sent, bytes_received
                
        except Exception as e:
            latency_ms = (time.time() - start_time) * 1000
            return False, latency_ms, 0, 0
    
    async def run_workload(self, config: PerformanceTestConfig, duration_seconds: int, keys_pool: List[str]):
        """运行工作负载"""
        end_time = time.time() + duration_seconds
        
        while time.time() < end_time:
            # 根据配置的比例选择操作类型
            rand = random.random()
            if rand < config.read_ratio:
                # GET操作
                key = random.choice(keys_pool) if keys_pool else self.generate_key(config.key_size_bytes)
                success, latency, bytes_sent, bytes_recv = await self.get_operation(key)
            elif rand < config.read_ratio + config.write_ratio:
                # PUT操作
                key = self.generate_key(config.key_size_bytes)
                value = self.generate_value(config.value_size_bytes)
                keys_pool.append(key)
                success, latency, bytes_sent, bytes_recv = await self.put_operation(key, value)
            else:
                # DELETE操作
                if keys_pool:
                    key = keys_pool.pop(random.randint(0, len(keys_pool)-1))
                    success, latency, bytes_sent, bytes_recv = await self.delete_operation(key)
                else:
                    # 如果没有键可删除，就执行GET操作
                    key = self.generate_key(config.key_size_bytes)
                    success, latency, bytes_sent, bytes_recv = await self.get_operation(key)
            
            # 记录指标
            self.operation_count += 1
            self.latencies.append(latency)
            self.bytes_sent += bytes_sent
            self.bytes_received += bytes_recv
            
            if success:
                self.success_count += 1
            else:
                self.error_count += 1
            
            # 小的随机延迟，避免过于规律的请求模式
            await asyncio.sleep(random.uniform(0.001, 0.01))

class PerformanceTester:
    """性能测试器"""
    
    def __init__(self, config: PerformanceTestConfig):
        self.config = config
        self.results: List[PerformanceMetrics] = []
        self.system_metrics = []
        
    async def warmup(self):
        """预热阶段"""
        logger.info(f"开始预热阶段，持续 {self.config.warmup_seconds} 秒")
        
        # 使用少量客户端进行预热
        warmup_clients = min(5, self.config.max_concurrent_users)
        await self._run_load_test(warmup_clients, self.config.warmup_seconds, is_warmup=True)
        
        logger.info("预热阶段完成")
    
    async def _run_load_test(self, concurrent_users: int, duration_seconds: int, is_warmup: bool = False) -> PerformanceMetrics:
        """运行负载测试"""
        start_time = time.time()
        keys_pool = []  # 共享的键池
        
        # 创建客户端
        clients = []
        for i in range(concurrent_users):
            client = PerformanceTestClient(self.config.load_balancer, i)
            clients.append(client)
        
        # 启动所有客户端
        async with asyncio.TaskGroup() as tg:
            for client in clients:
                await client.__aenter__()
        
        # 运行工作负载
        tasks = []
        for client in clients:
            task = asyncio.create_task(client.run_workload(self.config, duration_seconds, keys_pool))
            tasks.append(task)
        
        # 收集系统指标（在单独的线程中）
        system_metrics_task = None
        if not is_warmup:
            system_metrics_task = asyncio.create_task(self._collect_system_metrics(duration_seconds))
        
        # 等待所有任务完成
        try:
            await asyncio.gather(*tasks)
            if system_metrics_task:
                await system_metrics_task
        finally:
            # 清理客户端
            for client in clients:
                await client.__aexit__(None, None, None)
        
        # 聚合结果
        total_operations = sum(client.operation_count for client in clients)
        total_success = sum(client.success_count for client in clients)
        total_errors = sum(client.error_count for client in clients)
        total_bytes_sent = sum(client.bytes_sent for client in clients)
        total_bytes_received = sum(client.bytes_received for client in clients)
        
        all_latencies = []
        for client in clients:
            all_latencies.extend(client.latencies)
        
        metrics = PerformanceMetrics(
            timestamp=start_time,
            concurrent_users=concurrent_users,
            operation_count=total_operations,
            success_count=total_success,
            error_count=total_errors,
            total_bytes_sent=total_bytes_sent,
            total_bytes_received=total_bytes_received,
            latencies_ms=all_latencies
        )
        
        if not is_warmup:
            self.results.append(metrics)
            
            # 计算QPS
            actual_duration = time.time() - start_time
            qps = total_operations / actual_duration if actual_duration > 0 else 0
            
            logger.info(f"并发用户: {concurrent_users}, QPS: {qps:.2f}, "
                       f"P99延迟: {metrics.p99_latency_ms:.2f}ms, "
                       f"成功率: {metrics.success_rate:.3f}")
        
        return metrics
    
    async def _collect_system_metrics(self, duration_seconds: int):
        """收集系统指标"""
        start_time = time.time()
        
        while time.time() - start_time < duration_seconds:
            try:
                cpu_percent = psutil.cpu_percent(interval=1)
                memory = psutil.virtual_memory()
                disk = psutil.disk_usage('/')
                
                self.system_metrics.append({
                    'timestamp': time.time(),
                    'cpu_percent': cpu_percent,
                    'memory_percent': memory.percent,
                    'memory_available_gb': memory.available / (1024**3),
                    'disk_percent': disk.percent
                })
                
                await asyncio.sleep(5)  # 每5秒收集一次
            except Exception as e:
                logger.warning(f"收集系统指标失败: {e}")
    
    async def run_load_test(self):
        """运行完整的负载测试"""
        logger.info("开始负载测试")
        
        # 预热
        await self.warmup()
        
        # 逐步增加并发用户数
        for users in range(self.config.min_concurrent_users, 
                          self.config.max_concurrent_users + 1, 
                          self.config.user_step):
            
            logger.info(f"测试并发用户数: {users}")
            await self._run_load_test(users, self.config.test_duration_seconds)
            
            # 短暂休息
            await asyncio.sleep(5)
        
        logger.info("负载测试完成")
    
    async def run_latency_benchmark(self):
        """运行延迟基准测试"""
        logger.info("开始延迟基准测试")
        
        # 使用固定的并发数进行延迟测试
        benchmark_users = min(10, self.config.max_concurrent_users)
        benchmark_duration = 120  # 2分钟
        
        await self._run_load_test(benchmark_users, benchmark_duration)
        
        logger.info("延迟基准测试完成")
    
    def analyze_results(self) -> Dict:
        """分析测试结果"""
        if not self.results:
            return {}
        
        analysis = {
            'summary': {
                'total_tests': len(self.results),
                'max_concurrent_users': max(r.concurrent_users for r in self.results),
                'total_operations': sum(r.operation_count for r in self.results),
                'overall_success_rate': sum(r.success_count for r in self.results) / sum(r.operation_count for r in self.results)
            },
            'performance': [],
            'benchmarks': {
                'peak_qps': 0,
                'best_p99_latency': float('inf'),
                'optimal_concurrent_users': 0
            }
        }
        
        for result in self.results:
            duration = self.config.test_duration_seconds
            qps = result.operation_count / duration
            
            perf_data = {
                'concurrent_users': result.concurrent_users,
                'qps': qps,
                'avg_latency_ms': result.avg_latency_ms,
                'p50_latency_ms': result.p50_latency_ms,
                'p95_latency_ms': result.p95_latency_ms,
                'p99_latency_ms': result.p99_latency_ms,
                'p999_latency_ms': result.p999_latency_ms,
                'success_rate': result.success_rate,
                'error_rate': result.error_rate,
                'throughput_mbps': (result.total_bytes_sent + result.total_bytes_received) / (1024*1024) / duration
            }
            
            analysis['performance'].append(perf_data)
            
            # 更新最佳指标
            if qps > analysis['benchmarks']['peak_qps']:
                analysis['benchmarks']['peak_qps'] = qps
                analysis['benchmarks']['optimal_concurrent_users'] = result.concurrent_users
            
            if result.p99_latency_ms < analysis['benchmarks']['best_p99_latency']:
                analysis['benchmarks']['best_p99_latency'] = result.p99_latency_ms
        
        return analysis
    
    def generate_charts(self, output_dir: str):
        """生成性能图表"""
        if not self.results:
            return
        
        os.makedirs(output_dir, exist_ok=True)
        
        # 提取数据
        users = [r.concurrent_users for r in self.results]
        qps = [r.operation_count / self.config.test_duration_seconds for r in self.results]
        p99_latencies = [r.p99_latency_ms for r in self.results]
        success_rates = [r.success_rate for r in self.results]
        
        # QPS vs 并发用户数
        plt.figure(figsize=(12, 8))
        
        plt.subplot(2, 2, 1)
        plt.plot(users, qps, 'b-o', linewidth=2, markersize=6)
        plt.xlabel('并发用户数')
        plt.ylabel('QPS')
        plt.title('QPS vs 并发用户数')
        plt.grid(True, alpha=0.3)
        
        # P99延迟 vs 并发用户数
        plt.subplot(2, 2, 2)
        plt.plot(users, p99_latencies, 'r-o', linewidth=2, markersize=6)
        plt.xlabel('并发用户数')
        plt.ylabel('P99延迟 (ms)')
        plt.title('P99延迟 vs 并发用户数')
        plt.grid(True, alpha=0.3)
        
        # 成功率 vs 并发用户数
        plt.subplot(2, 2, 3)
        plt.plot(users, [sr * 100 for sr in success_rates], 'g-o', linewidth=2, markersize=6)
        plt.xlabel('并发用户数')
        plt.ylabel('成功率 (%)')
        plt.title('成功率 vs 并发用户数')
        plt.grid(True, alpha=0.3)
        
        # 延迟分布（最后一个测试的结果）
        plt.subplot(2, 2, 4)
        if self.results:
            plt.hist(self.results[-1].latencies_ms, bins=50, alpha=0.7, color='purple')
            plt.xlabel('延迟 (ms)')
            plt.ylabel('频次')
            plt.title(f'延迟分布 ({self.results[-1].concurrent_users} 并发用户)')
            plt.grid(True, alpha=0.3)
        
        plt.tight_layout()
        plt.savefig(f"{output_dir}/performance_charts.png", dpi=300, bbox_inches='tight')
        plt.close()
        
        # 系统资源使用图表
        if self.system_metrics:
            plt.figure(figsize=(12, 6))
            
            timestamps = [m['timestamp'] for m in self.system_metrics]
            start_time = timestamps[0]
            relative_times = [(t - start_time) / 60 for t in timestamps]  # 转换为分钟
            
            plt.subplot(2, 2, 1)
            cpu_data = [m['cpu_percent'] for m in self.system_metrics]
            plt.plot(relative_times, cpu_data, 'b-', linewidth=2)
            plt.xlabel('时间 (分钟)')
            plt.ylabel('CPU使用率 (%)')
            plt.title('CPU使用率')
            plt.grid(True, alpha=0.3)
            
            plt.subplot(2, 2, 2)
            memory_data = [m['memory_percent'] for m in self.system_metrics]
            plt.plot(relative_times, memory_data, 'r-', linewidth=2)
            plt.xlabel('时间 (分钟)')
            plt.ylabel('内存使用率 (%)')
            plt.title('内存使用率')
            plt.grid(True, alpha=0.3)
            
            plt.tight_layout()
            plt.savefig(f"{output_dir}/system_metrics.png", dpi=300, bbox_inches='tight')
            plt.close()
        
        logger.info(f"性能图表已保存到 {output_dir}")
    
    def generate_report(self, output_file: str):
        """生成详细的性能测试报告"""
        analysis = self.analyze_results()
        
        report = {
            'test_info': {
                'test_time': datetime.now().isoformat(),
                'config': {
                    'servers': self.config.servers,
                    'load_balancer': self.config.load_balancer,
                    'test_duration_seconds': self.config.test_duration_seconds,
                    'max_concurrent_users': self.config.max_concurrent_users,
                    'read_ratio': self.config.read_ratio,
                    'write_ratio': self.config.write_ratio,
                    'delete_ratio': self.config.delete_ratio
                }
            },
            'analysis': analysis,
            'detailed_results': []
        }
        
        for result in self.results:
            duration = self.config.test_duration_seconds
            detailed = {
                'concurrent_users': result.concurrent_users,
                'operation_count': result.operation_count,
                'success_count': result.success_count,
                'error_count': result.error_count,
                'qps': result.operation_count / duration,
                'success_rate': result.success_rate,
                'latency_stats': {
                    'min_ms': result.min_latency_ms,
                    'max_ms': result.max_latency_ms,
                    'avg_ms': result.avg_latency_ms,
                    'p50_ms': result.p50_latency_ms,
                    'p95_ms': result.p95_latency_ms,
                    'p99_ms': result.p99_latency_ms,
                    'p999_ms': result.p999_latency_ms
                },
                'throughput': {
                    'bytes_sent': result.total_bytes_sent,
                    'bytes_received': result.total_bytes_received,
                    'mbps': (result.total_bytes_sent + result.total_bytes_received) / (1024*1024) / duration
                }
            }
            report['detailed_results'].append(detailed)
        
        # 保存报告
        with open(output_file, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        
        # 打印摘要
        print("\n" + "="*80)
        print("分布式缓存数据库性能测试报告")
        print("="*80)
        
        if analysis:
            print(f"测试概要:")
            print(f"  总测试数: {analysis['summary']['total_tests']}")
            print(f"  最大并发用户: {analysis['summary']['max_concurrent_users']}")
            print(f"  总操作数: {analysis['summary']['total_operations']:,}")
            print(f"  整体成功率: {analysis['summary']['overall_success_rate']:.3f}")
            
            print(f"\n性能基准:")
            print(f"  峰值QPS: {analysis['benchmarks']['peak_qps']:.2f}")
            print(f"  最佳P99延迟: {analysis['benchmarks']['best_p99_latency']:.2f}ms")
            print(f"  最优并发数: {analysis['benchmarks']['optimal_concurrent_users']}")
            
            # 检查是否达到目标
            print(f"\n目标达成情况:")
            meets_qps = analysis['benchmarks']['peak_qps'] >= self.config.target_min_qps
            meets_p99 = analysis['benchmarks']['best_p99_latency'] <= self.config.target_p99_latency_ms
            
            print(f"  QPS目标 ({self.config.target_min_qps}): {'✅ 达成' if meets_qps else '❌ 未达成'}")
            print(f"  P99延迟目标 ({self.config.target_p99_latency_ms}ms): {'✅ 达成' if meets_p99 else '❌ 未达成'}")
        
        print("="*80)
        print(f"详细报告已保存到: {output_file}")

async def main():
    """主函数"""
    parser = argparse.ArgumentParser(description='分布式缓存数据库性能测试工具')
    parser.add_argument('--config', '-c', default='test_config.yaml', help='配置文件路径')
    parser.add_argument('--output', '-o', default='performance_report.json', help='报告输出文件')
    parser.add_argument('--charts', default='./charts', help='图表输出目录')
    parser.add_argument('--test-type', choices=['load', 'latency', 'all'], default='all', help='测试类型')
    
    args = parser.parse_args()
    
    # 加载配置
    try:
        if os.path.exists(args.config):
            config = PerformanceTestConfig.from_file(args.config)
        else:
            config = PerformanceTestConfig(
                servers=['http://localhost:8081', 'http://localhost:8082', 'http://localhost:8083'],
                load_balancer='http://localhost:8080'
            )
            logger.warning(f"配置文件 {args.config} 不存在，使用默认配置")
    except Exception as e:
        logger.error(f"加载配置失败: {e}")
        return
    
    # 创建性能测试器
    tester = PerformanceTester(config)
    
    try:
        # 运行测试
        if args.test_type in ['load', 'all']:
            await tester.run_load_test()
        
        if args.test_type in ['latency', 'all']:
            await tester.run_latency_benchmark()
        
        # 生成报告和图表
        tester.generate_report(args.output)
        tester.generate_charts(args.charts)
        
    except Exception as e:
        logger.error(f"性能测试执行失败: {e}")
        raise

if __name__ == "__main__":
    asyncio.run(main())