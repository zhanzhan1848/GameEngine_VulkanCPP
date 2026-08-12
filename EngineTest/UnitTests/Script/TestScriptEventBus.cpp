// === Phase 1 Task 5: Cross-Script Event Bus 测试 ===
//
// 验证 script_event_bus 的核心契约:
//   1) subscribe + emit + drain:handler 收到正确的 event payload
//   2) FIFO 顺序:多个 emit 按 emission order 投递
//   3) 无订阅者 emit 不 crash
//   4) Re-emit isolation:handler 内部 emit 的事件进 re_emitted_ 队列,
//      在当前 pass 不投递,而在下一 pass 投递。depth_cap=8 防止无限循环。
//   5) Deferred subscribe:drain 进行中 subscribe 的事件不立即生效,
//      当前 pass 不投递给新订阅者;drain 结束后才生效,后续 emit 能投递。
//
// 设计说明:
//   - 所有测试都用 nullptr owner(owner-less subscription),代表系统级广播。
//   - 测试之间用 bus.reset() 清空 state,避免跨测试污染。
//   - handler 用 std::shared_ptr<int> capture,避免 const_cast<int&> 的 UB。

#include "../TestFramework.h"
#include "Components/ScriptEventBus.h"
#include "Components/Script.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"

#include <memory>
#include <vector>
#include <cstdio>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

namespace {

// 测试用事件 — trivially copyable POD
struct TestEvent {
    int value;
};

// 另一个事件类型,用于验证 type isolation
struct OtherEvent {
    int payload;
};

// ----------------------------------------------------------------------------
// Test 1: subscribe + emit + drain → handler 收到 payload
// ----------------------------------------------------------------------------
TestResult test_subscribe_emit_basic() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    auto received = std::make_shared<int>(0);
    auto sub_id = bus.subscribe<TestEvent>(nullptr,
        [received](primal::script::entity_script*, const TestEvent& e) {
            *received = e.value;
        });

    bus.emit<TestEvent>({42});
    primal::script::drain_events();

    bus.unsubscribe(sub_id);
    primal::script::shutdown();

    return *received == 42 ? TestResult::Passed : TestResult::Failed;
}

// ----------------------------------------------------------------------------
// Test 2: FIFO order — 3 events emitted in order arrive in order
// ----------------------------------------------------------------------------
TestResult test_fifo_order_preserved() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    auto received_order = std::make_shared<std::vector<int>>();
    auto sub_id = bus.subscribe<TestEvent>(nullptr,
        [received_order](primal::script::entity_script*, const TestEvent& e) {
            received_order->push_back(e.value);
        });

    bus.emit<TestEvent>({1});
    bus.emit<TestEvent>({2});
    bus.emit<TestEvent>({3});
    primal::script::drain_events();

    bus.unsubscribe(sub_id);
    primal::script::shutdown();

    const std::vector<int> expected{1, 2, 3};
    return (*received_order == expected) ? TestResult::Passed : TestResult::Failed;
}

// ----------------------------------------------------------------------------
// Test 3: emit with no subscribers → drain does not crash
// ----------------------------------------------------------------------------
TestResult test_no_loss_with_no_subscribers() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    // No subscribers — drain must handle empty bucket gracefully.
    bus.emit<TestEvent>({999});
    primal::script::drain_events();

    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 4: Re-emit isolation
//
// Handler 内部 emit 的事件应进 re_emitted_ 队列,在当前 pass 结束后才投递。
// drain 会自动再 drain 一次 re_emitted_ 内容,所以单次 drain_events() 调用
// 应该能让 re-emitted 事件也被投递(在内部第二 pass)。
//
// 同时验证:真正无限循环(handler 永远 re-emit)会被 depth_cap=8 阻断,
// 表现为 assert 失败。但为了测试不 abort,我们用一个计数 handler
// 在第 N 次后停止 re-emit。
// ----------------------------------------------------------------------------
TestResult test_reemit_isolation() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    // 直接调用 bus.drain() 测试 — 因为 drain_events() 走 frame_tick
    // 也会做同样的事,但直接测 bus 更精确。
    auto primary_received = std::make_shared<int>(0);
    auto reemit_received = std::make_shared<int>(0);

    auto sub_id = bus.subscribe<TestEvent>(nullptr,
        [primary_received, reemit_received, &bus]
        (primal::script::entity_script*, const TestEvent& e) {
            // 第一个事件(value=1)触发 re-emit
            if (e.value == 1) {
                ++*primary_received;
                bus.emit<TestEvent>({100});  // re-emit,进 re_emitted_
            } else if (e.value == 100) {
                ++*reemit_received;
            }
        });

    bus.emit<TestEvent>({1});
    bus.drain();  // 单次 drain 应该消耗 queue_ 和 re_emitted_(内部多 pass)

    bus.unsubscribe(sub_id);
    primal::script::shutdown();

    // primary 应该被调用 1 次(原事件),reemit 应该被调用 1 次(re-emitted 事件)
    if (*primary_received != 1) return TestResult::Failed;
    if (*reemit_received != 1) return TestResult::Failed;
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 4b: depth cap — handler 永远 re-emit 会触发 depth_cap assert
//
// 这个测试验证 depth cap 生效。由于 assert 在 Release 模式会编译掉,
// 我们只能在 Debug 验证。为了避免测试 abort,我们用一个有限循环
// (re-emit 10 次然后停),验证即使超过 depth_cap=8 也不会无限循环
// (cap 之后 drain 返回,剩余 re_emitted_ 被丢弃)。
//
// 注意:这个测试依赖 NDEBUG。在 Debug 模式下,assert 会 abort,
// 所以我们不在 Debug 模式跑这个无限循环 case;只用有限循环验证
// 多次 re-emit 在 cap 内能正常工作。
// ----------------------------------------------------------------------------
TestResult test_reemit_depth_cap_within_bound() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    auto call_count = std::make_shared<int>(0);
    auto sub_id = bus.subscribe<TestEvent>(nullptr,
        [call_count, &bus]
        (primal::script::entity_script*, const TestEvent& e) {
            ++*call_count;
            // Re-emit 5 次(value 递增),第 6 次后停止
            if (e.value < 5) {
                bus.emit<TestEvent>({e.value + 1});
            }
        });

    bus.emit<TestEvent>({0});
    bus.drain();

    bus.unsubscribe(sub_id);
    primal::script::shutdown();

    // value 0,1,2,3,4,5 → 6 次调用,6 个事件全部投递(depth 远低于 cap)
    // (value 0 是初始,1-5 是 re-emit)
    if (*call_count != 6) {
        std::fprintf(stderr, "Expected 6 calls, got %d\n", *call_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 5: Deferred subscribe during drain
//
// drain 进行中 subscribe 的新订阅:
//   - 不应收到当前正在 drain 的事件(避免在 drain 中途加入桶)
//   - 应在 drain 结束后被加入,后续 emit 能正常投递
// ----------------------------------------------------------------------------
TestResult test_deferred_subscribe_during_drain() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    // 先订阅一个"触发器",handler 内部 subscribe 第二个
    auto primary_count = std::make_shared<int>(0);
    auto secondary_received = std::make_shared<int>(0);

    // 把 secondary sub id 存起来,以便后面 unsubscribe
    auto secondary_sub_id = std::make_shared<primal::script::subscription_id_t>(0);

    auto primary_sub_id = bus.subscribe<TestEvent>(nullptr,
        [primary_count, secondary_received, secondary_sub_id, &bus]
        (primal::script::entity_script*, const TestEvent& e) {
            ++*primary_count;
            // 第一次收到事件时,subscribe 一个新的 secondary handler
            if (e.value == 1 && *secondary_sub_id == 0) {
                *secondary_sub_id = bus.subscribe<TestEvent>(nullptr,
                    [secondary_received]
                    (primal::script::entity_script*, const TestEvent& ev) {
                        *secondary_received = ev.value;
                    });
            }
        });

    // emit event 1 — primary handler 会 subscribe secondary
    bus.emit<TestEvent>({1});
    bus.drain();

    // At this point: secondary 应该 NOT 收到 event 1(因为 deferred)
    if (*secondary_received != 0) {
        bus.unsubscribe(primary_sub_id);
        if (*secondary_sub_id != 0) bus.unsubscribe(*secondary_sub_id);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Now emit event 2 — secondary 应该收到(deferred subscribe 已 apply)
    bus.emit<TestEvent>({42});
    bus.drain();

    bus.unsubscribe(primary_sub_id);
    if (*secondary_sub_id != 0) bus.unsubscribe(*secondary_sub_id);
    primal::script::shutdown();

    return (*secondary_received == 42) ? TestResult::Passed : TestResult::Failed;
}

// ----------------------------------------------------------------------------
// Helper: create a minimal entity_script instance for owner-based tests.
//
// entity_script requires an entity handle for construction. We create a
// throwaway entity + minimal script subclass. The raw pointer is used as
// an owner identity for subscribe/unsubscribe_all. Cleanup is manual.
// ----------------------------------------------------------------------------
namespace {

class stub_script : public primal::script::entity_script {
public:
    explicit stub_script(primal::game_entity::entity e)
        : primal::script::entity_script(e) {}
};

std::unique_ptr<stub_script> make_stub_owner() {
    primal::transform::init_info tinfo{};
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    primal::game_entity::entity e = primal::game_entity::create(info);
    return std::make_unique<stub_script>(e);
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// Test 6 (I3): unsubscribe_all(owner) removes all subscriptions for the owner
//
// Owner A subscribes to E1 and E2. Owner B subscribes to E1 only.
// After unsubscribe_all(A), emitting E1 delivers only to B, and emitting
// E2 delivers to no one. This is the API that script::remove() depends on
// for the I1 use-after-free fix.
// ----------------------------------------------------------------------------
TestResult test_unsubscribe_all_by_owner() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    auto owner_a = make_stub_owner();
    auto owner_b = make_stub_owner();

    auto a_e1_received = std::make_shared<int>(0);
    auto a_e2_received = std::make_shared<int>(0);
    auto b_e1_received = std::make_shared<int>(0);

    bus.subscribe<TestEvent>(owner_a.get(),
        [a_e1_received](primal::script::entity_script*, const TestEvent& e) {
            *a_e1_received = e.value;
        });
    bus.subscribe<OtherEvent>(owner_a.get(),
        [a_e2_received](primal::script::entity_script*, const OtherEvent& e) {
            *a_e2_received = e.payload;
        });
    bus.subscribe<TestEvent>(owner_b.get(),
        [b_e1_received](primal::script::entity_script*, const TestEvent& e) {
            *b_e1_received = e.value;
        });

    // Unsubscribe all of owner A's subscriptions
    bus.unsubscribe_all(owner_a.get());

    // Emit E1 — only B should receive
    bus.emit<TestEvent>({10});
    bus.drain();

    // Emit E2 — no one should receive
    bus.emit<OtherEvent>({20});
    bus.drain();

    primal::script::shutdown();

    if (*a_e1_received != 0) {
        std::fprintf(stderr, "Owner A received E1 after unsubscribe_all\n");
        return TestResult::Failed;
    }
    if (*a_e2_received != 0) {
        std::fprintf(stderr, "Owner A received E2 after unsubscribe_all\n");
        return TestResult::Failed;
    }
    if (*b_e1_received != 10) {
        std::fprintf(stderr, "Owner B did not receive E1 (expected 10, got %d)\n", *b_e1_received);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 7 (I2): Deferred unsubscribe during drain
//
// Owner A and Owner B are both subscribed to E. Both receive event {1} during
// drain. Inside B's handler, we call bus.unsubscribe_all(B). Since we are
// mid-drain, this goes through the deferred path. After drain completes,
// we emit E again and verify only A receives it — B was unsubscribed.
//
// This mirrors test_deferred_subscribe_during_drain and covers the symmetric
// deferred unsubscribe path. Without this test, the deferred unsubscribe
// code path could break without detection.
// ----------------------------------------------------------------------------
TestResult test_deferred_unsubscribe_during_drain() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    auto owner_a = make_stub_owner();
    auto owner_b = make_stub_owner();

    auto a_count = std::make_shared<int>(0);
    auto b_count = std::make_shared<int>(0);

    bus.subscribe<TestEvent>(owner_a.get(),
        [a_count](primal::script::entity_script*, const TestEvent&) {
            ++*a_count;
        });

    bus.subscribe<TestEvent>(owner_b.get(),
        [b_count, &bus, owner_b_ptr = owner_b.get()]
        (primal::script::entity_script*, const TestEvent&) {
            ++*b_count;
            // Unsubscribe self during drain. Since draining_ == true, this
            // goes through deferred_unsubscribe_owners_ and is applied after
            // drain completes.
            bus.unsubscribe_all(owner_b_ptr);
        });

    // Emit event 1 — both A and B receive it during drain.
    // B unsubscribes itself (deferred).
    bus.emit<TestEvent>({1});
    bus.drain();

    // A was called once, B was called once.
    if (*a_count != 1 || *b_count != 1) {
        std::fprintf(stderr, "After first drain: a_count=%d (expected 1), b_count=%d (expected 1)\n",
                     *a_count, *b_count);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Emit event 2 — only A should receive it (B was unsubscribed).
    bus.emit<TestEvent>({2});
    bus.drain();

    primal::script::shutdown();

    if (*a_count != 2) {
        std::fprintf(stderr, "A should have been called 2 times total, got %d\n", *a_count);
        return TestResult::Failed;
    }
    if (*b_count != 1) {
        std::fprintf(stderr, "B should not receive events after deferred unsubscribe, got %d extra calls\n",
                     *b_count - 1);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

} // anonymous namespace

void RunScriptEventBusTests() {
    TestSuite suite("Script.EventBus");
    suite.AddTestCase(TestCase("subscribe_emit_basic",
                               test_subscribe_emit_basic,
                               "subscribe + emit + drain delivers payload to handler"));
    suite.AddTestCase(TestCase("fifo_order_preserved",
                               test_fifo_order_preserved,
                               "3 events emitted in order arrive at handler in FIFO order"));
    suite.AddTestCase(TestCase("no_loss_with_no_subscribers",
                               test_no_loss_with_no_subscribers,
                               "emit with no subscribers does not crash on drain"));
    suite.AddTestCase(TestCase("reemit_isolation",
                               test_reemit_isolation,
                               "handler-emitted event goes to re_emitted_ queue, "
                               "delivered in subsequent internal pass within same drain()"));
    suite.AddTestCase(TestCase("reemit_depth_cap_within_bound",
                               test_reemit_depth_cap_within_bound,
                               "5-level re-emit chain (depth < cap=8) drains cleanly; "
                               "all 6 events delivered"));
    suite.AddTestCase(TestCase("deferred_subscribe_during_drain",
                               test_deferred_subscribe_during_drain,
                               "subscribe() called from within a handler is deferred: "
                               "does not receive currently-draining event, "
                               "does receive subsequently-emitted events"));
    suite.AddTestCase(TestCase("unsubscribe_all_by_owner",
                               test_unsubscribe_all_by_owner,
                               "unsubscribe_all(owner) removes all event-type "
                               "subscriptions for the given owner"));
    suite.AddTestCase(TestCase("deferred_unsubscribe_during_drain",
                               test_deferred_unsubscribe_during_drain,
                               "unsubscribe_all() called from within a handler is deferred: "
                               "owner still receives current-pass events, "
                               "but does not receive subsequently-emitted events"));
    suite.RunAllTests();
}

int main() {
    RunScriptEventBusTests();
    return 0;
}
