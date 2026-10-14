// TestNavigationRuntime.cpp — ai-navigation S1 收口单测（无 GPU 依赖）。
//
// 验证链路：game_entity 实体 → navigation::runtime 全局资产/任务面 → tick
// 推进 + 实体 transform 回写（与 PipelineTick 的 nav 段同一实现）。
// 对应合同：A01 到达/事件恰一次、A02 取消/类型化失败、stale_nav 代际。

#include "NavigationTestFixtures.h"
#include "Navigation/NavigationRuntime.h"
#include "Components/Transform.h"
#include "Components/Entity.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"

#include <cmath>
#include <iostream>

using namespace primal;
using namespace primal::navigation;
namespace math = primal::math;
namespace navrt = primal::navigation::runtime;
namespace ntest = primal::navigation::test;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestNavigationRuntime] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestNavigationRuntime] PASS: " << name << std::endl;
    }
}

game_entity::entity make_entity(const math::v3& pos)
{
    transform::init_info tf{};
    tf.position[0] = pos.x; tf.position[1] = pos.y; tf.position[2] = pos.z;
    tf.rotation[0] = 0.f; tf.rotation[1] = 0.f; tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;
    tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;
    game_entity::entity_info info{};
    info.transform = &tf;
    return game_entity::create(info);
}


bool bake_flat_runtime()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 12.f, 12.f);
    return navrt::bake(mb.input(), primal::navigation::agent_profile{});
}

void test_entity_driven_movement()
{
    navrt::clear();
    check(bake_flat_runtime(), "runtime: bake asset");

    game_entity::entity e = make_entity(math::v3{ 1.f, 0.f, 1.f });
    check(e.is_valid(), "runtime: entity valid");

    const math::v3 target{ 10.f, 0.f, 10.f };
    const nav_error err = navrt::move_to(e.get_id(), target, 2.f);
    check(err == nav_error::ok, "runtime: move_to ok");
    check(navrt::state_of(e.get_id()) == move_state::moving, "runtime: state moving");

    for (int i = 0; i < 600; ++i) navrt::tick(1.f / 60.f);
    check(navrt::state_of(e.get_id()) == move_state::completed, "runtime: completed");
    const math::v3 p = e.position();
    const float d = sqrtf(powf(p.x - target.x, 2.f) + powf(p.z - target.z, 2.f));
    check(d <= 0.1f + 1e-4f, "runtime: entity within 0.1m of target");

}

void test_stop_and_replace()
{
    navrt::clear();
    check(bake_flat_runtime(), "stop: bake asset");
    game_entity::entity e = make_entity(math::v3{ 1.f, 0.f, 6.f });

    check(navrt::move_to(e.get_id(), math::v3{ 11.f, 0.f, 6.f }, 2.f) == nav_error::ok,
          "stop: first task ok");
    for (int i = 0; i < 60; ++i) navrt::tick(1.f / 60.f);
    navrt::stop(e.get_id());
    check(navrt::state_of(e.get_id()) == move_state::cancelled, "stop: cancelled");
    const math::v3 frozen = e.position();
    for (int i = 0; i < 60; ++i) navrt::tick(1.f / 60.f);
    check(ntest::near3(e.position(), frozen, 1e-6f), "stop: transform frozen after cancel");

    // 替换语义：同实体再 move_to = 新任务
    check(navrt::move_to(e.get_id(), math::v3{ 6.f, 0.f, 11.f }, 2.f) == nav_error::ok,
          "stop: replace with new task");
    check(navrt::state_of(e.get_id()) == move_state::moving, "stop: new task moving");

}

void test_typed_failures_and_stale()
{
    // 无资产 → no_nav_data（且不留半成品任务）
    navrt::clear();
    game_entity::entity e = make_entity(math::v3{ 1.f, 0.f, 1.f });
    check(navrt::move_to(e.get_id(), math::v3{ 5.f, 0.f, 5.f }, 2.f) == nav_error::no_nav_data,
          "typed: no_nav_data without asset");
    check(navrt::state_of(e.get_id()) == move_state::idle, "typed: no leftover task");

    // 有资产但目标不可达（断开岛）→ unreachable
    check(bake_flat_runtime(), "typed: bake asset");
    {
        ntest::mesh_builder isl;
        isl.quad_y(0.f, 0.f, 0.f, 2.f, 2.f);
        isl.quad_y(0.f, 5.f, 0.f, 7.f, 2.f);
        // 换成断开场景资产（替换全局资产）
        check(navrt::bake(isl.input(), primal::navigation::agent_profile{}), "typed: bake islands");
        game_entity::entity walker = make_entity(math::v3{ 1.f, 0.f, 1.f });
        check(navrt::move_to(walker.get_id(), math::v3{ 6.f, 0.f, 1.f }, 2.f) == nav_error::unreachable,
              "typed: unreachable across islands");
    }

    // stale_nav：任务进行中重烘焙 → 下一 tick 任务失败
    check(bake_flat_runtime(), "stale: bake flat");
    game_entity::entity e2 = make_entity(math::v3{ 1.f, 0.f, 1.f });
    check(navrt::move_to(e2.get_id(), math::v3{ 10.f, 0.f, 10.f }, 2.f) == nav_error::ok,
          "stale: task created");
    const u32 gen_before = navrt::generation();
    check(bake_flat_runtime(), "stale: rebake");
    check(navrt::generation() != gen_before, "stale: generation advanced");
    navrt::tick(1.f / 60.f);
    check(navrt::state_of(e2.get_id()) == move_state::failed
              && navrt::failure_of(e2.get_id()) == nav_error::stale_nav,
          "stale: task failed with stale_nav");

    navrt::clear();
}

} // anonymous namespace

int main()
{
    std::cout << "[TestNavigationRuntime] === begin ===" << std::endl;
    test_entity_driven_movement();
    test_stop_and_replace();
    test_typed_failures_and_stale();
    std::cout << "[TestNavigationRuntime] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
