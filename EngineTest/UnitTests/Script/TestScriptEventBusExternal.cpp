// === Phase 2b.2: String-Keyed Event Bus C ABI Tests ===
//
// Validates the parallel subscribers_c_ channel on script_event_bus via the
// C ABI surface (script_event_subscribe/emit/unsubscribe/unsubscribe_all).
// No Lua involvement — these tests exercise engine-only string-keyed path.
//
// Contract verified (see spec §7.2):
//   Test 1: basic subscribe + emit + drain round-trip
//   Test 2: unsubscribe_all_by_data removes matching subscriptions only
//   Test 3: payload deleter runs AFTER all handlers complete
//   Test 4: re-emit isolation — handler-emitted event goes to re_emitted_
//           queue, delivered in same drain's internal pass

#include "../TestFramework.h"
#include "Components/Script.h"
#include "Components/ScriptEventBus.h"
#include "Components/ScriptExternal.h"

#include <cstdio>
#include <cstring>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

namespace {

// Test 1: subscribe + emit + drain → handler fires with matching args
TestResult test_event_bus_external_basic_round_trip() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    struct Capture { int fired_count = 0; u64 recv_size = 0; int recv_value = 0; };
    Capture cap;

    int payload_value = 42;

    u64 sub_id = script_event_subscribe("test_event", &cap,
        [](void* ud, const void* p, u64 s) {
            auto* c = static_cast<Capture*>(ud);
            ++c->fired_count;
            c->recv_size = s;
            std::memcpy(&c->recv_value, p, sizeof(int));
        });
    if (sub_id == 0) {
        std::fprintf(stderr, "subscribe returned 0\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    script_event_emit("test_event", &payload_value, sizeof(payload_value),
        [](void* /*p*/) { /* no-op deleter — payload is stack */ });

    primal::script::drain_events();

    script_event_unsubscribe(sub_id);
    primal::script::shutdown();

    if (cap.fired_count != 1) {
        std::fprintf(stderr, "fired_count=%d (expected 1)\n", cap.fired_count);
        return TestResult::Failed;
    }
    if (cap.recv_size != sizeof(int)) {
        std::fprintf(stderr, "recv_size=%llu (expected %zu)\n",
                     (unsigned long long)cap.recv_size, sizeof(int));
        return TestResult::Failed;
    }
    if (cap.recv_value != 42) {
        std::fprintf(stderr, "recv_value=%d (expected 42)\n", cap.recv_value);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 2: unsubscribe_all_by_data removes only matching user_data subs.
// user_data is a plain int* (the counter). The handler dereferences it
// directly — same indirection pattern Lua backend's LuaSubRecord* will use.
TestResult test_event_bus_external_unsubscribe_all_by_data() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    int a_count = 0, b_count = 0;

    auto handler_thunk = [](void* ud, const void* /*payload*/, u64 /*size*/) {
        ++(*static_cast<int*>(ud));
    };

    u64 a1 = script_event_subscribe("evt", &a_count, handler_thunk);
    u64 a2 = script_event_subscribe("evt", &a_count, handler_thunk);  // 2nd A sub
    u64 b1 = script_event_subscribe("evt", &b_count, handler_thunk);
    (void)a1; (void)a2; (void)b1;

    // Unsubscribe all of &a_count — both A1 and A2 must die, B untouched
    script_event_unsubscribe_all(&a_count);

    int payload = 0;
    script_event_emit("evt", &payload, sizeof(payload),
        [](void*) {});
    primal::script::drain_events();

    primal::script::shutdown();

    if (a_count != 0) {
        std::fprintf(stderr, "a_count=%d (expected 0 — A unsubscribed)\n", a_count);
        return TestResult::Failed;
    }
    if (b_count != 1) {
        std::fprintf(stderr, "b_count=%d (expected 1 — only B's handler fires once)\n", b_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 3: deleter runs AFTER handler completes (engine holds payload during dispatch)
TestResult test_event_bus_external_payload_lifetime() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    struct State { bool handler_saw_deleter_ran = false; bool deleter_ran = false; };
    State st;

    auto handler_thunk = [](void* ud, const void* payload, u64 size) {
        auto* s = static_cast<State*>(ud);
        (void)payload; (void)size;
        // At handler time, deleter should NOT have run
        s->handler_saw_deleter_ran = s->deleter_ran;
    };

    // Payload carries only the State* — no value field, just verify lifetime.
    State** p = new State*{&st};

    u64 sub = script_event_subscribe("evt", &st, handler_thunk);
    script_event_emit("evt", p, sizeof(State*),
        [](void* raw) {
            (*static_cast<State**>(raw))->deleter_ran = true;
            delete static_cast<State**>(raw);
        });
    primal::script::drain_events();
    script_event_unsubscribe(sub);
    primal::script::shutdown();

    if (!st.deleter_ran) {
        std::fprintf(stderr, "deleter never ran\n");
        return TestResult::Failed;
    }
    if (st.handler_saw_deleter_ran) {
        std::fprintf(stderr, "deleter ran DURING handler — payload freed before handler read it\n");
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 4: handler-emitted event goes to re_emitted_ queue, delivered in
// same drain's internal pass (depth < cap)
TestResult test_event_bus_external_re_emit_isolation() {
    primal::script::initialize();
    auto& bus = primal::script::script_event_bus::instance();
    bus.reset();

    struct State { int primary = 0; int secondary = 0; };
    State st;

    auto primary_thunk = [](void* ud, const void* payload, u64 size) {
        auto* s = static_cast<State*>(ud);
        ++s->primary;
        (void)payload; (void)size;
        // Emit a secondary event from within handler — exercises re_emitted_ path
        int* p = new int(7);
        script_event_emit("secondary", p, sizeof(int),
            [](void* raw) { delete static_cast<int*>(raw); });
    };
    auto secondary_thunk = [](void* ud, const void* payload, u64 size) {
        auto* s = static_cast<State*>(ud);
        ++s->secondary;
        (void)payload; (void)size;
    };

    u64 sub_primary = script_event_subscribe("primary", &st, primary_thunk);
    u64 sub_secondary = script_event_subscribe("secondary", &st, secondary_thunk);
    (void)sub_primary; (void)sub_secondary;

    int payload = 1;
    script_event_emit("primary", &payload, sizeof(payload),
        [](void*) {});
    primal::script::drain_events();

    primal::script::shutdown();

    if (st.primary != 1) {
        std::fprintf(stderr, "primary fired %d times (expected 1)\n", st.primary);
        return TestResult::Failed;
    }
    if (st.secondary != 1) {
        std::fprintf(stderr, "secondary fired %d times (expected 1 — re-emit in same drain)\n",
                     st.secondary);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

} // anonymous namespace

void RunScriptEventBusExternalTests() {
    TestSuite suite("Script.EventBusExternal");
    suite.AddTestCase(TestCase("event_bus_external_basic_round_trip",
                               test_event_bus_external_basic_round_trip,
                               "subscribe + emit + drain delivers payload to handler"));
    suite.AddTestCase(TestCase("event_bus_external_unsubscribe_all_by_data",
                               test_event_bus_external_unsubscribe_all_by_data,
                               "unsubscribe_all removes only matching user_data subs"));
    suite.AddTestCase(TestCase("event_bus_external_payload_lifetime",
                               test_event_bus_external_payload_lifetime,
                               "payload deleter runs after all handlers complete"));
    suite.AddTestCase(TestCase("event_bus_external_re_emit_isolation",
                               test_event_bus_external_re_emit_isolation,
                               "handler-emitted event goes to re_emitted_ queue, "
                               "delivered in same drain's internal pass"));
    suite.RunAllTests();
}

int main() {
    RunScriptEventBusExternalTests();
    return 0;
}
