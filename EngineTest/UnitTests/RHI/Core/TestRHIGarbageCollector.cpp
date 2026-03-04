/**
 * @file TestRHIGarbageCollector.cpp
 * @brief RHI垃圾回收器测试
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-09
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIGarbageCollector.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

/**
 * @brief GC测试辅助类，确保RAII方式初始化和清理GC
 */
struct GCHelper {
    RHIGarbageCollector gc;
    GCHelper() { gc.Initialize(); }
    ~GCHelper() { gc.Shutdown(); }
};

// 测试基本延迟销毁
TestResult TestDeferredDestroyBasic() {
    GCHelper helper;
    bool destroyed = false;
    
    helper.gc.SetCurrentFrame(1);
    
    // 提交销毁请求
    helper.gc.DeferredDestroy([&]() {
        destroyed = true;
    });
    
    // 此时不应立即销毁
    TEST_ASSERT(!destroyed, "Should not be destroyed immediately");
    
    // 更新帧，完成第1帧
    helper.gc.Update(1);
    
    // 此时应该已销毁
    TEST_ASSERT(destroyed, "Should be destroyed after update");
    
    return TestResult::Passed;
}

// 测试跨帧销毁
TestResult TestDeferredDestroyCrossFrame() {
    GCHelper helper;
    bool destroyed = false;
    
    helper.gc.SetCurrentFrame(10);
    
    // 在第10帧提交销毁
    helper.gc.DeferredDestroy([&]() {
        destroyed = true;
    });
    
    // 完成第8帧（未达到10）
    helper.gc.Update(8);
    TEST_ASSERT(!destroyed, "Should not destroy at frame 8");
    
    // 完成第9帧（未达到10）
    helper.gc.Update(9);
    TEST_ASSERT(!destroyed, "Should not destroy at frame 9");
    
    // 完成第10帧
    helper.gc.Update(10);
    TEST_ASSERT(destroyed, "Should destroy at frame 10");
    
    return TestResult::Passed;
}

// 测试Flush
TestResult TestFlush() {
    GCHelper helper;
    int destroyCount = 0;
    
    helper.gc.SetCurrentFrame(5);
    
    helper.gc.DeferredDestroy([&]() { destroyCount++; });
    helper.gc.DeferredDestroy([&]() { destroyCount++; });
    
    TEST_ASSERT_EQ(0, destroyCount, "Should be 0 before flush");
    
    // 强制Flush
    helper.gc.Flush();
    
    TEST_ASSERT_EQ(2, destroyCount, "Should be 2 after flush");
    
    return TestResult::Passed;
}

// 测试多线程安全性
TestResult TestThreadSafety() {
    GCHelper helper;
    std::atomic<int> destroyCount{0};
    const int numThreads = 10;
    const int opsPerThread = 1000;
    
    helper.gc.SetCurrentFrame(1);
    
    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < opsPerThread; ++j) {
                helper.gc.DeferredDestroy([&]() {
                    destroyCount++;
                });
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT_EQ(0, destroyCount.load(), "Should be 0 before update");
    
    helper.gc.Update(1);
    
    TEST_ASSERT_EQ(numThreads * opsPerThread, destroyCount.load(), "Should equal total ops");
    
    return TestResult::Passed;
}

// 测试旧帧清理（模拟环形缓冲）
TestResult TestCleanupOldFrames() {
    GCHelper helper;
    bool destroyedFrame1 = false;
    bool destroyedFrame2 = false;
    
    // Frame 1
    helper.gc.SetCurrentFrame(1);
    helper.gc.DeferredDestroy([&]() { destroyedFrame1 = true; });
    
    // Frame 2
    helper.gc.SetCurrentFrame(2);
    helper.gc.DeferredDestroy([&]() { destroyedFrame2 = true; });
    
    // Update completed frame 1 -> should destroy frame 1 resources
    helper.gc.Update(1);
    TEST_ASSERT(destroyedFrame1, "Frame 1 resources should be destroyed");
    TEST_ASSERT(!destroyedFrame2, "Frame 2 resources should NOT be destroyed");
    
    // Update completed frame 2 -> should destroy frame 2 resources
    helper.gc.Update(2);
    TEST_ASSERT(destroyedFrame2, "Frame 2 resources should be destroyed");
    
    return TestResult::Passed;
}

int main() {
    auto suitePtr = std::make_shared<TestSuite>("RHIGarbageCollectorTests");
    TestSuite& suite = *suitePtr;
    
    TEST_CASE(suite, "TestDeferredDestroyBasic", TestDeferredDestroyBasic);
    TEST_CASE(suite, "TestDeferredDestroyCrossFrame", TestDeferredDestroyCrossFrame);
    TEST_CASE(suite, "TestFlush", TestFlush);
    TEST_CASE(suite, "TestThreadSafety", TestThreadSafety);
    TEST_CASE(suite, "TestCleanupOldFrames", TestCleanupOldFrames);
    
    TestRunner::RegisterTestSuite(suitePtr);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
