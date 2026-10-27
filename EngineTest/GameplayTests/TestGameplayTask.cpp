// TestGameplayTask.cpp — gameplay S2 单测：任务契约（条件终局/Lua 终局/
// count 作用域/终态先到先得/trace）。

#include "Gameplay/GameplayRuntime.h"
#include "Components/Entity.h"
#include "Components/ScriptEventBus.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"

#include <iostream>
#include <string>

using namespace primal;
namespace gprt = primal::gameplay::runtime;
namespace gpsm = primal::script;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestGameplayTask] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestGameplayTask] ok: " << name << std::endl;
    }
}

game_entity::entity make_entity(float px, float py, float pz)
{
    transform::init_info ti{};
    ti.position[0] = px; ti.position[1] = py; ti.position[2] = pz;
    ti.rotation[3] = 1.f;
    game_entity::entity_info ei{};
    ei.transform = &ti;
    script::init_info si{};
    ei.script = &si;
    return game_entity::entity{ game_entity::create(ei) };
}

bool task_seen(u32 want_kind, const char* want_id, const char* want_cause)
{
    for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
        u32 f; gameplay::trace_kind k; u64 e; std::string det;
        gprt::trace_get(i, f, k, e, det);
        if ((u32)k == want_kind && det.find(want_id) != std::string::npos &&
            det.find(want_cause) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

int main()
{
    gpsm::script_event_bus::instance().reset();
    gprt::clear();

    // ---- 条件终局：timer 完成集 ----
    {
        auto e = make_entity(0, 0, 0);
        check(gprt::task_begin(e.get_id(), "t1") == gameplay::gp_error::ok, "task begin");
        check(gprt::task_begin(e.get_id(), "t2") == gameplay::gp_error::busy, "double begin busy");
        gameplay::gp_predicate timer_done;
        timer_done.kind = gameplay::predicate_kind::timer;
        timer_done.op = gameplay::compare_op::gte;
        timer_done.value = 1.f;
        check(gprt::task_add_condition(0, timer_done) == gameplay::gp_error::ok, "complete cond");
        check(gprt::task_add_condition(2, timer_done) == gameplay::gp_error::invalid_argument, "bad kind");
        check(gprt::task_end() == gameplay::gp_error::ok, "task end");

        gameplay::task_state st{};
        check(gprt::task_get(e.get_id(), "t1", st) && st == gameplay::task_state::active, "active");
        gprt::tick(0.5f);
        check(gprt::task_get(e.get_id(), "t1", st) && st == gameplay::task_state::active, "0.5s still active");
        gprt::tick(0.6f);
        check(gprt::task_get(e.get_id(), "t1", st) && st == gameplay::task_state::completed, "1.1s completed");
        check(task_seen((u32)gameplay::trace_kind::task_completed, "t1", "cause=cond"),
              "trace completed cause=cond");

        // 重复 id 拒绝
        check(gprt::task_begin(e.get_id(), "t1") == gameplay::gp_error::already_exists, "dup id rejected");
        game_entity::remove(e.get_id());
    }

    // ---- 失败集优先序：完成集与失败集同时真 → 完成集先判（先到先得=完成优先） ----
    {
        gprt::clear();
        gpsm::script_event_bus::instance().reset();
        auto e = make_entity(1, 0, 0);
        gprt::task_begin(e.get_id(), "both");
        gameplay::gp_predicate t0;   // 0 秒即真
        t0.kind = gameplay::predicate_kind::timer; t0.op = gameplay::compare_op::gte; t0.value = 0.f;
        gprt::task_add_condition(0, t0);   // complete
        gprt::task_add_condition(1, t0);   // fail
        gprt::task_end();
        gprt::tick(0.1f);
        gameplay::task_state st{};
        check(gprt::task_get(e.get_id(), "both", st) && st == gameplay::task_state::completed,
              "complete set evaluated first");
        game_entity::remove(e.get_id());
    }

    // ---- Lua 终局：task_force 幂等失败 + trace cause=lua ----
    {
        gprt::clear();
        gpsm::script_event_bus::instance().reset();
        auto e = make_entity(2, 0, 0);
        gprt::task_begin(e.get_id(), "collect");
        gameplay::gp_predicate never;
        never.kind = gameplay::predicate_kind::event_count;
        never.op = gameplay::compare_op::gte; never.key = "nope"; never.value = 99.f;
        gprt::task_add_condition(0, never);
        gprt::task_end();

        std::string detail;
        check(gprt::task_force(e.get_id(), "collect", true, detail) == gameplay::gp_error::ok, "force complete ok");
        check(gprt::task_force(e.get_id(), "collect", false, detail) == gameplay::gp_error::already_exists,
              "force after terminal rejected");
        check(detail.find("already_completed") != std::string::npos, "detail says already");
        check(gprt::task_force(e.get_id(), "ghost", true, detail) == gameplay::gp_error::no_machine,
              "unknown task");
        check(task_seen((u32)gameplay::trace_kind::task_completed, "collect", "cause=lua"),
              "trace completed cause=lua");
        game_entity::remove(e.get_id());
    }

    // ---- count 作用域：任务创建以来（非机器状态） ----
    {
        gprt::clear();
        gpsm::script_event_bus::instance().reset();
        auto e = make_entity(3, 0, 0);
        gprt::task_begin(e.get_id(), "cnt");
        gameplay::gp_predicate c2;
        c2.kind = gameplay::predicate_kind::event_count;
        c2.op = gameplay::compare_op::gte; c2.key = "ev"; c2.value = 2.f;
        gprt::task_add_condition(0, c2);
        gprt::task_end();

        auto emit = [] {
            char* buf = new char[1]{ '\0' };
            gpsm::script_event_bus::instance().emit_by_name(
                "ev", buf, 1, [](void* p) { delete[] static_cast<char*>(p); });
        };
        emit();
        gpsm::script_event_bus::instance().drain();
        gprt::tick(0.f);
        gameplay::task_state st{};
        check(gprt::task_get(e.get_id(), "cnt", st) && st == gameplay::task_state::active, "count 1 active");
        emit();
        gpsm::script_event_bus::instance().drain();
        gprt::tick(0.f);
        check(gprt::task_get(e.get_id(), "cnt", st) && st == gameplay::task_state::completed, "count 2 completes");
        game_entity::remove(e.get_id());
    }

    gprt::clear();
    gpsm::script_event_bus::instance().reset();
    std::cout << (failures ? "[TestGameplayTask] FAILED\n" : "[TestGameplayTask] ALL PASS\n");
    return failures ? 1 : 0;
}
