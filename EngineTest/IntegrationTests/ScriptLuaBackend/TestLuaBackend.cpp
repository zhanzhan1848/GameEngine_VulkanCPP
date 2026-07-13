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

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>

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

// Read a float field from a LuaScriptInstance's instance table.
// Used by Test 9 to verify dt propagation (float→double→float round-trip).
static float lua_get_instance_float(LuaScriptInstance* inst, const char* field) {
    if (!inst || !inst->type || !inst->type->lua_state) return 0.0f;
    lua_State* L = (lua_State*)inst->type->lua_state;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0.0f;
    }
    lua_getfield(L, -1, field);
    float value = 0.0f;
    if (lua_isnumber(L, -1)) {
        value = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 2);
    return value;
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

// Helper: call a no-arg Lua method on the instance table and return int result.
// Equivalent to: local v = self:method_name()
static int lua_call_instance_int(LuaScriptInstance* inst, const char* method_name) {
    if (!inst || !inst->type || !inst->type->lua_state) return -1;
    lua_State* L = (lua_State*)inst->type->lua_state;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);  // instance_table
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    lua_getfield(L, -1, method_name);  // instance_table method_fn
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return -1; }
    lua_pushvalue(L, -2);  // instance_table method_fn self
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 2);  // pop error + instance_table
        return -1;
    }
    int v = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);  // pop result + instance_table
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

// Test 5: LuaBackend::shutdown() safety net must survive script::shutdown()
// being called first. This exercises the Phase 2b.2 fix: release_sub_record
// checks script::is_initialized() and skips engine unsubscribe when false.
//
// Without the fix, the safety net's release_sub_record →
// script_event_unsubscribe → check_main_thread → assert(g_initialized)
// would fire.
TestResult test_lua_shutdown_after_script_shutdown_safe() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity_a = make_test_entity();

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
    if (script_a == u64_invalid_id) {
        std::fprintf(stderr, "create_instance returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 1 frame_tick — begin_play fires → subscribes to ping/pong. Update emits
    // ping. drain delivers. Subscriptions are now live in inst_subs_.
    primal::script::frame_tick(0.016f);

    // MISUSE PATH: call script::shutdown() WITHOUT remove_for_entity. The
    // engine bus is destroyed but LuaBackend still holds LuaSubRecord*'s
    // referencing those (now-defunct) engine subscription ids.
    primal::script::shutdown();

    // This call previously crashed: safety net loop tried to unsubscribe via
    // a dead engine bus. With the fix, release_sub_record sees
    // script::is_initialized()==false and skips the engine call, only
    // releasing the Lua refs and freeing the record.
    LuaBackend::instance().shutdown();

    // If we got here without assert/crash, the fix works.
    return TestResult::Passed;
}

// Test 6: Lua hard reload with old_state migration.
//   1 frame_tick: old.update_count=1, old.score=42
//   script::reload(eid) — deferred
//   1 more frame_tick: old.update fires (count=2), then reload fires:
//     capture_state saves score=42 from old table
//     destroy frees old LuaScriptInstance
//     recreate allocates fresh LuaScriptInstance
//     begin_play runs (fresh: count=0, score=0)
//     on_reload(self, old_table) — Lua migrates score=42
//   Verify new instance: update_count=0, score=42
TestResult test_lua_reload_preserves_old_state() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "reload_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/reload_script.lua"
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

    // 1 frame_tick: old.update_count = 1, old.score = 42
    primal::script::frame_tick(0.016f);

    // Defer reload
    primal::script::reload(entity.get_id());

    // 1 more frame_tick: old.update fires (count=2), then process_deferred_reloads
    // constructs new instance. New instance has update_count=0 (fresh begin_play),
    // score=42 (migrated via on_reload from captured old table).
    primal::script::frame_tick(0.016f);

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr after reload\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    int update_count = lua_get_instance_int(inst, "update_count");
    int score = lua_get_instance_int(inst, "score");
    // Phase 2b.3 Issue 1: verify old instance's Lua destroy hook fired
    // exactly once during reload (capture_state_for_reload invokes it
    // before transferring table ownership).
    int destroy_count = lua_call_instance_int(inst, "get_destroy_count");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (update_count != 0) {
        std::fprintf(stderr, "post-reload update_count=%d (expected 0 — fresh begin_play)\n",
                     update_count);
        return TestResult::Failed;
    }
    if (score != 42) {
        std::fprintf(stderr, "post-reload score=%d (expected 42 — migrated via on_reload)\n",
                     score);
        return TestResult::Failed;
    }
    if (destroy_count != 1) {
        std::fprintf(stderr, "post-reload destroy_count=%d (expected 1 — old instance destroy fired during reload)\n",
                     destroy_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// === Phase 2b.3 Test 7: type-level source reload ===
//
// Tempfile helper: writes initial content, supports rewrite. RAII unlinks on
// destruction. Uses mkstemp for unique filename.
class TempLuaFile {
public:
    const std::string& get_path() const { return path; }

    explicit TempLuaFile(const std::string& content) {
        path = "/tmp/lua_reload_type_test_XXXXXX.lua";
        std::vector<char> tmpl(path.begin(), path.end());
        tmpl.push_back('\0');
        int fd = mkstemp(tmpl.data());
        if (fd == -1) {
            std::fprintf(stderr, "mkstemp failed\n");
            path.clear();
            return;
        }
        path = tmpl.data();
        if (write(fd, content.data(), content.size()) != (ssize_t)content.size()) {
            std::fprintf(stderr, "write failed\n");
            close(fd);
            unlink(path.c_str());
            path.clear();
            return;
        }
        close(fd);
    }

    void rewrite(const std::string& content) {
        if (path.empty()) return;
        int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
        if (fd == -1) {
            std::fprintf(stderr, "rewrite open failed\n");
            return;
        }
        if (write(fd, content.data(), content.size()) != (ssize_t)content.size()) {
            std::fprintf(stderr, "rewrite write failed\n");
            close(fd);
            unlink(path.c_str());
            path.clear();
            return;
        }
        close(fd);
    }

    ~TempLuaFile() {
        if (!path.empty()) unlink(path.c_str());
    }

    TempLuaFile(const TempLuaFile&) = delete;
    TempLuaFile& operator=(const TempLuaFile&) = delete;

private:
    std::string path;
};

// Test 7: LuaBackend::reload_type re-reads .lua source from disk.
//   register_type with file_v1 (value=1)
//   create instance A, verify value=1
//   modify file to v2 (value=2)
//   reload_type — re-reads file
//   create instance B, verify value=2 (new source)
//   Also verify instance A still reads value=1 (decoupled — A keeps old script).
TestResult test_lua_reload_type_re_reads_source() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    const std::string v1 =
        "local M = {}\n"
        "M.value = 1\n"
        "function M.begin_play(self) end\n"
        "function M.update(self, dt) end\n"
        "function M.destroy(self) end\n"
        "return M\n";
    const std::string v2 =
        "local M = {}\n"
        "M.value = 2\n"
        "function M.begin_play(self) end\n"
        "function M.update(self, dt) end\n"
        "function M.destroy(self) end\n"
        "return M\n";

    TempLuaFile tmp(v1);
    if (tmp.get_path().empty()) {
        std::fprintf(stderr, "TempLuaFile creation failed\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 type_id = LuaBackend::instance().register_type("reload_type_test", tmp.get_path().c_str());
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type failed\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity_a = make_test_entity();
    u64 script_a = LuaBackend::instance().create_instance(type_id, entity_a.get_id());
    if (script_a == u64_invalid_id) {
        std::fprintf(stderr, "create_instance A failed\n");
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst_a = LuaBackend::instance().find_instance(script_a);
    if (!inst_a) {
        std::fprintf(stderr, "find_instance A failed\n");
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    int value_a_first = lua_get_instance_int(inst_a, "value");

    // Modify file -> reload_type
    tmp.rewrite(v2);
    u64 reload_rc = LuaBackend::instance().reload_type(type_id);
    if (reload_rc == u64_invalid_id) {
        std::fprintf(stderr, "reload_type failed\n");
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // Create instance B AFTER reload_type — should use new source (value=2)
    primal::game_entity::entity entity_b = make_test_entity();
    u64 script_b = LuaBackend::instance().create_instance(type_id, entity_b.get_id());
    if (script_b == u64_invalid_id) {
        std::fprintf(stderr, "create_instance B failed\n");
        primal::script::remove_for_entity(entity_b.get_id());
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    auto* inst_b = LuaBackend::instance().find_instance(script_b);
    if (!inst_b) {
        std::fprintf(stderr, "find_instance B failed\n");
        primal::script::remove_for_entity(entity_b.get_id());
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }
    int value_b = lua_get_instance_int(inst_b, "value");

    // Instance A re-reads its value (still 1 — decoupled, A's instance table
    // holds its own shallow copy of the old script's fields).
    int value_a_after = lua_get_instance_int(inst_a, "value");

    primal::script::remove_for_entity(entity_a.get_id());
    primal::script::remove_for_entity(entity_b.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (value_a_first != 1) {
        std::fprintf(stderr, "A initial value=%d (expected 1)\n", value_a_first);
        return TestResult::Failed;
    }
    if (value_b != 2) {
        std::fprintf(stderr, "B value=%d (expected 2 — new source after reload_type)\n", value_b);
        return TestResult::Failed;
    }
    if (value_a_after != 1) {
        std::fprintf(stderr, "A value after reload_type=%d (expected 1 — decoupled)\n",
                     value_a_after);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 9: M.fixed_update + M.late_update fire when engine calls
// script::fixed_update(dt) / script::late_update(dt). Verifies dt propagation
// (float → lua_pushnumber → float round-trip is exact for these values).
TestResult test_lua_fixed_update_and_late_update_dispatch() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "lifecycle_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/lifecycle_script.lua"
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
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // begin_play already fired during create_instance (script_create_external
    // calls cbs.begin_play synchronously). No frame_tick needed — and frame_tick
    // would also fire fixed_update/late_update, polluting the counters.

    // Direct dispatch — should fire M.fixed_update with dt=0.033f.
    primal::script::fixed_update(0.033f);
    int fixed_count = lua_get_instance_int(inst, "fixed_count");
    float fixed_dt  = lua_get_instance_float(inst, "last_fixed_dt");

    // Direct dispatch — should fire M.late_update with dt=0.016f.
    primal::script::late_update(0.016f);
    int late_count = lua_get_instance_int(inst, "late_count");
    float late_dt  = lua_get_instance_float(inst, "last_late_dt");

    // Second calls — counters should reach 2.
    primal::script::fixed_update(0.033f);
    primal::script::late_update(0.016f);
    int fixed_count_2 = lua_get_instance_int(inst, "fixed_count");
    int late_count_2  = lua_get_instance_int(inst, "late_count");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (fixed_count != 1) {
        std::fprintf(stderr, "fixed_count after 1 call = %d (expected 1)\n", fixed_count);
        return TestResult::Failed;
    }
    if (late_count != 1) {
        std::fprintf(stderr, "late_count after 1 call = %d (expected 1)\n", late_count);
        return TestResult::Failed;
    }
    // float→double→float is exact for 0.033f and 0.016f. Use small epsilon
    // defensively in case of platform differences.
    const float eps = 1e-6f;
    if (fabsf(fixed_dt - 0.033f) > eps) {
        std::fprintf(stderr, "last_fixed_dt = %f (expected ~0.033)\n", fixed_dt);
        return TestResult::Failed;
    }
    if (fabsf(late_dt - 0.016f) > eps) {
        std::fprintf(stderr, "last_late_dt = %f (expected ~0.016)\n", late_dt);
        return TestResult::Failed;
    }
    if (fixed_count_2 != 2) {
        std::fprintf(stderr, "fixed_count after 2 calls = %d (expected 2)\n", fixed_count_2);
        return TestResult::Failed;
    }
    if (late_count_2 != 2) {
        std::fprintf(stderr, "late_count after 2 calls = %d (expected 2)\n", late_count_2);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 10: frame_tick dispatches hooks in order fixed_update → update → late_update.
// Uses the same lifecycle_script.lua fixture; verifies fixed_step=1, update_step=2,
// late_step=3 (each hook records its position in the call sequence).
//
// Note: begin_play already fired synchronously during create_instance (it is empty
// in the fixture and does not touch the step counter). frame_tick dispatches the
// three dt-passing hooks in order.
TestResult test_lua_frame_tick_lifecycle_order() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "lifecycle_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/lifecycle_script.lua"
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
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // Single frame_tick — dispatches fixed_update, update, late_update in order.
    // The fixture's step counter is file-scope; each hook snapshots it on entry.
    primal::script::frame_tick(0.016f);

    int fixed_step = lua_get_instance_int(inst, "fixed_step");
    int update_step = lua_get_instance_int(inst, "update_step");
    int late_step  = lua_get_instance_int(inst, "late_step");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (fixed_step != 1) {
        std::fprintf(stderr, "fixed_step = %d (expected 1 — fixed fires first)\n", fixed_step);
        return TestResult::Failed;
    }
    if (update_step != 2) {
        std::fprintf(stderr, "update_step = %d (expected 2 — update fires second)\n", update_step);
        return TestResult::Failed;
    }
    if (late_step != 3) {
        std::fprintf(stderr, "late_step = %d (expected 3 — late fires third)\n", late_step);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// extern "C" declarations for the Self-Test Backend (ScriptSelfTestBackend.c).
// The .c file is linked into TestLuaBackend to verify Phase 2b.3 reload flow
// doesn't break backends that DON'T implement the new hooks.
// ---------------------------------------------------------------------------
extern "C" {
int self_test_register_and_create(u64 entity_id_raw,
                                  u64* type_id_out,
                                  u64* script_handle_out);
int self_test_get_begin_play_count(void);
int self_test_get_update_count(void);
int self_test_get_destroy_count(void);
int self_test_get_on_reload_count(void);
int self_test_get_fixed_update_count(void);
int self_test_get_late_update_count(void);
void self_test_reset_counters(void);
} // extern "C"

namespace {

// Test 8: Self-Test Backend reload regression. The Self-Test Backend
// (ScriptSelfTestBackend.c) does NOT implement any of the three Phase 2b.3
// hooks. The engine's new flow must fall back gracefully:
//   capture_state_for_reload = NULL → captured_state stays null
//   recreate_instance_user_data_for_reload = NULL → new_inst_ud = nullptr
//   on_reload(nullptr) called — Self-Test's my_on_reload ignores old_state
// All existing Self-Test reload counters must still increment.
TestResult test_lua_self_test_backend_reload_unchanged() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    self_test_reset_counters();
    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = 0;
    u64 script_handle = 0;
    int rc = self_test_register_and_create(entity.get_id(), &type_id, &script_handle);
    if (rc != 1) {
        std::fprintf(stderr, "self_test_register_and_create failed\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 1 frame_tick — begin_play + 1 update on old instance
    primal::script::frame_tick(0.016f);

    int bp_before = self_test_get_begin_play_count();  // expect 1
    int upd_before = self_test_get_update_count();     // expect 1

    // Defer reload + 1 more frame_tick (old.update fires → reload fires →
    // new begin_play + new on_reload).
    primal::script::reload(entity.get_id());
    primal::script::frame_tick(0.016f);

    int bp_after = self_test_get_begin_play_count();    // expect 2 (1 old + 1 new)
    int upd_after = self_test_get_update_count();       // expect 2 (2 old updates pre-reload, 0 new this frame)
    int destroy_after = self_test_get_destroy_count();  // expect 1 (old instance destroyed)
    int reload_after = self_test_get_on_reload_count(); // expect 1 (new instance on_reload)

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (bp_before != 1 || upd_before != 1) {
        std::fprintf(stderr, "pre-reload counters wrong: bp=%d upd=%d\n", bp_before, upd_before);
        return TestResult::Failed;
    }
    if (bp_after != 2) {
        std::fprintf(stderr, "post-reload begin_play_count=%d (expected 2)\n", bp_after);
        return TestResult::Failed;
    }
    if (upd_after != 2) {
        std::fprintf(stderr, "post-reload update_count=%d (expected 2)\n", upd_after);
        return TestResult::Failed;
    }
    if (destroy_after != 1) {
        std::fprintf(stderr, "post-reload destroy_count=%d (expected 1)\n", destroy_after);
        return TestResult::Failed;
    }
    if (reload_after != 1) {
        std::fprintf(stderr, "post-reload on_reload_count=%d (expected 1)\n", reload_after);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 11: Self-Test Backend fixed_update + late_update regression.
// Self-Test Backend (ScriptSelfTestBackend.c) already implements both hooks
// with counters. This test verifies the engine dispatch path still works
// end-to-end after Phase 2b.4 Lua backend changes.
TestResult test_lua_self_test_backend_fixed_late_unchanged() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    self_test_reset_counters();
    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = 0;
    u64 script_handle = 0;
    int rc = self_test_register_and_create(entity.get_id(), &type_id, &script_handle);
    if (rc != 1) {
        std::fprintf(stderr, "self_test_register_and_create failed\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 1 frame_tick fires begin_play + fixed_update + update + late_update once each.
    primal::script::frame_tick(0.016f);

    int fixed_before = self_test_get_fixed_update_count();
    int late_before  = self_test_get_late_update_count();

    // Direct dispatch — should add 1 to each counter.
    primal::script::fixed_update(0.033f);
    primal::script::late_update(0.016f);

    int fixed_after = self_test_get_fixed_update_count();
    int late_after  = self_test_get_late_update_count();

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    // frame_tick fires fixed_update + late_update once each.
    if (fixed_before != 1) {
        std::fprintf(stderr, "fixed_before = %d (expected 1 from frame_tick)\n", fixed_before);
        return TestResult::Failed;
    }
    if (late_before != 1) {
        std::fprintf(stderr, "late_before = %d (expected 1 from frame_tick)\n", late_before);
        return TestResult::Failed;
    }
    // Direct dispatch adds 1 more.
    if (fixed_after != 2) {
        std::fprintf(stderr, "fixed_after = %d (expected 2 = frame_tick + direct)\n", fixed_after);
        return TestResult::Failed;
    }
    if (late_after != 2) {
        std::fprintf(stderr, "late_after = %d (expected 2 = frame_tick + direct)\n", late_after);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 12: Lua error in begin_play doesn't crash engine or invalidate instance.
// Fixture's begin_play calls error("boom in begin_play"). pcall in invoke_lua_hook
// catches it, logs to stderr, returns. create_instance must still return a valid
// script_id; the instance table must be populated. Direct script::update dispatch
// then fires M.update, incrementing update_count to 1.
TestResult test_lua_error_in_begin_play_survives() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "error_in_begin_play",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/error_in_begin_play.lua"
    );
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // create_instance fires begin_play synchronously (Phase 2b.4 verified timing).
    // begin_play throws; pcall catches; create_instance returns a valid id.
    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "create_instance returned invalid_id after begin_play error\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // Direct dispatch — should fire M.update, incrementing update_count.
    primal::script::update(0.016f);
    int update_count = lua_get_instance_int(inst, "update_count");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (update_count != 1) {
        std::fprintf(stderr, "update_count = %d (expected 1 — update must fire after begin_play error)\n", update_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 13: Lua error in update does not block late_update (same frame) or
// update (next frame). Pins per-hook pcall isolation and Lua state recovery.
// Fixture increments update_count BEFORE error() so the test can verify
// "update was dispatched" even though pcall caught the throw.
TestResult test_lua_error_in_update_does_not_block_late_update() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "error_in_update",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/error_in_update.lua"
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
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // Frame 1: frame_tick dispatches fixed_update (empty), update (errors after
    // incrementing counter), late_update (fires normally).
    primal::script::frame_tick(0.016f);
    int update_count_f1 = lua_get_instance_int(inst, "update_count");
    int late_count_f1   = lua_get_instance_int(inst, "late_count");

    // Frame 2: same dispatch order. update errors again; late fires again.
    primal::script::frame_tick(0.016f);
    int update_count_f2 = lua_get_instance_int(inst, "update_count");
    int late_count_f2   = lua_get_instance_int(inst, "late_count");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (update_count_f1 != 1) {
        std::fprintf(stderr, "update_count after frame 1 = %d (expected 1)\n", update_count_f1);
        return TestResult::Failed;
    }
    if (late_count_f1 != 1) {
        std::fprintf(stderr, "late_count after frame 1 = %d (expected 1 — late must fire after update error)\n", late_count_f1);
        return TestResult::Failed;
    }
    if (update_count_f2 != 2) {
        std::fprintf(stderr, "update_count after frame 2 = %d (expected 2)\n", update_count_f2);
        return TestResult::Failed;
    }
    if (late_count_f2 != 2) {
        std::fprintf(stderr, "late_count after frame 2 = %d (expected 2)\n", late_count_f2);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 14: Lua error in one instance doesn't affect another instance sharing
// the same lua_State. Pins per-instance pcall isolation across multiple frames.
// Creates two instances of different types (error_in_update + lifecycle_script),
// runs 3 frame_ticks, and verifies both instances' update_count == 3. Multi-frame
// is necessary to catch cumulative stack-corruption bugs that single-frame tests
// would miss.
TestResult test_lua_error_in_one_instance_does_not_affect_another() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity_a = make_test_entity();
    primal::game_entity::entity entity_b = make_test_entity();

    u64 error_type_id = LuaBackend::instance().register_type(
        "error_in_update",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/error_in_update.lua"
    );
    if (error_type_id == u64_invalid_id) {
        std::fprintf(stderr, "error register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 normal_type_id = LuaBackend::instance().register_type(
        "lifecycle_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/lifecycle_script.lua"
    );
    if (normal_type_id == u64_invalid_id) {
        std::fprintf(stderr, "normal register_type returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 error_script_id = LuaBackend::instance().create_instance(error_type_id, entity_a.get_id());
    if (error_script_id == u64_invalid_id) {
        std::fprintf(stderr, "error create_instance returned invalid_id\n");
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 normal_script_id = LuaBackend::instance().create_instance(normal_type_id, entity_b.get_id());
    if (normal_script_id == u64_invalid_id) {
        std::fprintf(stderr, "normal create_instance returned invalid_id\n");
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    auto* error_inst  = LuaBackend::instance().find_instance(error_script_id);
    auto* normal_inst = LuaBackend::instance().find_instance(normal_script_id);
    if (!error_inst || !normal_inst) {
        std::fprintf(stderr, "find_instance returned nullptr\n");
        primal::script::remove_for_entity(entity_a.get_id());
        primal::script::remove_for_entity(entity_b.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 3 frame_ticks dispatch both instances' hooks. Each frame: error_inst's update
    // throws (but increments counter first); normal_inst's update must still fire.
    // Multi-frame proves durable isolation — a stack-corruption bug from repeated
    // errors in error_inst would manifest in normal_inst's counter on frames 2 or 3.
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);

    int error_update_count  = lua_get_instance_int(error_inst, "update_count");
    int normal_update_count = lua_get_instance_int(normal_inst, "update_count");

    primal::script::remove_for_entity(entity_a.get_id());
    primal::script::remove_for_entity(entity_b.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (error_update_count != 3) {
        std::fprintf(stderr, "error_inst update_count = %d (expected 3 — update dispatched before error in each of 3 frames)\n", error_update_count);
        return TestResult::Failed;
    }
    if (normal_update_count != 3) {
        std::fprintf(stderr, "normal_inst update_count = %d (expected 3 — must fire in all 3 frames despite other instance's error)\n", normal_update_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 15: Lua error in on_reload doesn't break the reload flow.
// Phase 2b.3's reload sequence: capture_state → destroy old → recreate →
// begin_play new → on_reload. If on_reload errors, pcall catches it and the
// reload must complete: the new instance is alive and dispatching hooks on
// the next frame_tick.
//
// Timing: script::reload(entity_id) DEFERS; the actual reload drains at the
// END of the next frame_tick (per Phase 2b.3 Test 6's pattern). So:
//   frame_tick #1: v1.update fires (v1.update_count == 1)
//   script::reload defers
//   frame_tick #2: v1.update fires (v1.update_count == 2); at END of frame,
//                  reload drains: v1 destroyed, v2 created, begin_play on v2,
//                  on_reload on v2 throws (v2.reload_count == 1)
//   frame_tick #3: v2.update fires (v2.update_count == 1)
TestResult test_lua_error_in_on_reload_does_not_break_reload() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "error_in_on_reload",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/error_in_on_reload.lua"
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

    // Frame 1: pre-reload. v1.update fires.
    primal::script::frame_tick(0.016f);

    // Defer reload — drains at end of next frame_tick.
    primal::script::reload(entity.get_id());

    // Frame 2: v1.update fires, THEN reload drains at end of frame.
    // on_reload on v2 throws; reload flow completes despite the error.
    primal::script::frame_tick(0.016f);

    // Frame 3: v2.update fires — proves reload completed.
    primal::script::frame_tick(0.016f);

    // find_instance(script_id) now returns v2 (v1 was destroyed during reload).
    auto* inst = LuaBackend::instance().find_instance(script_id);
    if (!inst) {
        std::fprintf(stderr, "find_instance returned nullptr after reload\n");
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    int update_count = lua_get_instance_int(inst, "update_count");
    int reload_count = lua_get_instance_int(inst, "reload_count");

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();

    if (update_count != 1) {
        std::fprintf(stderr, "v2.update_count = %d (expected 1 — post-reload update must fire)\n", update_count);
        return TestResult::Failed;
    }
    if (reload_count != 1) {
        std::fprintf(stderr, "v2.reload_count = %d (expected 1 — on_reload was dispatched)\n", reload_count);
        return TestResult::Failed;
    }
    return TestResult::Passed;
}

// Test 16: Lua post(fn) enqueues a callback on the engine's immediate
// post_to_main_thread queue. The callback fires on the next frame_tick drain.
// Verifies: (1) begin_play calls post(fn) — callback queued but not yet fired;
// (2) frame_tick drains the queue — callback fires, setting `fired=true`.
TestResult test_lua_post_fires_on_next_drain() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "post_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/post_script.lua"
    );
    if (type_id == u64_invalid_id) {
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst = LuaBackend::instance().find_instance(script_id);

    // (1) Callback is queued during begin_play (synchronous) but not yet fired.
    bool fired_before = lua_get_instance_bool(inst, "fired");
    if (fired_before) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // (2) Drain fires the posted callback.
    primal::script::frame_tick(0.016f);

    bool fired_after = lua_get_instance_bool(inst, "fired");
    if (!fired_after) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();
    return TestResult::Passed;
}

// Test 17: Lua post_delayed(seconds, fn) enqueues on the engine's frame-dt
// accumulator queue. The callback fires only when frame_elapsed_time_ reaches
// the delay threshold. Verifies:
//   (1) After 3 frames (0.048s accumulated < 0.050s threshold) callback has
//       NOT fired.
//   (2) After 4th frame (0.064s >= 0.050s) callback fires, setting
//       fired=true.
TestResult test_lua_post_delayed_fires_after_frame_dt_accumulates() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "post_delayed_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/post_delayed_script.lua"
    );
    if (type_id == u64_invalid_id) {
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst = LuaBackend::instance().find_instance(script_id);

    // frame_elapsed_time_ accumulator: 0 -> 0.016 -> 0.032 -> 0.048 (all < 0.050 threshold)
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);
    primal::script::frame_tick(0.016f);

    bool fired_after_3 = lua_get_instance_bool(inst, "fired");
    if (fired_after_3) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // 0.048 + 0.016 = 0.064 >= 0.050 -> callback fires this frame's drain
    primal::script::frame_tick(0.016f);

    bool fired_after_4 = lua_get_instance_bool(inst, "fired");
    if (!fired_after_4) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();
    return TestResult::Passed;
}

// Test 18: Lua post_delayed_wall(seconds, fn) enqueues on the engine's
// wall-clock (steady_clock) queue. The callback fires when real time elapses
// past the delay threshold — independent of frame dt accumulation.
// Verifies:
//   (1) First frame_tick drains but wall time elapsed is microseconds
//       (< 20ms threshold) — callback has NOT fired.
//   (2) Sleep 25ms so wall-clock now exceeds the 20ms threshold.
//   (3) Next frame_tick's drain pops the due wall entry — callback fires.
TestResult test_lua_post_delayed_wall_fires_after_wall_clock() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "post_delayed_wall_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/post_delayed_wall_script.lua"
    );
    if (type_id == u64_invalid_id) {
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst = LuaBackend::instance().find_instance(script_id);

    // (1) First frame_tick drains but wall time elapsed is microseconds (< 20ms threshold).
    primal::script::frame_tick(0.016f);
    bool fired_before = lua_get_instance_bool(inst, "fired");
    if (fired_before) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    // (2) Sleep 25ms so wall-clock now exceeds the 20ms threshold.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // (3) Next frame_tick's drain pops the due wall entry.
    primal::script::frame_tick(0.016f);
    bool fired_after = lua_get_instance_bool(inst, "fired");
    if (!fired_after) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();
    return TestResult::Passed;
}

// Test 19: Error inside a posted Lua callback does not abort the drain.
// The trampoline (lua_post_trampoline) wraps the Lua callback in lua_pcall.
// On error it logs to stderr, pops the error, and continues. This test posts
// TWO callbacks in begin_play: the first calls error(...), the second sets
// M.fired = true. After a single frame_tick drain, both should have been
// dispatched (the first errored, the second succeeded), and fired must be true.
TestResult test_lua_post_callback_error_does_not_break_drain() {
    LuaBackend::instance().initialize();
    primal::script::initialize();

    primal::game_entity::entity entity = make_test_entity();

    u64 type_id = LuaBackend::instance().register_type(
        "post_error_script",
        "EngineTest/IntegrationTests/ScriptLuaBackend/scripts/post_error_script.lua"
    );
    if (type_id == u64_invalid_id) {
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    u64 script_id = LuaBackend::instance().create_instance(type_id, entity.get_id());
    if (script_id == u64_invalid_id) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    LuaScriptInstance* inst = LuaBackend::instance().find_instance(script_id);

    // Drain fires both posted callbacks: first errors (pcall catches), second sets fired=true.
    primal::script::frame_tick(0.016f);

    bool fired = lua_get_instance_bool(inst, "fired");
    if (!fired) {
        primal::script::remove_for_entity(entity.get_id());
        primal::script::shutdown();
        LuaBackend::instance().shutdown();
        return TestResult::Failed;
    }

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    LuaBackend::instance().shutdown();
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
    suite.AddTestCase(TestCase("lua_shutdown_after_script_shutdown_safe",
                               test_lua_shutdown_after_script_shutdown_safe,
                               "LuaBackend::shutdown after script::shutdown must not crash "
                               "even with stranded subscriptions"));
    suite.AddTestCase(TestCase("lua_reload_preserves_old_state",
                               test_lua_reload_preserves_old_state,
                               "Hard reload resets update_count but on_reload migrates score"));
    suite.AddTestCase(TestCase("lua_reload_type_re_reads_source",
                               test_lua_reload_type_re_reads_source,
                               "reload_type re-reads .lua file; new instances use new source"));
    suite.AddTestCase(TestCase("lua_self_test_backend_reload_unchanged",
                               test_lua_self_test_backend_reload_unchanged,
                               "Self-Test Backend (no Phase 2b.3 hooks) reload still works"));
    suite.AddTestCase(TestCase("lua_fixed_update_and_late_update_dispatch",
                               test_lua_fixed_update_and_late_update_dispatch,
                               "Direct script::fixed_update/late_update dispatch to Lua"));
    suite.AddTestCase(TestCase("lua_frame_tick_lifecycle_order",
                               test_lua_frame_tick_lifecycle_order,
                               "frame_tick dispatches fixed→update→late in order"));
    suite.AddTestCase(TestCase("lua_self_test_backend_fixed_late_unchanged",
                               test_lua_self_test_backend_fixed_late_unchanged,
                               "Self-Test Backend fixed/late counters increment (engine regression)"));
    suite.AddTestCase(TestCase("lua_error_in_begin_play_survives",
                               test_lua_error_in_begin_play_survives,
                               "Lua error in begin_play is caught; instance remains usable"));
    suite.AddTestCase(TestCase("lua_error_in_update_does_not_block_late_update",
                               test_lua_error_in_update_does_not_block_late_update,
                               "Lua error in update doesn't block late_update (same/next frame)"));
    suite.AddTestCase(TestCase("lua_error_in_one_instance_does_not_affect_another",
                               test_lua_error_in_one_instance_does_not_affect_another,
                               "Lua error in one instance doesn't affect another (shared lua_State)"));
    suite.AddTestCase(TestCase("lua_error_in_on_reload_does_not_break_reload",
                               test_lua_error_in_on_reload_does_not_break_reload,
                               "Lua error in on_reload doesn't break reload flow"));
    suite.AddTestCase(TestCase("lua_post_fires_on_next_drain",
                               test_lua_post_fires_on_next_drain,
                               "Lua post(fn) enqueues on engine immediate queue; "
                               "callback fires on next drain"));
    suite.AddTestCase(TestCase("lua_post_delayed_fires_after_frame_dt_accumulates",
                               test_lua_post_delayed_fires_after_frame_dt_accumulates,
                               "Frame-dt accumulator gates delayed callback firing"));
    suite.AddTestCase(TestCase("lua_post_delayed_wall_fires_after_wall_clock",
                               test_lua_post_delayed_wall_fires_after_wall_clock,
                               "steady_clock drives wall-time delayed callback firing"));
    suite.AddTestCase(TestCase("lua_post_callback_error_does_not_break_drain",
                               test_lua_post_callback_error_does_not_break_drain,
                               "Error in a posted callback doesn't abort the drain"));
    suite.RunAllTests();
}

int main() {
    RunLuaBackendTests();
    return 0;
}
