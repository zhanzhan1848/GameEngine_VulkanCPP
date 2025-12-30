/**
 * @file MpscQueuePerformanceTest.cpp
 * @brief MPSC队列性能测试
 * @details 测试基于moodycamel::ConcurrentQueue的MPSC队列性能
 * 
 * @author RHI开发团队
 * @date 2025-12-29
 * @version 2.0
 */

#include "TestFramework.h"
#include "../../Engine/Graphics/RHI/Core/RHIMpscQueue.h"
#include "../../Engine/Graphics/RHI/Core/RHIDevice.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>

using namespace primal::graphics::rhi;

/**
 * @brief MPSC队列性能测试类
 */
class MpscQueuePerformanceTest {
public:
    /**
     * @brief 运行基础性能测试
     */
    static Engine::Test::TestResult RunBasicPerformanceTest() {
        TEST_START_TEST("MPSC队列基础性能测试");
        
        try {
            // 创建虚拟设备
            RHIDevice device("PerformanceTestDevice", RHIAdapterType::Software);
            
            // 获取高性能配置
            auto config = MpscQueueFactory::GetRecommendedConfig("rendering", "high");
            config.maxQueueSize = 10000;
            
            // 创建MPSC队列
            auto queue = MpscQueueFactory::CreateQueue(device, config);
            TEST_ASSERT(queue != nullptr, "MPSC队列创建成功");
            TEST_ASSERT(queue->Start(), "MPSC队列启动成功");
            
            const uint32_t NUM_OPERATIONS = 10000;
            const uint32_t NUM_THREADS = 4;
            
            // 测试数据
            std::vector<std::thread> producers;
            std::atomic<uint32_t> totalEnqueued(0);
            std::atomic<uint32_t> totalProcessed(0);
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // 创建生产者线程
            for (uint32_t t = 0; t < NUM_THREADS; ++t) {
                producers.emplace_back([&, t]() {
                    uint32_t threadEnqueued = 0;
                    uint32_t operationsPerThread = NUM_OPERATIONS / NUM_THREADS;
                    
                    for (uint32_t i = 0; i < operationsPerThread; ++i) {
                        WorkItem item;
                        item.type = WorkItemType::CustomCallback;
                        item.priority = WorkPriority::Normal;
                        item.callbackData.callback = []() { /* 空回调 */ };
                        
                        uint64_t workId = queue->Enqueue(item);
                        if (workId != 0) {
                            threadEnqueued++;
                        }
                        
                        // 模拟一些工作负载
                        if (i % 1000 == 0) {
                            std::this_thread::sleep_for(std::chrono::microseconds(10));
                        }
                    }
                    
                    totalEnqueued.fetch_add(threadEnqueued);
                });
            }
            
            // 等待所有生产者完成
            for (auto& thread : producers) {
                thread.join();
            }
            
            // 等待队列处理完成
            queue->WaitForIdle();
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            
            // 获取统计信息
            auto stats = queue->GetStats();
            
            TEST_LOG("=== 性能测试结果 ===");
            TEST_LOG("总操作数: {}", NUM_OPERATIONS);
            TEST_LOG("成功入队: {}", totalEnqueued.load());
            TEST_LOG("已处理: {}", stats.totalProcessed.load());
            TEST_LOG("已完成: {}", stats.totalCompleted.load());
            TEST_LOG("失败数: {}", stats.totalFailed.load());
            TEST_LOG("处理时间: {} ms", duration.count());
            TEST_LOG("吞吐量: {} ops/s", (stats.totalProcessed.load() * 1000) / duration.count());
            TEST_LOG("平均处理时间: {} μs", stats.totalProcessed.load() > 0 ? 
                    (stats.totalProcessingTime.load() / stats.totalProcessed.load()) : 0);
            
            // 验证结果
            TEST_ASSERT(totalEnqueued.load() == NUM_OPERATIONS, "所有工作项成功入队");
            TEST_ASSERT(stats.totalProcessed.load() == NUM_OPERATIONS, "所有工作项已处理");
            TEST_ASSERT(stats.totalFailed.load() == 0, "没有失败的工作项");
            
            // 性能基准测试
            double throughput = (stats.totalProcessed.load() * 1000.0) / duration.count();
            TEST_LOG("性能评估: 吞吐量 {:.0f} ops/s", throughput);
            
            if (throughput >= 10000) {
                TEST_LOG("✓ 性能优秀 (>10K ops/s)");
            } else if (throughput >= 5000) {
                TEST_LOG("✓ 性能良好 (>5K ops/s)");
            } else {
                TEST_LOG("⚠ 性能有待改进 (<5K ops/s)");
            }
            
            // 清理
            queue->Stop();
            
            TEST_END_TEST();
            
        } catch (const std::exception& e) {
            TEST_FAIL("测试异常: {}", e.what());
            TEST_END_TEST();
        }
    }
    
    /**
     * @brief 运行优先级测试
     */
    static Engine::Test::TestResult RunPriorityTest() {
        TEST_START_TEST("MPSC队列优先级测试");
        
        try {
            RHIDevice device("PriorityTestDevice", RHIAdapterType::Software);
            auto config = MpscQueueFactory::GetRecommendedConfig("rendering", "high");
            config.enablePriorityQueue = true;
            
            auto queue = MpscQueueFactory::CreateQueue(device, config);
            TEST_ASSERT(queue != nullptr, "优先级队列创建成功");
            TEST_ASSERT(queue->Start(), "优先级队列启动成功");
            
            // 测试不同优先级的工作项处理顺序
            std::vector<uint64_t> processedOrder;
            std::mutex orderMutex;
            
            auto trackCallback = [&](WorkPriority priority) {
                return [&]() {
                    std::lock_guard<std::mutex> lock(orderMutex);
                    processedOrder.push_back(static_cast<uint64_t>(priority));
                };
            };
            
            // 添加不同优先级的工作项（故意打乱顺序）
            std::vector<WorkPriority> priorities = {
                WorkPriority::Low,
                WorkPriority::Critical,
                WorkPriority::Normal,
                WorkPriority::High,
                WorkPriority::Low,
                WorkPriority::Critical
            };
            
            for (auto priority : priorities) {
                WorkItem item;
                item.type = WorkItemType::CustomCallback;
                item.priority = priority;
                item.callbackData.callback = trackCallback(priority);
                
                uint64_t workId = queue->Enqueue(item);
                TEST_ASSERT(workId != 0, "工作项入队成功");
            }
            
            // 等待处理完成
            queue->WaitForIdle();
            
            // 验证优先级处理顺序
            TEST_ASSERT(processedOrder.size() == priorities.size(), "所有工作项都被处理");
            
            // 验证Critical优先级的工作项优先被处理
            bool criticalProcessedFirst = true;
            size_t firstNonCriticalIndex = 0;
            
            for (size_t i = 0; i < processedOrder.size(); ++i) {
                if (processedOrder[i] != static_cast<uint64_t>(WorkItemPriority::Critical)) {
                    firstNonCriticalIndex = i;
                    break;
                }
            }
            
            // 检查后面是否还有Critical优先级的工作项
            for (size_t i = firstNonCriticalIndex; i < processedOrder.size(); ++i) {
                if (processedOrder[i] == static_cast<uint64_t>(WorkItemPriority::Critical)) {
                    criticalProcessedFirst = false;
                    break;
                }
            }
            
            TEST_ASSERT(criticalProcessedFirst, "Critical优先级工作项优先被处理");
            
            // 输出处理顺序
            TEST_LOG("工作项处理顺序:");
            for (size_t i = 0; i < processedOrder.size(); ++i) {
                const char* priorityName = "Unknown";
                switch (static_cast<WorkPriority>(processedOrder[i])) {
                    case WorkPriority::Critical: priorityName = "Critical"; break;
                    case WorkPriority::High: priorityName = "High"; break;
                    case WorkPriority::Normal: priorityName = "Normal"; break;
                    case WorkPriority::Low: priorityName = "Low"; break;
                }
                TEST_LOG("  {}. {}", i + 1, priorityName);
            }
            
            queue->Stop();
            TEST_END_TEST();
            
        } catch (const std::exception& e) {
            TEST_FAIL("测试异常: {}", e.what());
            TEST_END_TEST();
        }
    }
    
    /**
     * @brief 运行并发安全性测试
     */
    static Engine::Test::TestResult RunConcurrencyTest() {
        TEST_START_TEST("MPSC队列并发安全性测试");
        
        try {
            RHIDevice device("ConcurrencyTestDevice", RHIAdapterType::Software);
            auto config = MpscQueueFactory::GetRecommendedConfig("rendering", "high");
            
            auto queue = MpscQueueFactory::CreateQueue(device, config);
            TEST_ASSERT(queue != nullptr, "并发测试队列创建成功");
            TEST_ASSERT(queue->Start(), "并发测试队列启动成功");
            
            const uint32_t NUM_PRODUCERS = 8;
            const uint32_t OPERATIONS_PER_PRODUCER = 1000;
            const uint32_t TOTAL_OPERATIONS = NUM_PRODUCERS * OPERATIONS_PER_PRODUCER;
            
            std::vector<std::thread> producers;
            std::atomic<uint32_t> successCount(0);
            std::atomic<uint32_t> failCount(0);
            
            // 创建多个生产者线程
            for (uint32_t t = 0; t < NUM_PRODUCERS; ++t) {
                producers.emplace_back([&, t]() {
                    for (uint32_t i = 0; i < OPERATIONS_PER_PRODUCER; ++i) {
                        WorkItem item;
                        item.type = WorkItemType::CustomCallback;
                        item.priority = static_cast<WorkItemPriority>((t + i) % 4);
                        item.callbackData.callback = []() { /* 空操作 */ };
                        
                        uint64_t workId = queue->Enqueue(item);
                        if (workId != 0) {
                            successCount.fetch_add(1);
                        } else {
                            failCount.fetch_add(1);
                        }
                        
                        // 随机延迟增加并发复杂性
                        if (i % 100 == 0) {
                            std::this_thread::sleep_for(std::chrono::microseconds(1));
                        }
                    }
                });
            }
            
            // 等待所有生产者完成
            for (auto& thread : producers) {
                thread.join();
            }
            
            // 等待队列处理完成
            queue->WaitForIdle();
            
            // 获取最终统计
            auto finalStats = queue->GetStats();
            
            TEST_LOG("=== 并发测试结果 ===");
            TEST_LOG("生产者线程数: {}", NUM_PRODUCERS);
            TEST_LOG("每线程操作数: {}", OPERATIONS_PER_PRODUCER);
            TEST_LOG("预期总操作数: {}", TOTAL_OPERATIONS);
            TEST_LOG("成功入队数: {}", successCount.load());
            TEST_LOG("失败入队数: {}", failCount.load());
            TEST_LOG("已处理数: {}", finalStats.totalProcessed.load());
            TEST_LOG("已完成数: {}", finalStats.totalCompleted.load());
            TEST_LOG("失败数: {}", finalStats.totalFailed.load());
            
            // 验证数据一致性
            uint64_t expectedProcessed = successCount.load() - failCount.load();
            TEST_ASSERT(finalStats.totalProcessed.load() == expectedProcessed, "处理数量与预期一致");
            TEST_ASSERT(finalStats.totalCompleted.load() + finalStats.totalFailed.load() == expectedProcessed, 
                       "完成+失败数量与预期一致");
            
            // 验证队列一致性
            TEST_ASSERT(queue->Validate(), "队列一致性验证通过");
            
            queue->Stop();
            TEST_END_TEST();
            
        } catch (const std::exception& e) {
            TEST_FAIL("测试异常: {}", e.what());
            TEST_END_TEST();
        }
    }
    
    /**
     * @brief 运行所有MPSC队列性能测试
     */
    static Engine::Test::TestResult RunAllPerformanceTests() {
        TEST_START_SUITE("MPSC队列性能测试套件");
        
        auto result1 = RunBasicPerformanceTest();
        auto result2 = RunPriorityTest();
        auto result3 = RunConcurrencyTest();
        
        // 检查是否有测试失败
        bool allPassed = (result1.status == Engine::Test::TestStatus::Passed) &&
                        (result2.status == Engine::Test::TestStatus::Passed) &&
                        (result3.status == Engine::Test::TestStatus::Passed);
        
        Engine::Test::TestResult suiteResult;
        suiteResult.status = allPassed ? Engine::Test::TestStatus::Passed : Engine::Test::TestStatus::Failed;
        suiteResult.message = allPassed ? "所有测试通过" : "部分测试失败";
        
        TEST_END_SUITE();
        return suiteResult;
    }
};

/**
 * @brief MPSC队列性能测试入口函数
 */
void RunMpscQueuePerformanceTests() {
    MpscQueuePerformanceTest::RunAllPerformanceTests();
}