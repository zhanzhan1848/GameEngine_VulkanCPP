// TestGameplayTrace.cpp — gameplay S1 单测：同输入同帧序同事件流可回放
//（gameplay-rules.md S1 验收冻结项）。跑两轮完全相同的输入序列，逐条比较
// trace（kind+frame+detail；entity id 含 generation 位跨轮不同，不参与比较）。

#include "Gameplay/GameplayRuntime.h"
#include "Components/Entity.h"
#include "Components/ScriptEventBus.h"
#include "Components/Transform.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"

#include <iostream>
#include <string>
#include <vector>

using namespace primal;
namespace gprt = primal::gameplay::runtime;
namespace gpsm = primal::script;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestGameplayTrace] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestGameplayTrace] ok: " << name << std::endl;
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

std::vector<std::string> run_scenario()
{
    gprt::clear();
    gpsm::script_event_bus::instance().reset();

    auto player = make_entity(10.f, 0.f, 0.f);
    auto button = make_entity(0.f, 0.f, 0.f);
    gprt::register_named_entity(player.get_id(), "Player");
    gprt::register_named_entity(button.get_id(), "Button");

    // Button：触发器 dist self Player lt 2 → button.near（refire：远离再靠近可再触发）
    gameplay::gp_predicate d;
    d.kind = gameplay::predicate_kind::distance;
    d.op = gameplay::compare_op::lt;
    d.entity_a = ""; d.entity_b = "Player";
    d.value = 2.f;
    gprt::trigger_add(button.get_id(), d, "button.near", gameplay::trigger_mode::refire);

    // Player：机器 far→near（guard 距离 Player→Button），near→far 反向。
    // 注意 guard 的 self 是机器 owner=Player，第二实体须显式指 Button。
    gprt::machine_begin(player.get_id(), "far");
    gprt::machine_add_state("far"); gprt::machine_add_state("near");
    gprt::machine_add_transition("far", "near", "player.near");
    gprt::machine_add_transition("near", "far", "player.far");
    gameplay::gp_predicate close = d, away = d;
    close.entity_a = ""; close.entity_b = "Button";
    away.entity_a = ""; away.entity_b = "Button";
    away.op = gameplay::compare_op::gte;
    gprt::machine_add_guard(0, close);
    gprt::machine_add_guard(1, away);
    gprt::machine_end();

    // 输入序列（固定）：靠近 → 远离 → 再靠近。transform 每帧显式写入。
    for (int f = 0; f < 60; ++f) {
        float x;
        if (f < 20)       x = 10.f - 0.6f * f;        // 10 → -2（路过按钮）
        else if (f < 40)  x = -2.f + 0.6f * (f - 20); // -2 → 10（离开）
        else              x = 10.f - 0.6f * (f - 40); // 10 → -2（再来）
        transform::set_position(
            transform::component{ transform::transform_id{ player.get_id() } },
            math::v3{ x, 0.f, 0.f });
        gprt::tick(1.f / 60.f);
    }

    std::vector<std::string> out;
    for (u32 i = 0; i < gprt::trace_record_count(); ++i) {
        u32 frame; gameplay::trace_kind kind; u64 entity; std::string det;
        gprt::trace_get(i, frame, kind, entity, det);
        out.push_back(std::to_string(frame) + "|" + std::to_string((u32)kind) + "|" + det);
    }

    game_entity::remove(player.get_id());
    game_entity::remove(button.get_id());
    return out;
}

}  // namespace

int main()
{
    const auto run1 = run_scenario();
    const auto run2 = run_scenario();

    check(!run1.empty(), "scenario produced records");
    check(run1.size() > 4, "enough records (auto+lua-less triggers multiple)");

    bool identical = run1.size() == run2.size();
    if (identical)
        for (size_t i = 0; i < run1.size(); ++i)
            if (run1[i] != run2[i]) { identical = false; break; }
    check(identical, "two identical runs → identical trace stream");

    // 结构抽检：包含两类 trigger_fired 事件与 player.near/far 状态转移
    bool near_fired = false, went_near = false, went_far = false;
    for (auto& r : run1) {
        if (r.find("button.near") != std::string::npos && r.find("|0|") != std::string::npos) near_fired = true;
        if (r.find("|1|") != std::string::npos && r.find("to=near") != std::string::npos) went_near = true;
        if (r.find("|1|") != std::string::npos && r.find("to=far") != std::string::npos) went_far = true;
    }
    check(near_fired, "button.near trigger fired");
    check(went_near && went_far, "auto transitions both directions");

    gprt::clear();
    gpsm::script_event_bus::instance().reset();
    std::cout << (failures ? "[TestGameplayTrace] FAILED\n" : "[TestGameplayTrace] ALL PASS\n");
    return failures ? 1 : 0;
}
