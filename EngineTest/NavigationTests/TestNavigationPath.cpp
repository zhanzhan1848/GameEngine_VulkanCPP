// TestNavigationPath.cpp — ai-navigation S1 任务生命周期单测（无 GPU 依赖）。
//
// 对应合同：到达 ≤0.1m 且事件恰好一次（A01）、取消后不再移动（A02）、
// 重复请求/重定目标、stale_nav 代际过期、类型化失败码、确定性。

#include "NavigationTestFixtures.h"
#include "Navigation/NavigationService.h"

#include <cmath>
#include <iostream>

using namespace primal::navigation;
namespace math = primal::math;
namespace ntest = primal::navigation::test;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestNavigationPath] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestNavigationPath] PASS: " << name << std::endl;
    }
}

inline f32 v3_dist(const math::v3& a, const math::v3& b)
{
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

navigation_asset make_flat_asset(f32 size = 12.f)
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, size, size);
    navigation_asset a;
    asset_bake(a, mb.input(), agent_profile{ 0.4f, 1.8f, 45.f, 0.35f });
    return a;
}

void test_arrival_single_event()
{
    const navigation_asset a = make_flat_asset();
    const math::v3 from{ 1.f, 0.f, 1.f };
    const math::v3 to{ 10.f, 0.f, 10.f };

    move_task t;
    check(task_create(a, from, to, 2.f, 1.f, t) == nav_error::ok, "arrival: create ok");
    check(t.state == move_state::moving, "arrival: starts moving");

    math::v3 pos = from;
    u32 arrived_count = 0;
    move_event ev = move_event::none;
    for (int i = 0; i < 900; ++i) {
        ev = task_update(a, t, pos, 1.f / 60.f, pos);
        if (ev == move_event::arrived) ++arrived_count;
    }
    check(t.state == move_state::completed, "arrival: completed");
    check(arrived_count == 1, "arrival: exactly one arrived event");
    check(v3_dist(pos, to) <= 0.1f + 1e-5f, "arrival: within 0.1m of target");

    // 完成后继续 update：不再产生事件，位置不动
    const math::v3 frozen = pos;
    for (int i = 0; i < 60; ++i) ev = task_update(a, t, pos, 1.f / 60.f, pos);
    check(ev == move_event::none && ntest::near3(pos, frozen, 1e-6f), "arrival: frozen after done");
}

void test_cancel_stops_movement()
{
    const navigation_asset a = make_flat_asset();
    const math::v3 from{ 1.f, 0.f, 6.f };
    const math::v3 to{ 11.f, 0.f, 6.f };

    move_task t;
    check(task_create(a, from, to, 2.f, 1.f, t) == nav_error::ok, "cancel: create ok");

    math::v3 pos = from;
    for (int i = 0; i < 60; ++i) task_update(a, t, pos, 1.f / 60.f, pos);
    const f32 moved_before = v3_dist(from, pos);
    check(moved_before > 1.5f, "cancel: moved before cancel");

    task_cancel(t);
    check(t.state == move_state::cancelled, "cancel: state cancelled");
    const math::v3 frozen = pos;
    move_event ev = move_event::none;
    for (int i = 0; i < 120; ++i) ev = task_update(a, t, pos, 1.f / 60.f, pos);
    check(ev == move_event::none && ntest::near3(pos, frozen, 1e-6f), "cancel: no further movement");

    task_cancel(t);   // 幂等
    check(t.state == move_state::cancelled, "cancel: idempotent");
}

void test_retarget()
{
    const navigation_asset a = make_flat_asset();
    const math::v3 from{ 1.f, 0.f, 1.f };

    move_task first;
    check(task_create(a, from, math::v3{ 10.f, 0.f, 1.f }, 2.f, 1.f, first) == nav_error::ok,
          "retarget: first create ok");
    math::v3 pos = from;
    for (int i = 0; i < 30; ++i) task_update(a, first, pos, 1.f / 60.f, pos);

    move_task second;
    check(task_create(a, pos, math::v3{ 5.f, 0.f, 10.f }, 3.f, 1.f, second) == nav_error::ok,
          "retarget: second create ok");
    check(second.id != first.id, "retarget: distinct ids");
    check(second.state == move_state::moving, "retarget: second moving");

    task_cancel(first);
    u32 arrived = 0;
    for (int i = 0; i < 900; ++i)
        if (task_update(a, second, pos, 1.f / 60.f, pos) == move_event::arrived) ++arrived;
    check(second.state == move_state::completed && arrived == 1, "retarget: second completes");
    check(v3_dist(pos, math::v3{ 5.f, 0.f, 10.f }) <= 0.1f + 1e-5f, "retarget: ends at new target");
}

void test_stale_generation()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 12.f, 12.f);
    navigation_asset a;
    check(asset_bake(a, mb.input(), agent_profile{}) == nav_error::ok, "stale: bake 1 ok");

    move_task t;
    check(task_create(a, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 10.f, 0.f, 10.f }, 2.f, 1.f, t)
              == nav_error::ok, "stale: create ok");

    // 重烘焙（几何略变）→ 代际递增 → 旧任务过期
    ntest::mesh_builder mb2;
    mb2.quad_y(0.f, 0.f, 0.f, 12.5f, 12.f);
    check(asset_bake(a, mb2.input(), agent_profile{}) == nav_error::ok, "stale: bake 2 ok");

    math::v3 pos{ 1.f, 0.f, 1.f };
    const math::v3 before = pos;
    const move_event ev = task_update(a, t, pos, 1.f / 60.f, pos);
    check(ev == move_event::failed_stale, "stale: event failed_stale");
    check(t.state == move_state::failed && t.failure == nav_error::stale_nav, "stale: typed failure");
    check(ntest::near3(pos, before, 1e-6f), "stale: no movement");
}

void test_typed_failures()
{
    // 空资产 → no_nav_data
    {
        const navigation_asset empty{};
        move_task t;
        const nav_error e = task_create(empty, math::v3{ 0, 0, 0 }, math::v3{ 1, 0, 1 }, 2.f, 1.f, t);
        check(e == nav_error::no_nav_data && t.state == move_state::failed, "typed: no_nav_data");
    }
    // 断开岛 → unreachable
    {
        ntest::mesh_builder mb;
        mb.quad_y(0.f, 0.f, 0.f, 2.f, 2.f);
        mb.quad_y(0.f, 5.f, 0.f, 7.f, 2.f);
        navigation_asset a;
        asset_bake(a, mb.input(), agent_profile{});
        move_task t;
        const nav_error e = task_create(a, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 6.f, 0.f, 1.f }, 2.f, 1.f, t);
        check(e == nav_error::unreachable && t.state == move_state::failed, "typed: unreachable");
    }
    // 目标远离任何可走面 → invalid_target
    {
        const navigation_asset a = make_flat_asset();
        move_task t;
        const nav_error e = task_create(a, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 40.f, 0.f, 40.f }, 2.f, 1.f, t);
        check(e == nav_error::invalid_target && t.state == move_state::failed, "typed: invalid_target");
    }
}

void test_determinism()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 10.f, 10.f);
    mb.box(4.f, 0.f, 4.f, 6.f, 2.f, 6.f);   // 中央障碍，逼出绕行

    nav_grid g1, g2;
    bake(mb.input(), agent_profile{}, g1);
    bake(mb.input(), agent_profile{}, g2);

    std::string s1, s2;
    serialize(g1, s1);
    serialize(g2, s2);
    check(s1 == s2, "determinism: identical serialization");

    const path_result r1 = find_path(g1, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 9.f, 0.f, 9.f }, 1.f);
    const path_result r2 = find_path(g2, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 9.f, 0.f, 9.f }, 1.f);
    check(r1.error == nav_error::ok && r2.error == nav_error::ok, "determinism: paths ok");
    bool same = r1.waypoints.size() == r2.waypoints.size();
    if (same) {
        for (size_t i = 0; i < r1.waypoints.size(); ++i) {
            if (!ntest::near3(r1.waypoints[i], r2.waypoints[i], 1e-6f)) {
                same = false;
                break;
            }
        }
    }
    check(same, "determinism: identical waypoints");
}

} // anonymous namespace

int main()
{
    std::cout << "[TestNavigationPath] === begin ===" << std::endl;
    test_arrival_single_event();
    test_cancel_stops_movement();
    test_retarget();
    test_stale_generation();
    test_typed_failures();
    test_determinism();
    std::cout << "[TestNavigationPath] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
