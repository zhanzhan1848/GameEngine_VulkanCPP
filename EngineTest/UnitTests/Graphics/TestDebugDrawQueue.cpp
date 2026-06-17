// === Phase 5: DebugDrawQueue 单元测试 ===
// 验证 EngineDLL 的 DrawDebugXxx → Engine ForwardSceneRenderer drain 的数据通道。
// 不依赖渲染管线，只测队列本身的线程安全 + 顺序保证 + 清空语义。

#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Graphics/DebugDraw/DebugDrawQueue.h"
#include <thread>
#include <vector>

using namespace Engine::Test;
using primal::graphics::debug_draw::DebugLine;
using primal::graphics::debug_draw::add_line;
using primal::graphics::debug_draw::clear;
using primal::graphics::debug_draw::drain_into;
using primal::graphics::debug_draw::line_count;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

// === Test 1: 单条 line 写入 + drain ===
TestResult TestAddAndDrainOneLine()
{
    clear();
    add_line(1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 0x112233);

    TEST_ASSERT_EQ(static_cast<u32>(1), line_count(), "Queue should hold 1 line");

    std::vector<DebugLine> sink;
    const u32 drained = drain_into(sink);
    TEST_ASSERT_EQ(static_cast<u32>(1), drained, "drain_into should report 1");
    TEST_ASSERT_EQ(static_cast<size_t>(1), sink.size(), "Sink should have 1 element");

    if (!sink.empty()) {
        const DebugLine& l = sink[0];
        TEST_ASSERT(std::abs(1.f - l.a.x) < 0.001f, "a.x should match");
        TEST_ASSERT(std::abs(2.f - l.a.y) < 0.001f, "a.y should match");
        TEST_ASSERT(std::abs(3.f - l.a.z) < 0.001f, "a.z should match");
        TEST_ASSERT(std::abs(4.f - l.b.x) < 0.001f, "b.x should match");
        TEST_ASSERT(std::abs(5.f - l.b.y) < 0.001f, "b.y should match");
        TEST_ASSERT(std::abs(6.f - l.b.z) < 0.001f, "b.z should match");
    }

    // After drain, queue is empty.
    TEST_ASSERT_EQ(static_cast<u32>(0), line_count(), "Queue should be empty post-drain");

    clear();
    return TestResult::Passed;
}

// === Test 2: 多条 line 顺序保持 ===
TestResult TestOrderPreserved()
{
    clear();
    for (u32 i = 0; i < 10; ++i) {
        add_line(static_cast<f32>(i), 0, 0, 0, 0, 0, i);
    }

    std::vector<DebugLine> sink;
    const u32 drained = drain_into(sink);
    TEST_ASSERT_EQ(static_cast<u32>(10), drained, "Should drain 10");

    for (u32 i = 0; i < 10; ++i) {
        if (i >= sink.size()) break;
        TEST_ASSERT(std::abs(static_cast<f32>(i) - sink[i].a.x) < 0.001f,
                    "Order should be FIFO");
    }

    clear();
    return TestResult::Passed;
}

// === Test 3: drain 空队列返回 0 ===
TestResult TestDrainEmpty()
{
    clear();
    std::vector<DebugLine> sink;
    sink.push_back({{1, 1, 1}, {2, 2, 2}, 0}); // pre-populated; drain should NOT touch
    const u32 drained = drain_into(sink);
    TEST_ASSERT_EQ(static_cast<u32>(0), drained, "Empty queue drains 0");
    TEST_ASSERT_EQ(static_cast<size_t>(1), sink.size(),
                   "drain should not push into non-empty sink");
    return TestResult::Passed;
}

// === Test 4: 并发写入线程安全 ===
TestResult TestConcurrentWriters()
{
    clear();
    constexpr u32 THREADS = 4;
    constexpr u32 PER_THREAD = 500;

    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (u32 t = 0; t < THREADS; ++t) {
        workers.emplace_back([t]() {
            for (u32 i = 0; i < PER_THREAD; ++i) {
                add_line(static_cast<f32>(t), static_cast<f32>(i), 0,
                         0, 0, 0, t * 1000 + i);
            }
        });
    }
    for (auto& w : workers) w.join();

    const u32 expected = THREADS * PER_THREAD;
    TEST_ASSERT_EQ(expected, line_count(),
                   "Concurrent writes should all land in the queue");

    std::vector<DebugLine> sink;
    const u32 drained = drain_into(sink);
    TEST_ASSERT_EQ(expected, drained, "Should drain everything");
    TEST_ASSERT_EQ(static_cast<size_t>(expected), sink.size(),
                   "Sink size should match");

    clear();
    return TestResult::Passed;
}

void RunDebugDrawQueueTests()
{
    TestSuite suite("Debug Draw Queue Tests (Phase 5)");
    suite.AddTestCase(TestCase("Add and Drain One Line",
        TestAddAndDrainOneLine,
        "Single line survives the round-trip; queue is empty after drain"));
    suite.AddTestCase(TestCase("Order Preserved",
        TestOrderPreserved,
        "Lines drain in FIFO order"));
    suite.AddTestCase(TestCase("Drain Empty Queue",
        TestDrainEmpty,
        "Draining empty queue returns 0 and doesn't touch sink"));
    suite.AddTestCase(TestCase("Concurrent Writers",
        TestConcurrentWriters,
        "4 threads × 500 lines all land in the queue"));
    suite.RunAllTests();
}

int main()
{
    RunDebugDrawQueueTests();
    return 0;
}
