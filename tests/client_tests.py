#!/usr/bin/env python3
"""
分布式缓存数据库测试客户端
支持基本功能测试、压力测试、集群测试等
"""

import asyncio
import aiohttp
import json
import time
import random
import string
import statistics
import threading
import yaml
import argparse
import logging
from typing import List, Dict, Any, Optional, Tuple
from dataclasses import dataclass, asdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import uuid
from datetime import datetime
import csv
import os

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('cache_test.log'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

@dataclass
class TestConfig:
    """测试配置"""
    # 服务器配置
    servers: List[str]
    load_balancer: str
    timeout: int = 30
    retry_count: int = 3
    
    # 测试配置
    test_duration_seconds: int = 60
    concurrent_clients: int = 10
    data_size_bytes: int = 1024
    key_prefix: str = "test"
    
    # 压力测试配置
    stress_test_ops: int = 10000
    stress_test_threads: int = 50
    
    # 数据配置
    max_key_size: int = 256
    max_value_size: int = 4096
    
    @classmethod
    def from_file(cls, config_file: str) -> 'TestConfig':
        """从配置文件加载配置"""
        with open(config_file, 'r', encoding='utf-8') as f:
            config_data = yaml.safe_load(f)
        return cls(**config_data)

@dataclass
class TestResult:
    """测试结果"""
    test_name: str
    success: bool
    duration_ms: float
    error_message: str = ""
    data: Dict[str, Any] = None

@dataclass
class StressTestMetrics:
    """压力测试指标"""
    total_operations: int = 0
    successful_operations: int = 0
    failed_operations: int = 0
    total_duration_seconds: float = 0.0
    latencies_ms: List[float] = None
    
    def __post_init__(self):
        if self.latencies_ms is None:
            self.latencies_ms = []
    
    @property
    def success_rate(self) -> float:
        if self.total_operations == 0:
            return 0.0
        return self.successful_operations / self.total_operations
    
    @property
    def qps(self) -> float:
        if self.total_duration_seconds == 0:
            return 0.0
        return self.total_operations / self.total_duration_seconds
    
    @property
    def avg_latency_ms(self) -> float:
        if not self.latencies_ms:
            return 0.0
        return statistics.mean(self.latencies_ms)
    
    @property
    def p99_latency_ms(self) -> float:
        if not self.latencies_ms:
            return 0.0
        return statistics.quantile(self.latencies_ms, 0.99)
    
    @property
    def p999_latency_ms(self) -> float:
        if not self.latencies_ms:
            return 0.0
        return statistics.quantile(self.latencies_ms, 0.999)

class CacheClient:
    """缓存客户端"""
    
    def __init__(self, server_url: str, timeout: int = 30):
        self.server_url = server_url.rstrip('/')
        self.timeout = timeout
        self.session = None
        
    async def __aenter__(self):
        """异步上下文管理器入口"""
        self.session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=self.timeout)
        )
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """异步上下文管理器出口"""
        if self.session:
            await self.session.close()
    
    async def get(self, key: str) -> Tuple[bool, Optional[str], str]:
        """获取缓存值"""
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            async with self.session.get(url) as response:
                if response.status == 200:
                    data = await response.json()
                    return True, data.get('value'), ""
                elif response.status == 404:
                    return True, None, ""
                else:
                    error_text = await response.text()
                    return False, None, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, None, str(e)
    
    async def put(self, key: str, value: str) -> Tuple[bool, str]:
        """设置缓存值"""
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            data = {"value": value}
            async with self.session.put(url, json=data) as response:
                if response.status in [200, 201]:
                    return True, ""
                else:
                    error_text = await response.text()
                    return False, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, str(e)
    
    async def delete(self, key: str) -> Tuple[bool, str]:
        """删除缓存值"""
        try:
            url = f"{self.server_url}/api/v1/cache/{key}"
            async with self.session.delete(url) as response:
                if response.status in [200, 204]:
                    return True, ""
                elif response.status == 404:
                    return True, ""  # 已经不存在了
                else:
                    error_text = await response.text()
                    return False, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, str(e)
    
    async def multi_get(self, keys: List[str]) -> Tuple[bool, Dict[str, str], str]:
        """批量获取缓存值"""
        try:
            url = f"{self.server_url}/api/v1/cache/multi"
            data = {"keys": keys}
            async with self.session.post(url, json=data) as response:
                if response.status == 200:
                    result = await response.json()
                    return True, result.get('values', {}), ""
                else:
                    error_text = await response.text()
                    return False, {}, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, {}, str(e)
    
    async def multi_put(self, entries: Dict[str, str]) -> Tuple[bool, str]:
        """批量设置缓存值"""
        try:
            url = f"{self.server_url}/api/v1/cache/multi"
            data = {"entries": entries}
            async with self.session.put(url, json=data) as response:
                if response.status in [200, 201]:
                    return True, ""
                else:
                    error_text = await response.text()
                    return False, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, str(e)
    
    async def ping(self) -> Tuple[bool, str]:
        """健康检查"""
        try:
            url = f"{self.server_url}/api/v1/ping"
            async with self.session.get(url) as response:
                if response.status == 200:
                    return True, ""
                else:
                    error_text = await response.text()
                    return False, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, str(e)
    
    async def stats(self) -> Tuple[bool, Dict[str, Any], str]:
        """获取统计信息"""
        try:
            url = f"{self.server_url}/api/v1/stats"
            async with self.session.get(url) as response:
                if response.status == 200:
                    data = await response.json()
                    return True, data, ""
                else:
                    error_text = await response.text()
                    return False, {}, f"HTTP {response.status}: {error_text}"
        except Exception as e:
            return False, {}, str(e)

class CacheTestSuite:
    """缓存测试套件"""
    
    def __init__(self, config: TestConfig):
        self.config = config
        self.test_results: List[TestResult] = []
        
    def generate_random_string(self, length: int) -> str:
        """生成随机字符串"""
        return ''.join(random.choices(string.ascii_letters + string.digits, k=length))
    
    def generate_test_data(self, size_bytes: int) -> str:
        """生成测试数据"""
        return self.generate_random_string(size_bytes)
    
    async def test_basic_operations(self) -> TestResult:
        """基本操作测试"""
        start_time = time.time()
        
        try:
            async with CacheClient(self.config.load_balancer) as client:
                # 测试数据
                test_key = f"{self.config.key_prefix}_basic_test_{uuid.uuid4().hex[:8]}"
                test_value = self.generate_test_data(self.config.data_size_bytes)
                
                # 1. PUT操作
                success, error = await client.put(test_key, test_value)
                if not success:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"PUT failed: {error}"
                    )
                
                # 2. GET操作
                success, value, error = await client.get(test_key)
                if not success:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"GET failed: {error}"
                    )
                
                if value != test_value:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"Data mismatch: expected {test_value[:50]}..., got {value[:50] if value else None}..."
                    )
                
                # 3. DELETE操作
                success, error = await client.delete(test_key)
                if not success:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"DELETE failed: {error}"
                    )
                
                # 4. 验证删除
                success, value, error = await client.get(test_key)
                if not success:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"GET after DELETE failed: {error}"
                    )
                
                if value is not None:
                    return TestResult(
                        test_name="basic_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message="Key still exists after DELETE"
                    )
                
                return TestResult(
                    test_name="basic_operations",
                    success=True,
                    duration_ms=(time.time() - start_time) * 1000
                )
                
        except Exception as e:
            return TestResult(
                test_name="basic_operations",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=str(e)
            )
    
    async def test_batch_operations(self) -> TestResult:
        """批量操作测试"""
        start_time = time.time()
        
        try:
            async with CacheClient(self.config.load_balancer) as client:
                # 准备测试数据
                test_entries = {}
                test_keys = []
                for i in range(10):
                    key = f"{self.config.key_prefix}_batch_{uuid.uuid4().hex[:8]}_{i}"
                    value = self.generate_test_data(self.config.data_size_bytes)
                    test_entries[key] = value
                    test_keys.append(key)
                
                # 1. 批量PUT
                success, error = await client.multi_put(test_entries)
                if not success:
                    return TestResult(
                        test_name="batch_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"MULTI_PUT failed: {error}"
                    )
                
                # 2. 批量GET
                success, values, error = await client.multi_get(test_keys)
                if not success:
                    return TestResult(
                        test_name="batch_operations",
                        success=False,
                        duration_ms=(time.time() - start_time) * 1000,
                        error_message=f"MULTI_GET failed: {error}"
                    )
                
                # 3. 验证数据
                for key, expected_value in test_entries.items():
                    if key not in values or values[key] != expected_value:
                        return TestResult(
                            test_name="batch_operations",
                            success=False,
                            duration_ms=(time.time() - start_time) * 1000,
                            error_message=f"Batch data mismatch for key {key}"
                        )
                
                # 4. 清理
                for key in test_keys:
                    await client.delete(key)
                
                return TestResult(
                    test_name="batch_operations",
                    success=True,
                    duration_ms=(time.time() - start_time) * 1000,
                    data={"keys_tested": len(test_keys)}
                )
                
        except Exception as e:
            return TestResult(
                test_name="batch_operations",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=str(e)
            )
    
    async def test_cluster_health(self) -> TestResult:
        """集群健康检查测试"""
        start_time = time.time()
        healthy_nodes = []
        failed_nodes = []
        
        try:
            # 检查所有节点
            for server_url in self.config.servers:
                try:
                    async with CacheClient(server_url) as client:
                        success, error = await client.ping()
                        if success:
                            healthy_nodes.append(server_url)
                        else:
                            failed_nodes.append((server_url, error))
                except Exception as e:
                    failed_nodes.append((server_url, str(e)))
            
            # 检查负载均衡器
            lb_healthy = False
            lb_error = ""
            try:
                async with CacheClient(self.config.load_balancer) as client:
                    success, error = await client.ping()
                    if success:
                        lb_healthy = True
                    else:
                        lb_error = error
            except Exception as e:
                lb_error = str(e)
            
            # 判断集群是否健康
            cluster_healthy = len(healthy_nodes) >= len(self.config.servers) // 2 + 1 and lb_healthy
            
            return TestResult(
                test_name="cluster_health",
                success=cluster_healthy,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=lb_error if not lb_healthy else "",
                data={
                    "healthy_nodes": len(healthy_nodes),
                    "total_nodes": len(self.config.servers),
                    "failed_nodes": failed_nodes,
                    "load_balancer_healthy": lb_healthy,
                    "load_balancer_error": lb_error
                }
            )
            
        except Exception as e:
            return TestResult(
                test_name="cluster_health",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=str(e)
            )
    
    async def test_large_data(self) -> TestResult:
        """大数据测试"""
        start_time = time.time()
        
        try:
            async with CacheClient(self.config.load_balancer) as client:
                # 测试不同大小的数据
                test_sizes = [1024, 4096, 8192, 16384]  # 1KB, 4KB, 8KB, 16KB
                results = {}
                
                for size in test_sizes:
                    if size > self.config.max_value_size:
                        continue
                        
                    test_key = f"{self.config.key_prefix}_large_{size}_{uuid.uuid4().hex[:8]}"
                    test_value = self.generate_test_data(size)
                    
                    # PUT
                    put_start = time.time()
                    success, error = await client.put(test_key, test_value)
                    put_duration = (time.time() - put_start) * 1000
                    
                    if not success:
                        return TestResult(
                            test_name="large_data",
                            success=False,
                            duration_ms=(time.time() - start_time) * 1000,
                            error_message=f"PUT failed for size {size}: {error}"
                        )
                    
                    # GET
                    get_start = time.time()
                    success, value, error = await client.get(test_key)
                    get_duration = (time.time() - get_start) * 1000
                    
                    if not success:
                        return TestResult(
                            test_name="large_data",
                            success=False,
                            duration_ms=(time.time() - start_time) * 1000,
                            error_message=f"GET failed for size {size}: {error}"
                        )
                    
                    if value != test_value:
                        return TestResult(
                            test_name="large_data",
                            success=False,
                            duration_ms=(time.time() - start_time) * 1000,
                            error_message=f"Data corruption for size {size}"
                        )
                    
                    results[size] = {
                        "put_latency_ms": put_duration,
                        "get_latency_ms": get_duration
                    }
                    
                    # 清理
                    await client.delete(test_key)
                
                return TestResult(
                    test_name="large_data",
                    success=True,
                    duration_ms=(time.time() - start_time) * 1000,
                    data=results
                )
                
        except Exception as e:
            return TestResult(
                test_name="large_data",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=str(e)
            )
    
    async def stress_test_worker(self, worker_id: int, operation_count: int) -> StressTestMetrics:
        """压力测试工作线程"""
        metrics = StressTestMetrics()
        
        try:
            async with CacheClient(self.config.load_balancer) as client:
                for i in range(operation_count):
                    # 随机选择操作类型
                    operation = random.choice(['put', 'get', 'delete'])
                    test_key = f"{self.config.key_prefix}_stress_{worker_id}_{i}"
                    
                    start_time = time.time()
                    success = False
                    
                    try:
                        if operation == 'put':
                            test_value = self.generate_test_data(random.randint(100, self.config.data_size_bytes))
                            success, _ = await client.put(test_key, test_value)
                        elif operation == 'get':
                            success, _, _ = await client.get(test_key)
                        elif operation == 'delete':
                            success, _ = await client.delete(test_key)
                        
                        latency_ms = (time.time() - start_time) * 1000
                        metrics.latencies_ms.append(latency_ms)
                        
                        if success:
                            metrics.successful_operations += 1
                        else:
                            metrics.failed_operations += 1
                            
                    except Exception:
                        metrics.failed_operations += 1
                    
                    metrics.total_operations += 1
                    
        except Exception as e:
            logger.error(f"Stress test worker {worker_id} error: {e}")
        
        return metrics
    
    async def stress_test(self) -> TestResult:
        """压力测试"""
        start_time = time.time()
        
        try:
            logger.info(f"Starting stress test: {self.config.stress_test_ops} operations with {self.config.stress_test_threads} concurrent workers")
            
            # 计算每个工作线程的操作数
            ops_per_worker = self.config.stress_test_ops // self.config.stress_test_threads
            
            # 启动并发工作线程
            tasks = []
            for i in range(self.config.stress_test_threads):
                task = asyncio.create_task(self.stress_test_worker(i, ops_per_worker))
                tasks.append(task)
            
            # 等待所有任务完成
            results = await asyncio.gather(*tasks, return_exceptions=True)
            
            # 合并指标
            total_metrics = StressTestMetrics()
            for result in results:
                if isinstance(result, StressTestMetrics):
                    total_metrics.total_operations += result.total_operations
                    total_metrics.successful_operations += result.successful_operations
                    total_metrics.failed_operations += result.failed_operations
                    total_metrics.latencies_ms.extend(result.latencies_ms)
            
            total_metrics.total_duration_seconds = time.time() - start_time
            
            return TestResult(
                test_name="stress_test",
                success=total_metrics.success_rate >= 0.95,  # 95%成功率阈值
                duration_ms=total_metrics.total_duration_seconds * 1000,
                data={
                    "total_operations": total_metrics.total_operations,
                    "successful_operations": total_metrics.successful_operations,
                    "failed_operations": total_metrics.failed_operations,
                    "success_rate": total_metrics.success_rate,
                    "qps": total_metrics.qps,
                    "avg_latency_ms": total_metrics.avg_latency_ms,
                    "p99_latency_ms": total_metrics.p99_latency_ms,
                    "p999_latency_ms": total_metrics.p999_latency_ms
                }
            )
            
        except Exception as e:
            return TestResult(
                test_name="stress_test",
                success=False,
                duration_ms=(time.time() - start_time) * 1000,
                error_message=str(e)
            )
    
    async def run_all_tests(self) -> List[TestResult]:
        """运行所有测试"""
        logger.info("开始运行分布式缓存数据库测试套件")
        
        tests = [
            ("集群健康检查", self.test_cluster_health()),
            ("基本操作测试", self.test_basic_operations()),
            ("批量操作测试", self.test_batch_operations()),
            ("大数据测试", self.test_large_data()),
            ("压力测试", self.stress_test())
        ]
        
        results = []
        
        for test_name, test_coro in tests:
            logger.info(f"运行测试: {test_name}")
            start_time = time.time()
            
            try:
                result = await test_coro
                results.append(result)
                
                status = "✅ 成功" if result.success else "❌ 失败"
                duration = time.time() - start_time
                logger.info(f"测试 {test_name} {status} (耗时: {duration:.2f}s)")
                
                if not result.success:
                    logger.error(f"错误信息: {result.error_message}")
                
                if result.data:
                    logger.info(f"测试数据: {json.dumps(result.data, indent=2, ensure_ascii=False)}")
                    
            except Exception as e:
                logger.error(f"测试 {test_name} 异常: {e}")
                results.append(TestResult(
                    test_name=test_name,
                    success=False,
                    duration_ms=(time.time() - start_time) * 1000,
                    error_message=str(e)
                ))
        
        return results
    
    def generate_report(self, results: List[TestResult], output_file: str = None):
        """生成测试报告"""
        successful_tests = sum(1 for r in results if r.success)
        total_tests = len(results)
        success_rate = successful_tests / total_tests if total_tests > 0 else 0
        
        report = {
            "测试概要": {
                "总测试数": total_tests,
                "成功测试数": successful_tests,
                "失败测试数": total_tests - successful_tests,
                "成功率": f"{success_rate:.2%}",
                "测试时间": datetime.now().isoformat()
            },
            "测试详情": []
        }
        
        for result in results:
            test_detail = {
                "测试名称": result.test_name,
                "状态": "成功" if result.success else "失败",
                "耗时(ms)": f"{result.duration_ms:.2f}",
                "错误信息": result.error_message,
                "测试数据": result.data
            }
            report["测试详情"].append(test_detail)
        
        # 输出到文件
        if output_file:
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(report, f, indent=2, ensure_ascii=False)
            logger.info(f"测试报告已保存到: {output_file}")
        
        # 打印摘要
        print("\n" + "="*60)
        print("分布式缓存数据库测试报告")
        print("="*60)
        print(f"总测试数: {total_tests}")
        print(f"成功: {successful_tests} | 失败: {total_tests - successful_tests}")
        print(f"成功率: {success_rate:.2%}")
        print("="*60)
        
        for result in results:
            status_icon = "✅" if result.success else "❌"
            print(f"{status_icon} {result.test_name}: {result.duration_ms:.2f}ms")
            if not result.success:
                print(f"   错误: {result.error_message}")
        
        print("="*60)

async def main():
    """主函数"""
    parser = argparse.ArgumentParser(description='分布式缓存数据库测试客户端')
    parser.add_argument('--config', '-c', default='test_config.yaml', help='配置文件路径')
    parser.add_argument('--output', '-o', help='测试报告输出文件')
    parser.add_argument('--test', choices=['basic', 'stress', 'cluster', 'all'], default='all', help='测试类型')
    
    args = parser.parse_args()
    
    # 加载配置
    try:
        if os.path.exists(args.config):
            config = TestConfig.from_file(args.config)
        else:
            # 使用默认配置
            config = TestConfig(
                servers=[
                    "http://localhost:8081",
                    "http://localhost:8082", 
                    "http://localhost:8083"
                ],
                load_balancer="http://localhost:8080"
            )
            logger.warning(f"配置文件 {args.config} 不存在，使用默认配置")
    except Exception as e:
        logger.error(f"加载配置文件失败: {e}")
        return
    
    # 创建测试套件
    test_suite = CacheTestSuite(config)
    
    # 运行测试
    if args.test == 'all':
        results = await test_suite.run_all_tests()
    elif args.test == 'basic':
        results = [
            await test_suite.test_basic_operations(),
            await test_suite.test_batch_operations()
        ]
    elif args.test == 'stress':
        results = [await test_suite.stress_test()]
    elif args.test == 'cluster':
        results = [await test_suite.test_cluster_health()]
    
    # 生成报告
    output_file = args.output or f"cache_test_report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
    test_suite.generate_report(results, output_file)

if __name__ == "__main__":
    asyncio.run(main())