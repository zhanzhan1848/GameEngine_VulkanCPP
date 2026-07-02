/**
 * @file TestMetalSyncQuery.cpp
 * @brief Metal 同步和查询功能测试
 * @details 测试 MetalSync 和 MetalQueryPool 的创建、使用和销毁
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 1.0
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalSync.h"
#include "Graphics/RHI/Platforms/Metal/MetalQuery.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// 辅助函数：创建 Metal 设备
static MetalDevice* CreateTestDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    
    MetalDevice* device = new MetalDevice(desc);
    if (!device->Initialize()) {
        delete device;
        return nullptr;
    }
    return device;
}

// 测试同步对象的创建和基本操作
TestResult TestMetalSync() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");
    
    // 1. 创建同步对象
    SyncHandle syncHandle = device->CreateSync();
    TEST_ASSERT(syncHandle != handles::INVALID_SYNC, "Failed to create sync object");
    
    // 2. 获取 MetalSync 对象验证
    MetalSync* sync = device->GetSync(syncHandle);
    TEST_ASSERT(sync != nullptr, "Failed to get MetalSync object");
    TEST_ASSERT(sync->GetNativeEvent() != nullptr, "Native event should not be null");
    
    // 3. 测试值操作
    TEST_ASSERT(sync->GetValue() == 0, "Initial sync value should be 0");
    sync->SetValue(100);
    TEST_ASSERT(sync->GetValue() == 100, "Sync value should be 100 after set");
    
    // 4. 测试等待（模拟）
    // 注意：真正的等待需要在 GPU 上或另一个线程，这里仅测试 API 调用不崩溃
    // 且当前线程等待自己设置的值应该立即返回（如果实现正确的话，或者根据 MTLSharedEvent 行为）
    // CPU 端等待通常是 waitForSyncImpl
    bool waitResult = device->WaitForSync(syncHandle, 100);
    TEST_ASSERT(waitResult, "WaitForSync should return true");
    
    // 5. 销毁
    device->DestroySync(syncHandle);
    // 验证销毁后无法获取（RHIAllocator 应该重置或标记）
    // 注意：RHIAllocator 如果复用 ID，可能会获取到新的或 nullptr，或者 crash 如果没检查。
    // 这里假设 Destroy 后 handle 无效，但实际上 allocator 可能只是标记为 free。
    // 安全起见，我们主要测试 Destroy 调用不崩溃。
    
    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// 测试查询池的创建和基本操作
TestResult TestMetalQueryPool() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");

    // 1. 创建查询池 (Timestamp)
    QueryPoolDesc desc;
    desc.type = QueryType::Timestamp;
    desc.queryCount = 10;

    QueryPoolHandle poolHandle = device->CreateQueryPool(desc);
    TEST_ASSERT(poolHandle != handles::INVALID_QUERY_POOL, "Failed to create query pool");

    // 2. 获取 MetalQueryPool 对象验证
    MetalQueryPool* pool = device->GetQueryPool(poolHandle);
    TEST_ASSERT(pool != nullptr, "Failed to get MetalQueryPool object");

    // 验证内部状态（如果有访问器）
    // 目前 MetalQueryPool 可能没有暴露太多 getter，主要测试创建成功

    // 3. 销毁
    device->DestroyQueryPool(poolHandle);

    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// ============================================================================
// PR3: MetalSync 完善 — 新 API (Signal / Wait / GetSignaledValue) 单元测试
// ============================================================================

// 测试新 API GetSignaledValue / Signal 的基本读写
TestResult TestMetalSync_NewApi_ReadWrite() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");

    SyncHandle h = device->CreateSync();
    TEST_ASSERT(h != handles::INVALID_SYNC, "Failed to create sync");

    MetalSync* sync = device->GetSync(h);
    TEST_ASSERT(sync != nullptr, "Failed to get MetalSync");

    // 初始 signaledValue 应为 0
    TEST_ASSERT(sync->GetSignaledValue() == 0,
                "Initial signaledValue should be 0");

    // Signal 应立即推进 signaledValue
    sync->Signal(42);
    TEST_ASSERT(sync->GetSignaledValue() == 42,
                "After Signal(42), signaledValue should be 42");

    // 老 API GetValue 应与 GetSignaledValue 一致 (deprecated alias)
    TEST_ASSERT(sync->GetValue() == sync->GetSignaledValue(),
                "GetValue should match GetSignaledValue");

    // 老 API SetValue 应与 Signal 等价 (deprecated alias)
    sync->SetValue(100);
    TEST_ASSERT(sync->GetSignaledValue() == 100,
                "After SetValue(100), signaledValue should be 100");

    device->DestroySync(h);
    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// 测试 Wait 的 fast path：等待已 signal 的值应立即返回
TestResult TestMetalSync_Wait_AlreadySignaled() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");

    SyncHandle h = device->CreateSync();
    MetalSync* sync = device->GetSync(h);

    sync->Signal(50);
    // Wait for value <= current — should return true immediately
    auto start = std::chrono::steady_clock::now();
    bool ok = sync->Wait(50, 1000);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    TEST_ASSERT(ok, "Wait(50) after Signal(50) should return true");
    TEST_ASSERT(ms < 50, "Fast-path Wait should return within 50ms");
    (void)ms; // unused if macro doesn't evaluate

    // Wait for value below current should also return immediately
    ok = sync->Wait(10, 1000);
    TEST_ASSERT(ok, "Wait(10) when signaledValue=50 should return true immediately");

    device->DestroySync(h);
    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// 测试 Wait 的 timeout：等待未到达的值,1秒超时应返回 false
TestResult TestMetalSync_Wait_Timeout() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");

    SyncHandle h = device->CreateSync();
    MetalSync* sync = device->GetSync(h);

    sync->Signal(10);
    // Wait for 100 with 200ms timeout — no one signals 100, should time out
    auto start = std::chrono::steady_clock::now();
    bool ok = sync->Wait(100, 200);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    TEST_ASSERT(!ok, "Wait(100) with no signaler should time out (return false)");
    TEST_ASSERT(ms >= 180 && ms < 500,
                "Should wait roughly the 200ms timeout");

    device->DestroySync(h);
    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// 测试 Wait 跨线程链路：另一线程延迟 Signal,主线程 Wait 应阻塞到 fire
TestResult TestMetalSync_Wait_CrossThreadSignal() {
    MetalDevice* device = CreateTestDevice();
    TEST_ASSERT(device != nullptr, "Failed to create device");

    SyncHandle h = device->CreateSync();
    MetalSync* sync = device->GetSync(h);

    sync->Signal(0);

    std::atomic<bool> fired{false};
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        sync->Signal(7);
        fired.store(true);
    });

    // Wait for value=7 (signaled from another thread after 150ms)
    auto start = std::chrono::steady_clock::now();
    bool ok = sync->Wait(7, 2000); // 2s timeout — plenty for 150ms signaler
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    producer.join();

    TEST_ASSERT(ok, "Wait(7) should succeed after cross-thread Signal(7)");
    TEST_ASSERT(fired.load(), "Signaler thread should have run");
    TEST_ASSERT(ms >= 100 && ms < 1000,
                "Should block ~150ms for delayed signaler");

    device->DestroySync(h);
    device->Shutdown();
    delete device;
    return TestResult::Passed;
}

// 注册测试套件
void RegisterMetalSyncQueryTests() {
    auto suite = std::make_shared<TestSuite>("MetalSyncQueryTests");
    suite->AddTestCase(TestCase("MetalSync", TestMetalSync));
    suite->AddTestCase(TestCase("MetalQueryPool", TestMetalQueryPool));
    // PR3: MetalSync 完善 — 新 API 测试
    suite->AddTestCase(TestCase("MetalSync_NewApi_ReadWrite",         TestMetalSync_NewApi_ReadWrite));
    suite->AddTestCase(TestCase("MetalSync_Wait_AlreadySignaled",     TestMetalSync_Wait_AlreadySignaled));
    suite->AddTestCase(TestCase("MetalSync_Wait_Timeout",             TestMetalSync_Wait_Timeout));
    suite->AddTestCase(TestCase("MetalSync_Wait_CrossThreadSignal",   TestMetalSync_Wait_CrossThreadSignal));

    TestRunner::RegisterTestSuite(suite);
}

// 如果作为独立可执行文件运行
#ifndef UNIT_TEST_LIB
int main() {
    RegisterMetalSyncQueryTests();
    TestRunner::RunAllSuites();
    return 0;
}
#endif
