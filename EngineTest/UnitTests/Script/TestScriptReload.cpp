// === Phase 1 Task 7: Hot Reload 测试 ===
//
// 验证 external_script 热重载机制:
//   1) reload(eid) deferred 到 frame_tick 末尾处理
//   2) 旧实例 destroy() 被调,新实例 begin_play + on_reload 被调
//   3) 同帧多次 reload 去重(只处理一次)
//   4) 同帧 reload + remove:remove 胜,reload no-op
//
// Phase 1 限制:
//   - 只测 external_script(native C++ reload 是 Phase 2)
//   - external_script 无 C ABI 设 reload_state,on_reload 收 nullptr
//
// 设计说明:
//   - 与 TestScriptExternal.cpp 同模式:手动 entity 创建 + C ABI 注册 + script::initialize/shutdown。
//   - 每个测试自包含,shutdown() 后 deferred_reloads_ 清空,不跨测试污染。

#include "../TestFramework.h"
#include "Components/ScriptExternal.h"
#include "Components/Script.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"

#include <cstdio>

using Engine::Test::TestCase;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;

namespace {

// Helper: 创建一个带最小化 transform 的 entity
static primal::game_entity::entity make_test_entity() {
    primal::transform::init_info tinfo{};
    primal::game_entity::entity_info info{};
    info.transform = &tinfo;
    return primal::game_entity::create(info);
}

// ----------------------------------------------------------------------------
// Shared callback state: counters for each lifecycle hook.
// begin_play/destroy/on_reload 各加一,让我们验证 reload 时新实例的 begin_play
// 和 on_reload 被调、旧实例的 destroy 被调。
// ----------------------------------------------------------------------------
struct reload_counters {
    int begin_play;
    int update;
    int destroy;
    int on_reload;
    void* last_old_state;  // on_reload 收到的 old_state 指针
};

void reload_begin_play(void* ud) {
    auto* s = static_cast<reload_counters*>(ud);
    ++s->begin_play;
}

void reload_update(void* ud, float) {
    auto* s = static_cast<reload_counters*>(ud);
    ++s->update;
}

void reload_destroy(void* ud) {
    auto* s = static_cast<reload_counters*>(ud);
    ++s->destroy;
}

void reload_on_reload(void* ud, void* old_state) {
    auto* s = static_cast<reload_counters*>(ud);
    ++s->on_reload;
    s->last_old_state = old_state;
}

// Helper: 注册一个 standard reload-test external type。
// 返回 type_id 或 u64_invalid_id。
static u64 register_reload_test_type(reload_counters* state, const char* name) {
    script_external_callbacks cbs{};
    cbs.begin_play = &reload_begin_play;
    cbs.update = &reload_update;
    cbs.destroy = &reload_destroy;
    cbs.on_reload = &reload_on_reload;
    return script_register_external(name, state, &cbs);
}

// ----------------------------------------------------------------------------
// Test 1: reload replaces instance
//
// Register an external type with begin_play/update/destroy/on_reload callbacks.
// Create entity + script. frame_tick (update fires). reload(eid). frame_tick
// (deferred reload runs). Verify:
//   - destroy called once on old instance (1 from reload)
//   - begin_play called twice (1 from create + 1 from reload)
//   - on_reload called once with nullptr (Phase 1: external can't set state)
//   - update called twice total (one per frame_tick, new instance ticks on 2nd)
// ----------------------------------------------------------------------------
TestResult test_external_reload_replaces_instance() {
    primal::script::initialize();

    reload_counters state{0, 0, 0, 0, nullptr};

    u64 type_id = register_reload_test_type(&state, "reload_replaces");
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // begin_play fired at create time
    if (state.begin_play != 1) {
        std::fprintf(stderr, "after create: begin_play=%d (expected 1)\n", state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // frame_tick 1: update fires
    primal::script::frame_tick(0.016f);
    if (state.update != 1) {
        std::fprintf(stderr, "after frame 1: update=%d (expected 1)\n", state.update);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Request reload
    primal::script::reload(entity.get_id());

    // frame_tick 2: deferred reload processes at end of frame
    // Sequence: drain_callbacks → fixed_update → update (old instance, update=2) →
    //           late_update → drain_events → process_deferred_reloads
    //           (destroy old, begin_play new, on_reload new)
    primal::script::frame_tick(0.016f);

    // After reload: destroy called once (old instance), begin_play total 2,
    // on_reload called once with nullptr
    if (state.destroy != 1) {
        std::fprintf(stderr, "after reload: destroy=%d (expected 1)\n", state.destroy);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.begin_play != 2) {
        std::fprintf(stderr, "after reload: begin_play=%d (expected 2)\n", state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.on_reload != 1) {
        std::fprintf(stderr, "after reload: on_reload=%d (expected 1)\n", state.on_reload);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.last_old_state != nullptr) {
        std::fprintf(stderr, "after reload: last_old_state=%p (expected nullptr, Phase 1 limit)\n",
                     state.last_old_state);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // frame_tick 3: new instance should update (update=3 total: 1 old + 1 during reload frame + 1 new)
    primal::script::frame_tick(0.016f);
    if (state.update != 3) {
        std::fprintf(stderr, "after frame 3: update=%d (expected 3)\n", state.update);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Cleanup
    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 2: reload deduped same frame
//
// Call reload(eid) 3 times for the same entity. frame_tick processes deferred
// queue. Verify only ONE destroy/begin_play/on_reload cycle (not 3).
// ----------------------------------------------------------------------------
TestResult test_reload_deduped_same_frame() {
    primal::script::initialize();

    reload_counters state{0, 0, 0, 0, nullptr};

    u64 type_id = register_reload_test_type(&state, "reload_dedup");
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // begin_play from create
    if (state.begin_play != 1) {
        std::fprintf(stderr, "after create: begin_play=%d (expected 1)\n", state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Request reload 3 times
    primal::script::reload(entity.get_id());
    primal::script::reload(entity.get_id());
    primal::script::reload(entity.get_id());

    // frame_tick: deferred dedup should collapse 3 → 1
    primal::script::frame_tick(0.016f);

    // Only one reload cycle should have happened
    if (state.destroy != 1) {
        std::fprintf(stderr, "after dedup: destroy=%d (expected 1)\n", state.destroy);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.begin_play != 2) {
        std::fprintf(stderr, "after dedup: begin_play=%d (expected 2: 1 create + 1 reload)\n",
                     state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.on_reload != 1) {
        std::fprintf(stderr, "after dedup: on_reload=%d (expected 1)\n", state.on_reload);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::script::remove_for_entity(entity.get_id());
    primal::script::shutdown();
    return TestResult::Passed;
}

// ----------------------------------------------------------------------------
// Test 3: reload and remove same frame (remove wins)
//
// reload(eid) then remove_for_entity(eid) before frame_tick.
// remove_for_entity immediately calls remove() → swap-erase → clears
// entity_to_script[entity_index]. When frame_tick's process_deferred_reloads
// runs, the lookup finds invalid entity_to_script entry → skips. No crash,
// no reload cycle (no destroy/begin_play/on_reload from reload path).
//
// Note: remove_for_entity DOES call destroy() on the script (1 destroy from
// removal path). The reload path contributes 0 because entity is gone.
// ----------------------------------------------------------------------------
TestResult test_reload_and_remove_same_frame() {
    primal::script::initialize();

    reload_counters state{0, 0, 0, 0, nullptr};

    u64 type_id = register_reload_test_type(&state, "reload_remove");
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::game_entity::entity entity = make_test_entity();
    u64 script_id = script_create_external(type_id, (u64)entity.get_id());
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "script_create_external failed\n");
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // begin_play from create
    if (state.begin_play != 1) {
        std::fprintf(stderr, "after create: begin_play=%d (expected 1)\n", state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // Reload then remove before frame_tick
    primal::script::reload(entity.get_id());
    primal::script::remove_for_entity(entity.get_id());

    // remove_for_entity called destroy() → state.destroy == 1
    if (state.destroy != 1) {
        std::fprintf(stderr, "after remove: destroy=%d (expected 1 from removal)\n", state.destroy);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    // frame_tick: deferred reload tries but entity_to_script is invalid → skip
    primal::script::frame_tick(0.016f);

    // Reload path should NOT have fired again
    if (state.destroy != 1) {
        std::fprintf(stderr, "after frame: destroy=%d (expected still 1, reload no-op)\n",
                     state.destroy);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.begin_play != 1) {
        std::fprintf(stderr, "after frame: begin_play=%d (expected still 1, no reload)\n",
                     state.begin_play);
        primal::script::shutdown();
        return TestResult::Failed;
    }
    if (state.on_reload != 0) {
        std::fprintf(stderr, "after frame: on_reload=%d (expected 0, remove wins)\n",
                     state.on_reload);
        primal::script::shutdown();
        return TestResult::Failed;
    }

    primal::script::shutdown();
    return TestResult::Passed;
}

} // anonymous namespace

void RunScriptReloadTests() {
    TestSuite suite("Script.Reload");
    suite.AddTestCase(TestCase("external_reload_replaces_instance",
                               test_external_reload_replaces_instance,
                               "reload(eid) on external_script: old instance destroyed, "
                               "new instance begin_play + on_reload(nullptr) called"));
    suite.AddTestCase(TestCase("reload_deduped_same_frame",
                               test_reload_deduped_same_frame,
                               "3x reload(eid) same frame: dedup collapses to 1 cycle"));
    suite.AddTestCase(TestCase("reload_and_remove_same_frame",
                               test_reload_and_remove_same_frame,
                               "reload + remove before frame_tick: remove wins, reload no-op"));
    suite.RunAllTests();
}

int main() {
    RunScriptReloadTests();
    return 0;
}
