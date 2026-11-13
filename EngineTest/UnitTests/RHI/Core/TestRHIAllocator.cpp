#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIAllocator.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

#include "Utilities/MathTypes.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;
using namespace primal::math;

// 测试用的数据结构
struct TestData {
    uint32_t id;
    float value;
    char name[32];
    
    TestData(uint32_t i, float v) : id(i), value(v) {
        memset(name, 0, sizeof(name));
        snprintf(name, sizeof(name), "Data-%d", id);
    }
};

TestResult TestMathTypesCompatibility() {
    RHIAllocator<v4> allocator;
    allocator.Initialize();
    
    // 使用simd::float4 (v4)
    uint32_t id1 = allocator.Allocate(v4{1.0f, 2.0f, 3.0f, 4.0f});
    v4* vec = allocator.Get(id1);
    
    TEST_ASSERT(vec != nullptr, "Should allocate v4");
    TEST_ASSERT_EQ(1.0f, vec->x, "v4.x match");
    TEST_ASSERT_EQ(2.0f, vec->y, "v4.y match");
    TEST_ASSERT_EQ(3.0f, vec->z, "v4.z match");
    TEST_ASSERT_EQ(4.0f, vec->w, "v4.w match");
    
    allocator.Free(id1);
    allocator.Destroy();
    return TestResult::Passed;
}

TestResult TestBasicAllocation() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();
    
    uint32_t id1 = allocator.Allocate(1, 1.0f);
    uint32_t id2 = allocator.Allocate(2, 2.0f);
    
    TEST_ASSERT(id1 != id2, "IDs should be unique");
    
    TestData* p1 = allocator.Get(id1);
    TEST_ASSERT(p1 != nullptr, "Get should return valid pointer");
    TEST_ASSERT_EQ(1u, p1->id, "Data content should match constructor args");
    TEST_ASSERT_EQ(1.0f, p1->value, "Data content should match constructor args");
    
    allocator.Free(id1);
    // p1 现在可能无效，但在 free_list 实现中通常只是标记为删除
    
    // 再次分配，可能会重用 id1
    uint32_t id3 = allocator.Allocate(3, 3.0f);
    TEST_ASSERT(allocator.Get(id3) != nullptr, "Reallocation should succeed");
    
    allocator.Free(id2);
    allocator.Free(id3);

    allocator.Destroy();
    return TestResult::Passed;
}

TestResult TestMultiThreading() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();
    
    const int threadCount = 4;
    const int itemsPerThread = 1000;
    std::atomic<int> successCount{0};
    
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i]() {
            std::vector<uint32_t> ids;
            ids.reserve(itemsPerThread);
            
            // 分配
            for (int j = 0; j < itemsPerThread; ++j) {
                ids.push_back(allocator.Allocate(i * 10000 + j, (float)j));
            }
            
            // 验证
            bool localSuccess = true;
            for (size_t j = 0; j < ids.size(); ++j) {
                TestData* data = allocator.Get(ids[j]);
                if (!data || data->id != (uint32_t)(i * 10000 + j)) {
                    localSuccess = false;
                    break;
                }
            }
            
            // 释放
            for (uint32_t id : ids) {
                allocator.Free(id);
            }
            
            if (localSuccess) successCount++;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT_EQ(threadCount, successCount.load(), "All threads should succeed");
    
    // 验证所有都已释放
    AllocatorStats stats = allocator.GetStats();
    TEST_ASSERT_EQ(0ull, stats.activeAllocations.load(), "Active allocations should be 0");
    
    allocator.Destroy();
    return TestResult::Passed;
}

TestResult TestAllocatorStats() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();
    
    uint32_t id1 = allocator.Allocate(1, 1.0f);
    uint32_t id2 = allocator.Allocate(2, 2.0f);
    (void)id2; // Suppress unused variable warning
    
    AllocatorStats stats = allocator.GetStats();
    TEST_ASSERT_EQ(2ull, stats.activeAllocations.load(), "Should have 2 active allocations");
    TEST_ASSERT_EQ(2ull, stats.totalAllocated.load(), "Should have 2 total allocations");
    
    allocator.Free(id1);
    
    stats = allocator.GetStats();
    TEST_ASSERT_EQ(1ull, stats.activeAllocations.load(), "Should have 1 active allocation");
    TEST_ASSERT_EQ(1ull, stats.totalFreed.load(), "Should have 1 freed");
    
    allocator.Free(id2);

    allocator.Destroy();
    return TestResult::Passed;
}

TestResult TestDefragment() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();
    
    // 分配大量对象
    std::vector<uint32_t> ids;
    for (int i = 0; i < 1000; ++i) {
        ids.push_back(allocator.Allocate(i, 0.0f));
    }
    
    uint32_t capacityBefore = allocator.Capacity();
    TEST_ASSERT(capacityBefore >= 1000, "Capacity should be at least 1000");
    
    // 释放大部分对象
    for (int i = 0; i < 900; ++i) {
        allocator.Free(ids[i]);
    }
    
    // 执行整理
    allocator.Defragment();
    
    // 验证剩余对象仍然有效
    for (int i = 900; i < 1000; ++i) {
        TestData* data = allocator.Get(ids[i]);
        TEST_ASSERT(data != nullptr, "Remaining data should be valid");
        TEST_ASSERT_EQ((uint32_t)i, data->id, "Data ID should match");
        allocator.Free(ids[i]);
    }
    
    allocator.Destroy();
    return TestResult::Passed;
}

TestResult TestShutdownLeak() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();
    
    // 分配一些对象但不释放
    allocator.Allocate(1, 1.0f);
    allocator.Allocate(2, 2.0f);
    allocator.Allocate(3, 3.0f);
    
    AllocatorStats stats = allocator.GetStats();
    TEST_ASSERT_EQ(3ull, stats.activeAllocations.load(), "Should have 3 active allocations");
    
    // 调用 Shutdown
    allocator.Shutdown();
    
    stats = allocator.GetStats();
    TEST_ASSERT_EQ(0ull, stats.activeAllocations.load(), "Active allocations should be 0 after Shutdown");
    
    // Destroy 不应触发断言
    allocator.Destroy();
    
    return TestResult::Passed;
}

// R12-3.4: 句柄代际——槽位回收复用后 generation+1，旧句柄解析为 nullptr
// 而不是槽位新对象（R11 taskC 的 id 别名 hazard 回归测试）。
TestResult TestHandleGeneration() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();

    uint32_t id = allocator.Allocate(1, 1.0f);
    u64 h1 = allocator.MakeHandle(id);
    TEST_ASSERT(h1 != 0, "Fresh slot encodes a non-zero handle (generation starts at 1)");
    TEST_ASSERT_EQ(id, (uint32_t)h1, "Handle low 32 bits are the slot id");
    TEST_ASSERT(allocator.GetChecked(h1) != nullptr, "Fresh handle resolves");
    // 未编码句柄（gen=0）永不匹配活槽位：
    TEST_ASSERT(allocator.GetChecked(static_cast<u64>(id)) == nullptr,
                "Unencoded plain-id handle never resolves");

    allocator.Free(id);
    TEST_ASSERT(allocator.GetChecked(h1) == nullptr, "Freed-slot handle is stale");

    uint32_t id2 = allocator.Allocate(2, 2.0f);
    TEST_ASSERT_EQ(id, id2, "free_list recycles the slot id");
    u64 h2 = allocator.MakeHandle(id2);
    TEST_ASSERT(h2 != h1, "Recycled slot gets a new generation");
    TEST_ASSERT(allocator.GetChecked(h2) != nullptr, "New-generation handle resolves");
    TEST_ASSERT(allocator.GetChecked(h1) == nullptr,
                "Stale handle does not alias the recycled slot's new object");
    TestData* p2 = allocator.GetChecked(h2);
    TEST_ASSERT(p2 != nullptr && p2->id == 2u, "Recycled slot holds the new object");

    allocator.Free(id2);
    allocator.Destroy();
    return TestResult::Passed;
}

// R12-3.4: 延迟回收（retire 队列）——lag 到期前槽位保持存活（基线语义），
// 到期后回收且代际推进；Shutdown 清空 retire 队列不泄漏。
TestResult TestRetireDeferredReclaim() {
    RHIAllocator<TestData> allocator;
    allocator.Initialize();

    uint32_t id = allocator.Allocate(1, 1.0f);
    u64 h = allocator.MakeHandle(id);

    allocator.Retire(id);
    // 重复 Retire 幂等：
    allocator.Retire(id);

    // lag 到期前：槽位存活（与基线「从未释放」期间语义一致）
    TEST_ASSERT(allocator.GetChecked(h) != nullptr, "Retired slot stays alive during the lag");
    allocator.TickRetires(1);
    allocator.TickRetires(2);
    TEST_ASSERT(allocator.GetChecked(h) != nullptr, "Still alive before lag expires");

    allocator.TickRetires(3);  // frame 3 >= retire_frame(0) + kRetireFreeLag(3)
    TEST_ASSERT(allocator.GetChecked(h) == nullptr, "Reclaimed once the lag expires");

    // 回收后：槽位可复用，旧代际句柄保持死亡
    uint32_t id2 = allocator.Allocate(2, 2.0f);
    TEST_ASSERT_EQ(id, id2, "Slot id recycled after reclaim");
    TEST_ASSERT(allocator.GetChecked(allocator.MakeHandle(id2)) != nullptr,
                "Next generation resolves");
    TEST_ASSERT(allocator.GetChecked(h) == nullptr, "Old-generation handle stays dead");

    // Retire 后又显式 Free：tick 侧 is_valid 复查跳过，不二次释放
    uint32_t id3 = allocator.Allocate(3, 3.0f);
    allocator.Retire(id3);
    allocator.Free(id3);
    allocator.TickRetires(4);
    AllocatorStats stats = allocator.GetStats();
    TEST_ASSERT_EQ(1ull, stats.activeAllocations.load(),
                   "Retire+Free double path releases exactly once");

    allocator.Shutdown();
    return TestResult::Passed;
}

int main() {
    TestSuite suite("RHIAllocatorTests");
    suite.AddTestCase(TestCase("BasicAllocation", TestBasicAllocation, "Test basic allocate and free"));
    suite.AddTestCase(TestCase("MultiThreading", TestMultiThreading, "Test thread safety"));
    suite.AddTestCase(TestCase("Stats", TestAllocatorStats, "Test statistics"));
    suite.AddTestCase(TestCase("Defragment", TestDefragment, "Test defragmentation interface"));
    suite.AddTestCase(TestCase("MathCompatibility", TestMathTypesCompatibility, "Test math types compatibility"));
    suite.AddTestCase(TestCase("ShutdownLeak", TestShutdownLeak, "Test Shutdown mechanism"));
    suite.AddTestCase(TestCase("HandleGeneration", TestHandleGeneration, "R12-3.4 handle generation: stale handles never alias recycled slots"));
    suite.AddTestCase(TestCase("RetireDeferredReclaim", TestRetireDeferredReclaim, "R12-3.4 retire queue: deferred reclaim + generation bump"));

    TestStats stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
