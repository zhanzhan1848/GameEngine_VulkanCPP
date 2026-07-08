// === Phase 1 Task 6: register_external C ABI 测试 ===
//
// 验证 C ABI 的核心契约:
//   1) script_register_external + script_create_external 能成功注册并创建
//      外部脚本类型,begin_play/update/destroy 回调按预期被调用,user_data
//      正确传递。
//   2) 全 NULL 回调 + NULL user_data 不会 crash(no-op adapter)。
//   3) C visitor reflect 路径:外部 reflect 回调通过 property_visitor_c
//      声明属性,property_collector 在 C++ 侧收集到匹配的描述符。
//
// 设计说明:
//   - 与 TestScriptLifecycle.cpp 类似的模式:手动 entity 创建 + script::initialize。
//   - C ABI 函数是 extern "C",但测试本身是 C++,可以直接调用。
//   - external_types_ 是 Script.cpp 的 file-static,测试通过 C ABI 间接验证。

#include "../TestFramework.h"
#include "Components/ScriptExternal.h"
#include "Components/ScriptProperty.h"
#include "Components/Script.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"

#include <string>
#include <cstdio>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

namespace {

// Helper: 创建一个带最小化 transform 的 entity(与 TestScriptLifecycle 同模式)
static primal::game_entity::entity make_test_entity() {
    primal::transform::init_info tinfo{};
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// ----------------------------------------------------------------------------
// Test 1: register_external basic dispatch
//
// Register a type with begin_play/update/destroy callbacks. The callbacks
// receive user_data pointing to a struct with counters. Create an entity +
// script instance via script_create_external. Verify:
//   a) begin_play was called at create time
//   b) update was called after script::update with correct dt
//   c) destroy was called after remove_for_entity
//   d) user_data pointer was forwarded correctly to every callback
// ----------------------------------------------------------------------------
struct counters_state {
    int begin_play_calls;
    int update_calls;
    int destroy_calls;
    float last_dt;
};

void test1_begin_play(void* ud) {
    auto* s = static_cast<counters_state*>(ud);
    ++s->begin_play_calls;
}

void test1_update(void* ud, float dt) {
    auto* s = static_cast<counters_state*>(ud);
    ++s->update_calls;
    s->last_dt = dt;
}

void test1_destroy(void* ud) {
    auto* s = static_cast<counters_state*>(ud);
    ++s->destroy_calls;
}

TestResult test_register_external_basic_dispatch() {
    primal::script::initialize();

    counters_state state{0, 0, 0, 0.0f};

    script_external_callbacks cbs{};
    cbs.begin_play = &test1_begin_play;
    cbs.update = &test1_update;
    cbs.destroy = &test1_destroy;

    u64 type_id = script_register_external("test_basic", &state, &cbs);
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external returned invalid_id\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Create entity + external script
    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external returned invalid_id\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // begin_play should have fired during create
    if (state.begin_play_calls != 1) {
        std::fprintf(stderr, "begin_play_calls=%d (expected 1)\n", state.begin_play_calls);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // update
    primal::script::update(0.016f);
    if (state.update_calls != 1 || state.last_dt != 0.016f) {
        std::fprintf(stderr, "update_calls=%d last_dt=%f (expected 1, 0.016)\n",
                     state.update_calls, state.last_dt);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // destroy via remove_for_entity
    primal::script::remove_for_entity(entity.get_id());
    if (state.destroy_calls != 1) {
        std::fprintf(stderr, "destroy_calls=%d (expected 1)\n", state.destroy_calls);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 2: null callbacks are no-op
//
// Register a type with ALL null callback function pointers and nullptr
// user_data. Create + run frame_tick + remove. Must not crash.
// ----------------------------------------------------------------------------
TestResult test_null_callbacks_are_noop() {
    primal::script::initialize();

    script_external_callbacks cbs{};  // all fields zero-initialized = all NULL

    u64 type_id = script_register_external("test_null", nullptr, &cbs);
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external returned invalid_id for null test\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external returned invalid_id for null test\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Run a frame — all callbacks are null, adapter should silently skip
    primal::script::frame_tick(0.016f);

    // Remove must also be safe with null destroy
    primal::script::remove_for_entity(entity.get_id());

    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 3: C visitor → property_collector bridge
//
// Phase 1 has no public API to retrieve a script instance and call reflect()
// on it. This test exercises only the C visitor → property_collector bridge
// in isolation. The full adapter path (external_script::reflect →
// cbs_.reflect → visitor → collector) is covered by Task 9's Self-Test
// Backend (pure C end-to-end).
// ----------------------------------------------------------------------------

struct reflect_test_state {
    int reflect_calls;
    int property_calls;
    int property_enum_calls;
};

// Reflect callback: uses the C visitor to declare 2 properties.
void test3_reflect(void* ud, property_visitor_c visitor) {
    auto* s = static_cast<reflect_test_state*>(ud);
    ++s->reflect_calls;

    // Declare a float32 property "speed" at offset 0
    if (visitor.property) {
        visitor.property(visitor.state, "speed", SCRIPT_PROPERTY_TYPE_FLOAT32, 0);
        ++s->property_calls;
    }

    // Declare an enum property "mode" at offset 4 with 3 values
    static const char* enum_names[] = {"Idle", "Active", "Paused"};
    if (visitor.property_enum) {
        visitor.property_enum(visitor.state, "mode", 4, 3, enum_names);
        ++s->property_enum_calls;
    }
}

TestResult test_c_visitor_to_property_collector_bridge() {
    primal::script::initialize();

    reflect_test_state state{0, 0, 0};

    script_external_callbacks cbs{};
    cbs.reflect = &test3_reflect;

    u64 type_id = script_register_external("test_reflect", &state, &cbs);
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external returned invalid_id for reflect test\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Create entity + external script
    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external returned invalid_id for reflect test\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Build a property_collector and a C visitor that forwards into it,
    // then manually drive the reflect callback. This tests the C visitor →
    // property_collector bridging logic in isolation.
    primal::script::property_collector collector;
    property_visitor_c visitor;
    visitor.state = &collector;
    visitor.property = [](void* st, const char* name, int type, u32 offset) {
        auto* pc = static_cast<primal::script::property_collector*>(st);
        pc->property(name, (primal::script::property_type)type, offset);
    };
    visitor.property_enum = [](void* st, const char* name, u32 offset,
                                u32 count, const char** names) {
        auto* pc = static_cast<primal::script::property_collector*>(st);
        pc->property_enum(name, offset, count, names);
    };
    visitor.property_with_accessor = [](void* st, const char* name, int type,
                                         void(*g)(void*, void*),
                                         void(*s)(void*, const void*)) {
        auto* pc = static_cast<primal::script::property_collector*>(st);
        pc->property_with_accessor(name, (primal::script::property_type)type, g, s);
    };

    // Drive the C reflect callback with our visitor
    test3_reflect(&state, visitor);

    // Verify the reflect callback was invoked
    if (state.reflect_calls != 1) {
        std::fprintf(stderr, "reflect_calls=%d (expected 1)\n", state.reflect_calls);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Verify the collector received 2 descriptors
    const auto& descs = collector.descriptors();
    if (descs.size() != 2) {
        std::fprintf(stderr, "descriptors.size()=%zu (expected 2)\n", descs.size());
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Check first descriptor: "speed" float32 at offset 0
    if (std::string(descs[0].name) != "speed" ||
        descs[0].type != primal::script::property_type::float32 ||
        descs[0].offset != 0) {
        std::fprintf(stderr, "descs[0] mismatch: name=%s type=%d offset=%u\n",
                     descs[0].name, (int)descs[0].type, descs[0].offset);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Check second descriptor: "mode" enum at offset 4 with 3 names
    if (std::string(descs[1].name) != "mode" ||
        descs[1].type != primal::script::property_type::enum_ ||
        descs[1].offset != 4 ||
        descs[1].enum_count != 3) {
        std::fprintf(stderr, "descs[1] mismatch: name=%s type=%d offset=%u enum_count=%u\n",
                     descs[1].name, (int)descs[1].type, descs[1].offset, descs[1].enum_count);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Clean up
    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 4: reject invalid args (null/empty type_name, null callbacks)
//
// Spec §6.3 requires: null/empty type_name → return invalid_id, log.
// Verify all three invalid-arg combinations are rejected.
// ----------------------------------------------------------------------------
TestResult test_register_external_rejects_invalid_args() {
    primal::script::initialize();

    counters_state state{0, 0, 0, 0.0f};

    script_external_callbacks cbs{};
    cbs.begin_play = &test1_begin_play;
    cbs.update = &test1_update;
    cbs.destroy = &test1_destroy;

    // 1) null type_name
    u64 id1 = script_register_external(nullptr, &state, &cbs);
    if (id1 != u64_invalid_id) {
        std::fprintf(stderr, "null type_name should return invalid_id, got %llu\n",
                     (unsigned long long)id1);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // 2) empty type_name
    u64 id2 = script_register_external("", &state, &cbs);
    if (id2 != u64_invalid_id) {
        std::fprintf(stderr, "empty type_name should return invalid_id, got %llu\n",
                     (unsigned long long)id2);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // 3) null callbacks
    u64 id3 = script_register_external("valid_name", &state, nullptr);
    if (id3 != u64_invalid_id) {
        std::fprintf(stderr, "null callbacks should return invalid_id, got %llu\n",
                     (unsigned long long)id3);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::script::shutdown();
    return TestResult::Passed;
}

} // anonymous namespace

void RunScriptExternalTests() {
    TestSuite suite("Script.External");
    suite.AddTestCase(TestCase("register_external_basic_dispatch",
                               test_register_external_basic_dispatch,
                               "register with begin_play/update/destroy callbacks; "
                               "verify all called with correct user_data"));
    suite.AddTestCase(TestCase("null_callbacks_are_noop",
                               test_null_callbacks_are_noop,
                               "all-null callbacks struct + nullptr user_data: "
                               "create + frame_tick + remove does not crash"));
    suite.AddTestCase(TestCase("c_visitor_to_property_collector_bridge",
                               test_c_visitor_to_property_collector_bridge,
                               "C visitor → property_collector bridge in isolation "
                               "(full adapter path covered by Task 9)"));
    suite.AddTestCase(TestCase("register_external_rejects_invalid_args",
                               test_register_external_rejects_invalid_args,
                               "null/empty type_name and null callbacks are rejected with invalid_id"));
    suite.RunAllTests();
}

int main() {
    RunScriptExternalTests();
    return 0;
}
