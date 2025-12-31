/**
 * @file MpscQueueSimpleTest.cpp
 * @brief MPSC队列简化验证测试
 * @details 验证moodycamel::ConcurrentQueue集成的正确性，符合技术规范要求
 * 
 * @author RHI开发团队
 * @date 2025-12-30
 * @version 2.0
 */

#include "Engine/Common/CommonHeaders.h"

// 直接包含moodycamel库进行测试
#include "concurrentqueue.h"

// === 测试框架定义 ===

/**
 * @brief 测试输出句柄
 * @details 使用C标准IO，完全避免iostream依赖
 */
static FILE* g_testOutput = nullptr;

/**
 * @brief 初始化测试输出
 */
static bool InitTestOutput(const char* filename) {
    if (!filename) {
        g_testOutput = stdout;
        return true;
    }
    g_testOutput = fopen(filename, "w");
    return g_testOutput != nullptr;
}

/**
 * @brief 关闭测试输出
 */
static void CloseTestOutput() {
    if (g_testOutput && g_testOutput != stdout) {
        fclose(g_testOutput);
        g_testOutput = nullptr;
    }
}

/**
 * @brief 测试断言宏（相等检查）
 */
#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s (expected: %llu, actual: %llu)\n", \
                        msg, static_cast<unsigned long long>(b), static_cast<unsigned long long>(a)); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

/**
 * @brief 测试断言宏（真值检查）
 */
#define TEST_ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s\n", msg); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

/**
 * @brief 输出测试标题
 */
#define TEST_SECTION(title) \
    do { \
        if (g_testOutput) { \
            fprintf(g_testOutput, "=== %s ===\n", title); \
            fflush(g_testOutput); \
        } \
    } while(0)

/**
 * @brief 简单调试输出宏
 */
#define DEBUG_TRACE(msg, ...) \
    do { \
        if (g_testOutput) { \
            fprintf(g_testOutput, "[TRACE] " msg "\n", ##__VA_ARGS__); \
            fflush(g_testOutput); \
        } \
    } while(0)

#define DEBUG_ERROR(msg, ...) \
    do { \
        if (g_testOutput) { \
            fprintf(g_testOutput, "[ERROR] " msg "\n", ##__VA_ARGS__); \
            fflush(g_testOutput); \
        } \
    } while(0)

/**
 * @brief 简化的MPSC队列验证测试
 */
class MpscQueueSimpleTest {
public:
    /**
     * @brief 测试基本的并发队列功能
     */
    static bool TestBasicConcurrentQueue() {
        TEST_SECTION("moodycamel并发队列基础功能测试");
        
        try {
            // 创建并发队列
            moodycamel::ConcurrentQueue<int> queue;
            
            // 测试入队
            bool enqueueResult = queue.enqueue(42);
            if (!enqueueResult) {
                DEBUG_ERROR("元素入队失败");
                return false;
            }
            DEBUG_TRACE("元素入队成功");
            
            // 测试出队
            int result;
            bool dequeueResult = queue.try_dequeue(result);
            if (!dequeueResult) {
                DEBUG_ERROR("元素出队失败");
                return false;
            }
            if (result != 42) {
                DEBUG_ERROR("出队元素值错误: %d (预期: 42)", result);
                return false;
            }
            DEBUG_TRACE("元素出队成功，值: %d", result);
            
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
            
            if (dequeuedCount != NUM_ELEMENTS) {
                DEBUG_ERROR("批量出队数量错误: %d (预期: %d)", dequeuedCount, NUM_ELEMENTS);
                return false;
            }
            DEBUG_TRACE("批量出队数量正确: %d", dequeuedCount);
            
            // 验证总和 (0 + 1 + ... + 999 = 999 * 1000 / 2 = 499500)
            int expectedSum = (NUM_ELEMENTS - 1) * NUM_ELEMENTS / 2;
            if (sum != expectedSum) {
                DEBUG_ERROR("批量出队元素总和错误: %d (预期: %d)", sum, expectedSum);
                return false;
            }
            DEBUG_TRACE("批量出队元素总和正确: %d", sum);
            
            DEBUG_TRACE("入队测试: 通过");
            DEBUG_TRACE("出队测试: 通过");  
            DEBUG_TRACE("批量操作测试: 通过");
            DEBUG_TRACE("处理元素数量: %d", dequeuedCount);
            DEBUG_TRACE("元素总和: %d (预期: %d)", sum, expectedSum);
            
            DEBUG_TRACE("测试通过");
            return true;
            
        } catch (...) {
            DEBUG_ERROR("测试异常发生");
            return false;
        }
    }
    
    /**
     * @brief 测试多生产者单消费者场景
     */
    static bool TestMultipleProducers() {
        TEST_SECTION("多生产者单消费者测试");
        
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
            for (uint32_t i = 0; i < producers.size(); ++i) {
                producers[i].join();
            }
            
            // 等待消费者完成
            consumer.join();
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            if (producedCount.load() != TOTAL_ITEMS) {
                DEBUG_ERROR("生产数量错误: %u (预期: %u)", producedCount.load(), TOTAL_ITEMS);
                return false;
            }
            if (consumedCount.load() != TOTAL_ITEMS) {
                DEBUG_ERROR("消费数量错误: %u (预期: %u)", consumedCount.load(), TOTAL_ITEMS);
                return false;
            }
            if (queue.size_approx() != 0) {
                DEBUG_ERROR("队列未清空，剩余元素: %zu", queue.size_approx());
                return false;
            }
            
            double throughput = static_cast<double>(TOTAL_ITEMS) / duration.count() * 1000000.0; // items/sec
            
            DEBUG_TRACE("=== 多生产者测试结果 ===");
            DEBUG_TRACE("生产者数量: %u", NUM_PRODUCERS);
            DEBUG_TRACE("每生产者项目数: %u", ITEMS_PER_PRODUCER);
            DEBUG_TRACE("总项目数: %u", TOTAL_ITEMS);
            DEBUG_TRACE("生产成功数: %u", producedCount.load());
            DEBUG_TRACE("消费成功数: %u", consumedCount.load());
            DEBUG_TRACE("处理时间: %lld 微秒", duration.count());
            DEBUG_TRACE("吞吐量: %.0f 项目/秒", throughput);
            DEBUG_TRACE("测试通过");
            return true;
            
        } catch (...) {
            DEBUG_ERROR("测试异常发生");
            return false;
        }
    }
    
    /**
     * @brief 运行所有简化测试
     */
    static void RunAllSimpleTests() {
        DEBUG_TRACE("=== MPSC队列简化验证测试套件 ===");
        
        auto result1 = TestBasicConcurrentQueue();
        auto result2 = TestMultipleProducers();
        
        bool allPassed = result1 && result2;
        
        DEBUG_TRACE("测试套件结果: %s", (allPassed ? "通过" : "失败"));
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
    DEBUG_TRACE("=== MPSC队列简化验证测试 ===");
    DEBUG_TRACE("验证moodycamel::ConcurrentQueue集成正确性");
    DEBUG_TRACE("========================================");
    
    // 初始化测试输出
    InitTestOutput("mpsc_queue_test_output.txt");
    
    RunMpscQueueSimpleTests();
    
    // 关闭测试输出
    CloseTestOutput();
    
    DEBUG_TRACE("=== 测试完成 ===");
    return 0;
}