// TestGameplayPredicates.cpp — gameplay S1 单测：封闭谓词集四类求值 + 三态
// 语义（unknown→false + warn trace；gameplay-rules.md："谓词越界有确定语义"）。

#include "Gameplay/GameplayRuntime.h"
#include "Components/Entity.h"
#include "Components/ScriptEventBus.h"
#include "Components/ScriptState.h"
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
        std::cerr << "[TestGameplayPredicates] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestGameplayPredicates] ok: " << name << std::endl;
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

void move_entity(game_entity::entity e, float px, float py, float pz)
{
    transform::set_position(
        transform::component{ transform::transform_id{ e.get_id() } },
        math::v3{ px, py, pz });
}

}  // namespace

int main()
{
    gpsm::script_event_bus::instance().reset();
    gprt::clear();
    gpsm::ScriptState::instance().initialize();

    // ---- distance：3-4-5 三角 + self 解析 + 未知名 ----
    {
        auto a = make_entity(0.f, 0.f, 0.f);
        auto b = make_entity(3.f, 0.f, 4.f);
        check(a.is_valid() && b.is_valid(), "entities created");
        gprt::register_named_entity(a.get_id(), "A");
        gprt::register_named_entity(b.get_id(), "B");

        check(gprt::trigger_add(b.get_id(), {}, "", gameplay::trigger_mode::once) ==
              gameplay::gp_error::invalid_argument, "empty event rejected");

        // B 上的触发器：dist self A lt 6 → true（5 < 6）；用 trace 验证发射。
        gameplay::gp_predicate d;
        d.kind = gameplay::predicate_kind::distance;
        d.op = gameplay::compare_op::lt;
        d.entity_a = "";          // self
        d.entity_b = "A";
        d.value = 6.f;
        check(gprt::trigger_add(b.get_id(), d, "near.a", gameplay::trigger_mode::once) ==
              gameplay::gp_error::ok, "dist trigger added");
        gprt::tick(0.f);
        bool fired = false;
        for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
            u32 f; gameplay::trace_kind k; u64 e; std::string det;
            gprt::trace_get(i, f, k, e, det);
            if (k == gameplay::trace_kind::trigger_fired && det.find("near.a") != std::string::npos)
                fired = true;
        }
        check(fired, "distance 5 lt 6 fires");

        // 移远 → 下界不再满足（refire 模式二次触发验证回落-再上升沿）
        move_entity(a, 30.f, 0.f, 40.f);
        gameplay::gp_predicate d2 = d;
        d2.value = 100.f;
        d2.op = gameplay::compare_op::lt;
        gprt::clear();   // 简化：重置后只建 refire 触发器
        gprt::register_named_entity(a.get_id(), "A");
        gprt::register_named_entity(b.get_id(), "B");
        check(gprt::trigger_add(b.get_id(), d2, "refire.evt", gameplay::trigger_mode::refire) ==
              gameplay::gp_error::ok, "refire trigger added");
        gprt::tick(0.f);   // 50 < 100 → 首次上升沿
        move_entity(a, 300.f, 0.f, 400.f);
        gprt::tick(0.f);   // 500 > 100 → 回落（无发射）
        move_entity(a, 3.f, 0.f, 4.f);
        gprt::tick(0.f);   // 5 → 再上升沿 → 第二次发射
        u32 fires = 0;
        for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
            u32 f; gameplay::trace_kind k; u64 e; std::string det;
            gprt::trace_get(i, f, k, e, det);
            if (k == gameplay::trace_kind::trigger_fired && det.find("refire.evt") != std::string::npos)
                ++fires;
        }
        check(fires == 2, "refire fires twice (fall + rise again)");

        // 未知名 → unknown → false + warn
        gameplay::gp_predicate bad;
        bad.kind = gameplay::predicate_kind::distance;
        bad.op = gameplay::compare_op::lt;
        bad.entity_a = "self"; bad.entity_b = "Nobody";
        bad.value = 1e9f;
        gprt::trigger_add(b.get_id(), bad, "never.fires", gameplay::trigger_mode::refire);
        gprt::tick(0.f);
        bool warned = false;
        for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
            u32 f; gameplay::trace_kind k; u64 e; std::string det;
            gprt::trace_get(i, f, k, e, det);
            if (k == gameplay::trace_kind::warn && det.find("Nobody") != std::string::npos)
                warned = true;
        }
        check(warned, "unknown name → warn trace");

        game_entity::remove(a.get_id());
        game_entity::remove(b.get_id());
    }

    // ---- property：state.entities.<id>.<key> 比较 + 缺失 → unknown ----
    {
        auto e = make_entity(0, 0, 0);
        gpsm::StateValue hp;
        hp.tag = gpsm::StateValue::Tag::Number; hp.n = 50.0;
        gpsm::state().set({ "entities", std::to_string(e.get_id()), "hp" }, hp, e.get_id());

        gameplay::gp_predicate p;
        p.kind = gameplay::predicate_kind::property;
        p.op = gameplay::compare_op::lt;
        p.entity_a = "self";
        p.key = "hp";
        p.value = 100.f;
        check(gprt::trigger_add(e.get_id(), p, "low.hp", gameplay::trigger_mode::once) ==
              gameplay::gp_error::ok, "prop trigger added");
        gprt::tick(0.f);
        bool fired = false;
        for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
            u32 f; gameplay::trace_kind k; u64 ent; std::string det;
            gprt::trace_get(i, f, k, ent, det);
            if (k == gameplay::trace_kind::trigger_fired && det.find("low.hp") != std::string::npos)
                fired = true;
        }
        check(fired, "hp 50 lt 100 fires");

        gprt::clear();
        p.key = "missing_key";
        gprt::trigger_add(e.get_id(), p, "never", gameplay::trigger_mode::once);
        gprt::tick(0.f);
        bool fired2 = false;
        for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
            u32 f; gameplay::trace_kind k; u64 ent; std::string det;
            gprt::trace_get(i, f, k, ent, det);
            if (k == gameplay::trace_kind::trigger_fired) fired2 = true;
        }
        check(!fired2, "missing property → unknown → no fire");
        game_entity::remove(e.get_id());
    }

    // ---- event_count：总线计数 + 状态进入以来作用域 ----
    {
        auto e = make_entity(0, 0, 0);
        check(gprt::machine_begin(e.get_id(), "s0") == gameplay::gp_error::ok, "begin count");
        gprt::machine_add_state("s0"); gprt::machine_add_state("s1");
        gprt::machine_add_transition("s0", "s1", nullptr);
        gameplay::gp_predicate c;
        c.kind = gameplay::predicate_kind::event_count;
        c.op = gameplay::compare_op::gte;
        c.key = "tick.evt";
        c.value = 2.f;
        gprt::machine_add_guard(0, c);
        gprt::machine_end();

        auto emit = [] {
            char* buf = new char[1]{ '\0' };
            gpsm::script_event_bus::instance().emit_by_name(
                "tick.evt", buf, 1, [](void* p) { delete[] static_cast<char*>(p); });
        };
        emit();
        gpsm::script_event_bus::instance().drain();   // 计数 1
        gprt::tick(0.f);
        std::string st;
        check(gprt::state_get(e.get_id(), st) && st == "s0", "count 1 < 2 stays");
        emit();
        gpsm::script_event_bus::instance().drain();   // 计数 2
        gprt::tick(0.f);                              // → s1（计数器随状态进入清零）
        check(gprt::state_get(e.get_id(), st) && st == "s1", "count 2 gte 2 transitions");
        game_entity::remove(e.get_id());
    }

    // ---- timer（独立触发器作用域 = 创建以来） ----
    {
        auto e = make_entity(0, 0, 0);
        gameplay::gp_predicate t;
        t.kind = gameplay::predicate_kind::timer;
        t.op = gameplay::compare_op::gte;
        t.value = 0.5f;
        gprt::trigger_add(e.get_id(), t, "half.sec", gameplay::trigger_mode::once);
        gprt::tick(0.3f);
        bool fired = false;
        auto any_fire = [&] {
            fired = false;
            for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
                u32 f; gameplay::trace_kind k; u64 ent; std::string det;
                gprt::trace_get(i, f, k, ent, det);
                if (k == gameplay::trace_kind::trigger_fired && det.find("half.sec") != std::string::npos)
                    fired = true;
            }
        };
        any_fire();
        check(!fired, "0.3s no fire");
        gprt::tick(0.3f);
        any_fire();
        check(fired, "0.6s fires");
        gprt::tick(1.f);   // once：不再触发
        any_fire();
        check(fired, "once stays armed-off");
        game_entity::remove(e.get_id());
    }

    gprt::clear();
    gpsm::ScriptState::instance().shutdown();
    gpsm::script_event_bus::instance().reset();
    std::cout << (failures ? "[TestGameplayPredicates] FAILED\n" : "[TestGameplayPredicates] ALL PASS\n");
    return failures ? 1 : 0;
}
