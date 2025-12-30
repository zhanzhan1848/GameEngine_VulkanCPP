/**
 * @file MpscQueueSimpleTest.cpp
 * @brief MPSC队列简化验证测试
 * @details 验证moodycamel::ConcurrentQueue集成的正确性
 * 
 * @author RHI开发团队
 * @date 2025-12-30
 * @version 1.0
 */

#include "TestFramework.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

// 直接包含moodycamel库进行测试
#include "../../../../third_party/moodycamel-ConcurrentQueue/concurrentqueue.h"

using namespace Engine::Test;

/**
 * @brief 简化的MPSC队列验证测试
 */
class MpscQueueSimpleTest {
public:
    /**
     * @brief 测试基本的并发队列功能
     */
    static TestResult TestBasicConcurrentQueue() {
        START_TEST("moodycamel并发队列基础功能测试");
        
        try {
            // 创建并发队列
            moodycamel::ConcurrentQueue<int> queue;
            
            // 测试入队
            bool enqueueResult = queue.enqueue(42);
            ASSERT(enqueueResult, "元素入队成功");
            
            // 测试出队
            int result;
            bool dequeueResult = queue.try_dequeue(result);
            ASSERT(dequeueResult, "元素出队成功");
            ASSERT(result == 42, "出队元素值正确");
            
            // 测试批量操作
            const int NUM_ELEMENTS = 1000;
            for (int i = 0; i < NUM_ELEMENTS; ++i) {
                queue.enqueue(i);
            }
            
            int dequeuedCount = 0;
            int sum = 0;
            int value;
            while (queue.try_dequeue(value)) {
                dequeuedCount++;
                sum += value;
            }
            
            ASSERT(dequeuedCount == NUM_ELEMENTS, "批量出队数量正确");
            
            // 验证总和 (0 + 1 + ... + 999 = 999 * 1000 / 2 = 499500)
            int expectedSum = (NUM_ELEMENTS - 1) * NUM_ELEMENTS / 2;
            ASSERT(sum == expectedSum, "批量出队元素总和正确");
            
            LOG("=== 基础功能测试结果 ===");
            LOG("入队测试: 通过");
            LOG("出队测试: 通过");  
            LOG("批量操作测试: 通过");
            LOG("处理元素数量: {}", dequeuedCount);
            LOG("元素总和: {} (预期: {})", sum, expectedSum);
            
            END_TEST();
            return TestResult::Passed;
            
        } catch (const std::exception& e) {
            FAIL("测试异常: {}", e.what());
            END_TEST();
            return TestResult::Failed;
        }
    }
    
    /**
     * @brief 测试多生产者单消费者场景
     */
    static TestResult TestMultipleProducers() {
        START_TEST("多生产者单消费者测试");
        
        try {
            moodycamel::ConcurrentQueue<uint64_t> queue;
            const uint32_t NUM_PRODUCERS = 4;
            const uint32_t ITEMS_PER_PRODUCER = 1000;
            const uint32_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
            
            std::atomic<uint32_t> producedCount(0);
            std::atomic<uint32_t> consumedCount(0);
            std::vector<std::thread> producers;
            std::thread consumer;
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // 启动生产者线程
            for (uint32_t p = 0; p < NUM_PRODUCERS; ++p) {
                producers.emplace_back([&, p]() {
                    for (uint32_t i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                        uint64_t item = (static_cast<uint64_t>(p) << 32) | i;
                        if (queue.enqueue(item)) {
                            producedCount.fetch_add(1);
                        }
                        
                        // 轻微延迟模拟真实工作负载
                        if (i % 100 == 0) {
                            std::this_thread::sleep_for(std::chrono::microseconds(1));
                        }
                    }
                });
            }
            
            // 启动消费者线程
            consumer = std::thread([&]() {
                uint64_t item;
                while (consumedCount.load() < TOTAL_ITEMS) {
                    if (queue.try_dequeue(item)) {
                        consumedCount.fetch_add(1);
                    } else {
                        // 队列为空时短暂休眠
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
            
            ASSERT(producedCount.load() == TOTAL_ITEMS, "生产数量正确");
            ASSERT(consumedCount.load() == TOTAL_ITEMS, "消费数量正确");
            ASSERT(queue.size_approx() == 0, "队列已清空");
            
            double throughput = static_cast<double>(TOTAL_ITEMS) / duration.count() * 1000000.0; // items/sec
            
            LOG("=== 多生产者测试结果 ===");
            LOG("生产者数量: {}", NUM_PRODUCERS);
            LOG("每生产者项目数: {}", ITEMS_PER_PRODUCER);
            LOG("总项目数: {}", TOTAL_ITEMS);
            LOG("生产成功数: {}", producedCount.load());
            LOG("消费成功数: {}", consumedCount.load());
            LOG("处理时间: {} 微秒", duration.count());
            LOG("吞吐量: {:.2f} 项目/秒", throughput);
            
            END_TEST();
            return TestResult::Passed;
            
        } catch (const std::exception& e) {
            FAIL("测试异常: {}", e.what());
            END_TEST();
            return TestResult::Failed;
        }
    }
    
    /**
     * @brief 运行所有简化测试
     */
    static void RunAllSimpleTests() {
        START_SUITE("MPSC队列简化验证测试套件");
        
        auto result1 = TestBasicConcurrentQueue();
        auto result2 = TestMultipleProducers();
        
        bool allPassed = (result1.status == TestStatus::Passed) &&
                        (result2.status == TestStatus::Passed);
        
        TestResult suiteResult;
        suiteResult.status = allPassed ? TestStatus::Passed : TestStatus::Failed;
        suiteResult.message = allPassed ? "所有简化测试通过" : "部分简化测试失败";
        
        END_SUITE();
    }
};

/**
 * @brief 简化测试入口函数
 */
void RunMpscQueueSimpleTests() {
    MpscQueueSimpleTest::RunAllSimpleTests();
}

/**
 * @brief 主函数
 */
int main() {
    std::cout << "=== MPSC队列简化验证测试 ===" << std::endl;
    std::cout << "验证moodycamel::ConcurrentQueue集成正确性" << std::endl;
    std::cout << "========================================" << std::endl;
    
    RunMpscQueueSimpleTests();
    
    std::cout << "\n=== 测试完成 ===" << std::endl;
    return 0;
}