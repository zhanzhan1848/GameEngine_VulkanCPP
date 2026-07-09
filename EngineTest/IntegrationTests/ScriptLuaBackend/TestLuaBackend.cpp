// === Phase 2a Task 3-4 + Phase 2b.1 Task 3-4 + Phase 2b.2 Task 4: Lua Backend Tests ===
//
// Test 1: lifecycle hooks fire (begin_play x1, update x3, destroy x1) — read
//         back via instance fields (Phase 2b.1) using find_instance().
// Test 2: reflect declares properties via Lua-side visitor API.
// Test 3 (added in Task 4): multi-instance isolation — two instances of the
//         same type maintain independent hook counts.
// Test 4 (Phase 2b.2): event bus round-trip — 2 instances subscribe to
//         ping/pong, drain delivers across instances + re-emit isolation.

#include "../UnitTests/TestFramework.h"
#include "LuaBackend.h"
#include "Components/Script.h"
#include "Components/ScriptProperty.h"
#include "Components/ScriptExternal.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"

#include <cstdio>
#include <string>

extern "C" {
#include "lua.h"
}

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using primal::lua_backend::LuaBackend;
using primal::lua_backend::LuaScriptInstance;

namespace {

// Helper: create a minimal entity for testing.
static primal::game_entity::entity make_test_entity() {
    primal::transform::init_info tinfo{};
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// Helper: read an integer field from a Lua instance table.
// Stack-safe: pushes and pops the field value.
static int lua_get_instance_int(LuaScriptInstance* inst, const char* field) {
    if (!inst || !inst->type || !inst->type->lua_state) return -1;
    lua_State* L = (lua_State*)inst->type->lua_state;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    lua_getfield(L, -1, field);
    int v = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);
    return v;
}

// Helper: read a boolean field from a Lua instance table.
static bool lua_get_instance_bool(LuaScriptInstance* inst, const char* field) {
    if (!inst || !inst->type || !inst->type->lua_state) return false;
    lua_State* L = (lua_State*)inst->type->lua_state;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, field);
    bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return v;
}

// Test 1: register a minimal Lua script, attach to entity, frame_tick x3,
// destroy. Verify begin_play called once, update called 3 times, destroy
// called once. Reads back via instance fields (Phase 2b.1).
TestResult test_lua_lifecycle_hooks_called() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "minimal_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/minimal_script.lua"
    );
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "create_instance returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // Run 3 frame_ticks.
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);

    // Read back instance fields BEFORE destroy — destroy erases the instance
    // from LuaBackend's map. begin_play/destroy_called are already set by
    // hooks up to this point; destroy_called is read INSIDE my_destroy (not
    // observable from outside).
    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr before destroy\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    bool begin_play_called = lua_get_instance_bool(inst, "begin_play_called");
    int update_count = lua_get_instance_int(inst, "update_count");

    // Destroy. my_destroy fires the Lua destroy hook (setting destroy_called
    // on the instance table), unrefs the table, then erases from the map.
    // We verify destroy_hook_fired indirectly: if my_destroy ran, the
    // instance was erased from the map.
    primal::script::remove_for_entity(entity.get_id());

    bool destroy_hook_fired =
        (LuaBackend::instance().find_instance(script_id) == nullptr);

    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (!begin_play_called) {
        std::fprintf(stderr, "begin_play_called=false (expected true)\n");
        return TestResult::Failed;
    }
    if (update_count != 3) {
        std::fprintf(stderr, "update_count=%d (expected 3)\n", update_count);
        return TestResult::Failed;
    }
    if (!destroy_hook_fired) {
        std::fprintf(stderr, "destroy hook did not fire (instance still in map)\n");
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 2: register a Lua script with a reflect hook. Verify the Lua-side
// visitor API declares 2 properties with correct name/type.
TestResult test_lua_reflect_declares_properties() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    u64 type_id = LuaBackend::instance().register_type(
        "reflect_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/reflect_script.lua"
    );
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "create_instance returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::property_collector collector;
    property_visitor_c visitor;
    visitor.state = &collector;
    visitor.property = [](void* st, const char* name, int type, u32 offset) {
        (void)offset;
        auto* pc = static_cast<primal::script::property_collector*>(st);
        pc->property(name, (primal::script::property_type)type, /*offset=*/0);
    };
    visitor.property_enum = [](void* st, const char* name, u32 offset,
                                u32 count, const char** names) {
        (void)st; (void)name; (void)offset; (void)count; (void)names;
    };
    visitor.property_with_accessor = [](void* st, const char* name, int type,
                                         void(*g)(void*, void*),
                                         void(*s)(void*, const void*)) {
        (void)st; (void)name; (void)type; (void)g; (void)s;
    };

    LuaBackend::instance().invoke_reflect_for_test(inst, visitor);

    const auto& descs = collector.descriptors();
    if (descs.size() != 2) {
        std::fprintf(stderr, "descriptors.size()=%zu (expected 2)\n", descs.size());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    if (std::string(descs[0].name) != "speed" ||
        descs[0].type != primal::script::property_type::float32) {
        std::fprintf(stderr, "descs[0] mismatch: name=%s type=%d\n",
                     descs[0].name, (int)descs[0].type);
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    if (std::string(descs[1].name) != "count" ||
        descs[1].type != primal::script::property_type::int32) {
        std::fprintf(stderr, "descs[1] mismatch: name=%s type=%d\n",
                     descs[1].name, (int)descs[1].type);
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::shutdown();
    LuaBackend::instance().shutdown();
    return TestResult::Passed;
}

// Test 3: two instances of the same type on different entities. Each
// instance must maintain independent hook counts (begin_play_called,
// update_count, destroy_called). Validates Phase 2b.1 multi-instance goal.
TestResult test_lua_multi_instance_per_type() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity_a = make_test_entity();
    primal::game_entity::entity entity_b = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "minimal_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/minimal_script.lua"
    );
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_a = LuaBackend::instance().create_instance(type_id, entity_a.get_id());
    u64 script_b = LuaBackend::instance().create_instance(type_id, entity_b.get_id());
    if (script_a == u64_invalid_id || script_b == u64_invalid_id) {
        std::fprintf(stderr, "create_instance returned invalid_id (a=%llu, b=%llu)\n",
                     (unsigned long long)script_a, (unsigned long long)script_b);
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst_a = LuaBackend::instance().find_instance(script_a);
    LuaScriptInstance* inst_b = LuaBackend::instance().find_instance(script_b);
    if (!inst_a || !inst_b) {
        std::fprintf(stderr, "find_instance returned nullptr (a=%p, b=%p)\n",
                     (void*)inst_a, (void*)inst_b);
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 2 frame_ticks — both A and B update (count goes to 2 each).
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);

    int a_updates_after_2 = lua_get_instance_int(inst_a, "update_count");
    int b_updates_after_2 = lua_get_instance_int(inst_b, "update_count");

    // Destroy A. B stays alive.
    primal::script::remove_for_entity(entity_a.get_id());

    // inst_a is now dangling — do not touch it. inst_b is still valid.
    bool a_destroy_hook_fired =
        (LuaBackend::instance().find_instance(script_a) == nullptr);

    // 1 more frame_tick — only B updates (B count goes to 3, A is gone).
    primal::script::frame_tick(0.016f);

    int b_updates_after_3 = lua_get_instance_int(inst_b, "update_count");

    // Destroy B.
    primal::script::remove_for_entity(entity_b.get_id());
    bool b_destroy_hook_fired =
        (LuaBackend::instance().find_instance(script_b) == nullptr);

    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    // Verify A: 2 updates (before destroy), destroy fired.
    if (a_updates_after_2 != 2) {
        std::fprintf(stderr, "A update_count after 2 ticks=%d (expected 2)\n",
                     a_updates_after_2);
        return TestResult::Failed;
    }
    if (!a_destroy_hook_fired) {
        std::fprintf(stderr, "A destroy hook did not fire\n");
        return TestResult::Failed;
    }

    // Verify B: 2 updates before A's destroy, 3 after the extra tick.
    if (b_updates_after_2 != 2) {
        std::fprintf(stderr, "B update_count after 2 ticks=%d (expected 2)\n",
                     b_updates_after_2);
        return TestResult::Failed;
    }
    if (b_updates_after_3 != 3) {
        std::fprintf(stderr, "B update_count after 3 ticks=%d (expected 3)\n",
                     b_updates_after_3);
        return TestResult::Failed;
    }
    if (!b_destroy_hook_fired) {
        std::fprintf(stderr, "B destroy hook did not fire\n");
        return TestResult::Failed;
    }

    // Critical: A and B update counts diverged (2 vs 3) — proves instances
    // are isolated. Under Phase 2a's "1 type 1 instance" hack, the second
    // create_instance would have clobbered the first instance's table,
    // making one of these counts wrong.
    return TestResult::Passed;
}

// Test 4: Lua event bus round-trip — 2 instances subscribe to ping/pong,
// each first update emits ping (both have update_count==0 entering first
// update), ping handler re-emits pong, drain delivers all.
TestResult test_lua_event_bus_round_trip() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity_a = make_test_entity();
    primal::game_entity::entity entity_b = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "event_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/event_script.lua"
    );
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_a = LuaBackend::instance().create_instance(type_id, entity_a.get_id());
    u64 script_b = LuaBackend::instance().create_instance(type_id, entity_b.get_id());
    if (script_a == u64_invalid_id || script_b == u64_invalid_id) {
        std::fprintf(stderr, "create_instance failed (a=%llu, b=%llu)\n",
                     (unsigned long long)script_a, (unsigned long long)script_b);
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst_a = LuaBackend::instance().find_instance(script_a);
    LuaScriptInstance* inst_b = LuaBackend::instance().find_instance(script_b);
    if (!inst_a || !inst_b) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 1 frame_tick — both A.update and B.update run while their update_count
    // is 0, so both emit ping. drain delivers each ping to BOTH subscribers,
    // and each ping handler emits pong → drain delivers each pong to both.
    primal::script::frame_tick(0.016f);

    int a_update = lua_get_instance_int(inst_a, "update_count");
    int a_ping = lua_get_instance_int(inst_a, "ping_count");
    int a_pong = lua_get_instance_int(inst_a, "pong_count");
    int b_update = lua_get_instance_int(inst_b, "update_count");
    int b_ping = lua_get_instance_int(inst_b, "ping_count");
    int b_pong = lua_get_instance_int(inst_b, "pong_count");

    // Cleanup
    primal::script::remove_for_entity(entity_a.get_id());
    primal::script::remove_for_entity(entity_b.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    // Verify: both A and B updated exactly once
    if (a_update != 1 || b_update != 1) {
        std::fprintf(stderr, "update counts: A=%d B=%d (expected 1/1)\n", a_update, b_update);
        return TestResult::Failed;
    }
    // Both received 2 pings (one from A.update, one from B.update).
    if (a_ping != 2 || b_ping != 2) {
        std::fprintf(stderr, "ping counts: A=%d B=%d (expected 2/2)\n", a_ping, b_ping);
        return TestResult::Failed;
    }
    // Both received 4 pongs — 2 ping events × 2 pong emits per ping (one
    // from A's ping handler, one from B's).
    if (a_pong != 4 || b_pong != 4) {
        std::fprintf(stderr, "pong counts: A=%d B=%d (expected 4/4)\n", a_pong, b_pong);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

} // anonymous namespace

void RunLuaBackendTests() {
    TestSuite suite("Script.LuaBackend");
    suite.AddTestCase(TestCase("lua_lifecycle_hooks_called",
                               test_lua_lifecycle_hooks_called,
                               "Lua script receives begin_play (x1) / update (x3) / "
                               "destroy (x1) via Phase 1 C ABI"));
    suite.AddTestCase(TestCase("lua_reflect_declares_properties",
                               test_lua_reflect_declares_properties,
                               "Lua script declares 2 properties via Lua-side visitor API"));
    suite.AddTestCase(TestCase("lua_multi_instance_per_type",
                               test_lua_multi_instance_per_type,
                               "Two instances of same type maintain independent hook counts"));
    suite.AddTestCase(TestCase("lua_event_bus_round_trip",
                               test_lua_event_bus_round_trip,
                               "2 instances subscribe to ping/pong, drain delivers across "
                               "instances + re-emit isolation"));
    suite.RunAllTests();
}

int main() {
    RunLuaBackendTests();
    return 0;
}
