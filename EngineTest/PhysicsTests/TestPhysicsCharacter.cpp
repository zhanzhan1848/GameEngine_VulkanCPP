// TestPhysicsCharacter.cpp — physics S2 基础角色移动与诊断（无 GPU 依赖）。
//
// physics-S2-A01/A02 语义：意图驱动速度（水平=意图×速度、垂直保留）；
// 平地行走位移≈速度×时间；零意图停止；地面 grounded=true / 空中 false；
// 挡墙卡住诊断（意图非零但位移<期望 30% 持续窗口）→ stuck=true；
// remove 后状态查询 false；非法参数拒绝。

#include "CommonHeaders.h"
#include "Simulation/SimulationAPI.h"

#include <cmath>
#include <iostream>

using namespace primal;
namespace sim = primal::simulation;

namespace
{
int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestPhysicsCharacter] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestPhysicsCharacter] PASS: " << name << std::endl;
    }
}

sim::body_desc ground_desc()
{
    sim::body_desc d{};
    d.s = sim::body_desc::shape::box;
    d.m = sim::body_desc::mode::static_body;
    d.half_extents = math::v3{ 50.f, 0.5f, 50.f };
    d.position = math::v3{ 0, -0.5f, 0 };
    return d;
}

sim::body_desc wall_desc(f32 x)
{
    sim::body_desc d{};
    d.s = sim::body_desc::shape::box;
    d.m = sim::body_desc::mode::static_body;
    d.half_extents = math::v3{ 0.5f, 3.f, 5.f };
    d.position = math::v3{ x, 3, 0 };
    return d;
}

void test_walk_stop_grounded()
{
    sim::initialize(sim::settings{});
    sim::body_create(ground_desc());

    sim::character_desc cd{};
    cd.position = math::v3{ 0, 1.0f, 0 };   // 半高 0.9 → 脚底 0.1 悬空落地
    cd.walk_speed = 4.f;
    const u64 ch = sim::character_create(cd);
    check(ch != 0, "character created");

    sim::set_running(true);
    // 落地
    for (int i = 0; i < 60; ++i) sim::step(1.f / 60.f);
    sim::character_state st{};
    check(sim::character_get_state(ch, st), "state readable");
    check(st.grounded, "grounded after settling");

    // 行走 2 秒（+X 方向 4m/s → 位移 ≈ 8m ±10%）
    check(sim::character_move_intent(ch, 1, 0), "intent set");
    for (int i = 0; i < 120; ++i) sim::step(1.f / 60.f);
    sim::character_get_state(ch, st);
    check(st.position.x > 7.f && st.position.x < 9.f, "walk displacement ~8m in 2s");
    check(!st.stuck, "not stuck on open ground");
    check(st.horizontal_speed > 3.f, "horizontal speed ~4m/s");

    // 停止：零意图 → 速度衰减至 ~0
    sim::character_move_intent(ch, 0, 0);
    for (int i = 0; i < 120; ++i) sim::step(1.f / 60.f);
    sim::character_get_state(ch, st);
    check(st.horizontal_speed < 0.5f, "zero intent stops (friction)");

    sim::set_running(false);
    sim::shutdown();
}

void test_stuck_against_wall()
{
    sim::initialize(sim::settings{});
    sim::body_create(ground_desc());
    sim::body_create(wall_desc(4.f));   // 角色起点 3m 外的墙

    sim::character_desc cd{};
    cd.position = math::v3{ 0, 1.0f, 0 };
    cd.walk_speed = 4.f;
    const u64 ch = sim::character_create(cd);
    sim::set_running(true);
    for (int i = 0; i < 60; ++i) sim::step(1.f / 60.f);   // 落地
    sim::character_move_intent(ch, 1, 0);

    bool stuck_seen = false;
    for (int i = 0; i < 300; ++i)   // 5 秒：撞墙后应触发卡住诊断
    {
        sim::step(1.f / 60.f);
        sim::character_state st{};
        sim::character_get_state(ch, st);
        if (st.stuck) stuck_seen = true;
    }
    check(stuck_seen, "stuck diagnosed against wall (intent active, no displacement)");

    // remove 后查询 false
    check(sim::character_remove(ch), "character removed");
    sim::character_state st2{};
    check(!sim::character_get_state(ch, st2), "state query after remove = false");
    sim::set_running(false);
    sim::shutdown();
}

void test_airborne_and_invalid()
{
    sim::initialize(sim::settings{});
    // 无地面：角色下落 → grounded false（脚 ray 不命中）
    sim::character_desc cd{};
    cd.position = math::v3{ 0, 5, 0 };
    const u64 ch = sim::character_create(cd);
    sim::set_running(true);
    for (int i = 0; i < 30; ++i) sim::step(1.f / 60.f);
    sim::character_state st{};
    sim::character_get_state(ch, st);
    check(!st.grounded, "airborne: no ground below");
    sim::set_running(false);

    sim::character_desc bad{};
    bad.radius = -1.f;
    check(sim::character_create(bad) == 0, "invalid radius rejected");
    check(!sim::character_move_intent(999, 1, 0), "unknown id intent rejected");
    check(!sim::character_remove(999), "unknown id remove rejected");
    sim::shutdown();
}

} // anonymous namespace

int main()
{
    std::cout << "[TestPhysicsCharacter] === begin ===" << std::endl;
    test_walk_stop_grounded();
    test_stuck_against_wall();
    test_airborne_and_invalid();
    std::cout << "[TestPhysicsCharacter] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
