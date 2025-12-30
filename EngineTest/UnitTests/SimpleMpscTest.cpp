/**
 * @file SimpleMpscTest.cpp
 * @brief MPSC队列最简验证测试
 * @details 直接测试moodycamel::ConcurrentQueue基础功能
 * 
 * @author RHI开发团队
 * @date 2025-12-30
 * @version 1.0
 */

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cassert>

// 直接包含moodycamel库
#include "third_party/moodycamel-ConcurrentQueue/concurrentqueue.h"

/**
 * @brief 简单测试结果
 */
struct TestResult {
    bool passed;
    std::string message;
    
    TestResult(bool p, const std::string& msg) : passed(p), message(msg) {}
};

/**
 * @brief 简化的MPSC队列测试类
 */
class SimpleMpscTest {
public:
    /**
     * @brief 测试基础入队出队功能
     */
    static TestResult TestBasicOperations() {
        std::cout << "\n=== 测试基础入队出队功能 ===" << std::endl;
        
        try {
            moodycamel::ConcurrentQueue<int> queue;
            
            // 测试单元素入队出队
            bool enqueueSuccess = queue.enqueue(42);
            if (!enqueueSuccess) {
                return TestResult(false, "单元素入队失败");
            }
            
            int result;
            bool dequeueSuccess = queue.try_dequeue(result);
            if (!dequeueSuccess) {
                return TestResult(false, "单元素出队失败");
            }
            
            if (result != 42) {
                return TestResult(false, "出队元素值不正确，期望42，实际" + std::to_string(result));
            }
            
            std::cout << "✓ 单元素入队出队测试通过" << std::endl;
            
            // 测试批量操作
            const int BATCH_SIZE = 10000;
            for (int i = 0; i < BATCH_SIZE; ++i) {
                queue.enqueue(i);
            }
            
            int dequeuedCount = 0;
            long long sum = 0;
            int value;
            while (queue.try_dequeue(value)) {
                dequeuedCount++;
                sum += value;
            }
            
            if (dequeuedCount != BATCH_SIZE) {
                return TestResult(false, "批量出队数量不正确，期望" + std::to_string(BATCH_SIZE) + 
                                 "，实际" + std::to_string(dequeuedCount));
            }
            
            // 验证总和公式: 0 + 1 + ... + (n-1) = n*(n-1)/2
            long long expectedSum = static_cast<long long>(BATCH_SIZE) * (BATCH_SIZE - 1) / 2;
            if (sum != expectedSum) {
                return TestResult(false, "批量出队元素总和不正确，期望" + std::to_string(expectedSum) + 
                                 "，实际" + std::to_string(sum));
            }
            
            std::cout << "✓ 批量操作测试通过，处理" << dequeuedCount << "个元素" << std::endl;
            std::cout << "✓ 元素总和验证通过: " << sum << std::endl;
            
            return TestResult(true, "基础操作测试全部通过");
            
        } catch (const std::exception& e) {
            return TestResult(false, std::string("测试异常: ") + e.what());
        }
    }
    
    /**
     * @brief 测试多生产者单消费者场景
     */
    static TestResult TestMultipleProducers() {
        std::cout << "\n=== 测试多生产者单消费者场景 ===" << std::endl;
        
        try {
            moodycamel::ConcurrentQueue<uint64_t> queue;
            const uint32_t NUM_PRODUCERS = 8;
            const uint32_t ITEMS_PER_PRODUCER = 1000;
            const uint32_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
            
            std::atomic<uint32_t> producedCount(0);
            std::atomic<uint32_t> consumedCount(0);
            std::vector<std::thread> producers;
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // 启动生产者线程
            for (uint32_t p = 0; p < NUM_PRODUCERS; ++p) {
                producers.emplace_back([&, p]() {
                    uint64_t producerId = p;
                    for (uint32_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                        // 构造唯一ID: 高32位是生产者ID，低32位是序列号
                        uint64_t item = (producerId << 32) | i;
                        if (queue.enqueue(item)) {
                            producedCount.fetch_add(1);
                        }
                        
                        // 每100个项目休眠1微秒，模拟真实工作负载
                        if (i % 100 == 0) {
                            std::this_thread::sleep_for(std::chrono::microseconds(1));
                        }
                    }
                });
            }
            
            // 主线程作为消费者
            std::thread consumer([&]() {
                uint64_t item;
                while (consumedCount.load() < TOTAL_ITEMS) {
                    if (queue.try_dequeue(item)) {
                        consumedCount.fetch_add(1);
                    } else {
                        // 队列为空时短暂休眠，避免CPU空转
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }
                }
            });
            
            // 等待所有生产者完成
            for (auto& producer : producers) {
                producer.join();
            }
            
            // 等待消费者完成
            consumer.join();
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            // 验证结果
            if (producedCount.load() != TOTAL_ITEMS) {
                return TestResult(false, "生产数量不正确，期望" + std::to_string(TOTAL_ITEMS) + 
                                 "，实际" + std::to_string(producedCount.load()));
            }
            
            if (consumedCount.load() != TOTAL_ITEMS) {
                return TestResult(false, "消费数量不正确，期望" + std::to_string(TOTAL_ITEMS) + 
                                 "，实际" + std::to_string(consumedCount.load()));
            }
            
            if (queue.size_approx() != 0) {
                return TestResult(false, "队列未完全清空，剩余约" + std::to_string(queue.size_approx()) + "个元素");
            }
            
            // 计算性能指标
            double throughput = static_cast<double>(TOTAL_ITEMS) / duration.count() * 1000000.0; // items/sec
            double avgLatency = static_cast<double>(duration.count()) / TOTAL_ITEMS; // microseconds
            
            std::cout << "✓ 多生产者测试通过" << std::endl;
            std::cout << "  生产者数量: " << NUM_PRODUCERS << std::endl;
            std::cout << "  每生产者项目数: " << ITEMS_PER_PRODUCER << std::endl;
            std::cout << "  总项目数: " << TOTAL_ITEMS << std::endl;
            std::cout << "  生产成功数: " << producedCount.load() << std::endl;
            std::cout << "  消费成功数: " << consumedCount.load() << std::endl;
            std::cout << "  处理时间: " << duration.count() << " 微秒" << std::endl;
            std::cout << "  吞吐量: " << std::fixed << std::setprecision(2) << throughput << " 项目/秒" << std::endl;
            std::cout << "  平均延迟: " << std::fixed << std::setprecision(2) << avgLatency << " 微秒/项目" << std::endl;
            
            return TestResult(true, "多生产者测试通过，吞吐量: " + std::to_string(throughput) + " items/sec");
            
        } catch (const std::exception& e) {
            return TestResult(false, std::string("测试异常: ") + e.what());
        }
    }
    
    /**
     * @brief 运行所有测试
     */
    static void RunAllTests() {
        std::cout << "========================================" << std::endl;
        std::cout << "    MPSC队列简化验证测试" << std::endl;
        std::cout << "    验证moodycamel::ConcurrentQueue集成" << std::endl;
        std::cout << "========================================" << std::endl;
        
        std::vector<TestResult> results;
        
        // 运行基础测试
        results.push_back(TestBasicOperations());
        
        // 运行多生产者测试
        results.push_back(TestMultipleProducers());
        
        // 汇总结果
        std::cout << "\n========================================" << std::endl;
        std::cout << "           测试结果汇总" << std::endl;
        std::cout << "========================================" << std::endl;
        
        int passedCount = 0;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& result = results[i];
            std::cout << (i + 1) << ". " << (result.passed ? "✅ 通过" : "❌ 失败") << std::endl;
            if (!result.passed) {
                std::cout << "   错误: " << result.message << std::endl;
            }
            if (result.passed) {
                passedCount++;
            }
        }
        
        std::cout << "\n总体结果: " << passedCount << "/" << results.size() << " 个测试通过" << std::endl;
        
        if (passedCount == results.size()) {
            std::cout << "🎉 所有测试通过！MPSC队列集成验证成功！" << std::endl;
        } else {
            std::cout << "⚠️  部分测试失败，请检查实现。" << std::endl;
        }
        
        std::cout << "========================================" << std::endl;
    }
};

/**
 * @brief 主函数
 */
int main() {
    SimpleMpscTest::RunAllTests();
    return 0;
}