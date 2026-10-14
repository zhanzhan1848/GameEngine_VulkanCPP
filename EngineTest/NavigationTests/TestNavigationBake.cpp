// TestNavigationBake.cpp — ai-navigation S1 烘焙单测（无 GPU 依赖）。
//
// 对应阶段合同用例族 ai-navigation-S1-A01/A02 的数据侧：
//   可达/断开图、坡度边界、台阶边界、角色尺寸（腐蚀）、净空、输入校验。

#include "NavigationTestFixtures.h"
#include "Navigation/NavGrid.h"

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
        std::cerr << "[TestNavigationBake] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestNavigationBake] PASS: " << name << std::endl;
    }
}

agent_profile default_profile()
{
    return agent_profile{ 0.4f, 1.8f, 45.f, 0.35f };
}

void test_flat_plane()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 10.f, 10.f);
    nav_grid g;
    check(bake(mb.input(), default_profile(), g) == nav_error::ok, "flat: bake ok");
    check(!g.empty() && g.walkable_span_count > 0, "flat: has walkable spans");
    check(g.region_count == 1, "flat: single region");

    math::v3 proj{};
    check(nearest_walkable(g, math::v3{ 0.3f, 0.f, 0.3f }, 1.f, proj) == nav_error::ok,
          "flat: project near origin");
    check(fabsf(proj.y) < 1e-4f, "flat: projected y ~ 0");

    const path_result r = find_path(g, math::v3{ 0.3f, 0.f, 0.3f }, math::v3{ 9.7f, 0.f, 9.7f }, 1.f);
    check(r.error == nav_error::ok, "flat: path ok");
    check(r.waypoints.size() >= 2, "flat: path has endpoints");
    if (!r.waypoints.empty()) {
        const f32 d = sqrtf(powf(r.waypoints.back().x - 9.7f, 2.f)
                            + powf(r.waypoints.back().z - 9.7f, 2.f));
        check(d <= 0.5f, "flat: last waypoint near target");
    }
}

void test_islands_unreachable()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 2.f, 2.f);    // 岛 A
    mb.quad_y(0.f, 5.f, 0.f, 7.f, 2.f);    // 岛 B（断开）
    nav_grid g;
    check(bake(mb.input(), default_profile(), g) == nav_error::ok, "islands: bake ok");
    check(g.region_count == 2, "islands: two regions");

    const path_result cross = find_path(g, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 6.f, 0.f, 1.f }, 1.f);
    check(cross.error == nav_error::unreachable, "islands: A->B unreachable");
    check(cross.region_from != cross.region_to, "islands: regions differ");

    const path_result inner = find_path(g, math::v3{ 0.5f, 0.f, 0.5f }, math::v3{ 1.5f, 0.f, 1.5f }, 1.f);
    check(inner.error == nav_error::ok, "islands: same island reachable");
}

void test_slope_boundary()
{
    // 缓坡：floor -> 3m 长 2m 高（~33.7°<45°）-> 平台 → 可达
    {
        ntest::mesh_builder mb;
        mb.quad_y(0.f, 0.f, 0.f, 3.f, 3.f);
        mb.quad_slope(3.f, 0.f, 6.f, 3.f, 0.f, 2.f);
        mb.quad_y(2.f, 6.f, 0.f, 9.f, 3.f);
        nav_grid g;
        check(bake(mb.input(), default_profile(), g) == nav_error::ok, "slope: gentle bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 1.5f }, math::v3{ 8.f, 2.f, 1.5f }, 1.f);
        check(r.error == nav_error::ok, "slope: gentle ramp reachable");
        if (r.error == nav_error::ok)
            check(fabsf(r.waypoints.back().y - 2.f) < 0.5f, "slope: top y ~ 2");
    }
    // 陡坡：1m 长 2m 高（~63°>45°）→ 不可达
    {
        ntest::mesh_builder mb;
        mb.quad_y(0.f, 0.f, 0.f, 3.f, 3.f);
        mb.quad_slope(3.f, 0.f, 4.f, 3.f, 0.f, 2.f);
        mb.quad_y(2.f, 4.f, 0.f, 7.f, 3.f);
        nav_grid g;
        check(bake(mb.input(), default_profile(), g) == nav_error::ok, "slope: steep bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 1.5f }, math::v3{ 6.f, 2.f, 1.5f }, 1.f);
        check(r.error == nav_error::unreachable, "slope: steep ramp unreachable");
    }
}

void test_gap_vs_agent_radius()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 10.f, 6.f);                  // 地板
    mb.box(4.f, 0.f, -0.5f, 6.f, 2.f, 2.75f);             // 整墙（上段），留 0.5m 门洞
    mb.box(4.f, 0.f, 3.25f, 6.f, 2.f, 6.5f);              // 整墙（下段）
    const bake_input in = mb.input();

    {
        nav_grid g;
        agent_profile small{ 0.1f, 1.8f, 45.f, 0.35f };   // required = ceil(0.1/0.25) = 1
        check(bake(in, small, g) == nav_error::ok, "gap: small agent bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 3.f }, math::v3{ 9.f, 0.f, 3.f }, 1.f);
        check(r.error == nav_error::ok, "gap: small agent passes");
        bool through_gap = false;
        for (const auto& w : r.waypoints)
            if (w.x > 3.9f && fabsf(w.z - 3.f) < 0.4f) through_gap = true;
        check(through_gap, "gap: small agent path uses gap");
    }
    {
        nav_grid g;
        agent_profile big{ 0.5f, 1.8f, 45.f, 0.35f };    // required = ceil(0.5/0.25) = 2
        check(bake(in, big, g) == nav_error::ok, "gap: big agent bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 3.f }, math::v3{ 9.f, 0.f, 3.f }, 1.f);
        check(r.error == nav_error::unreachable, "gap: big agent blocked");
    }
}

void test_step_boundary()
{
    {
        ntest::mesh_builder mb;
        mb.quad_y(0.f, 0.f, 0.f, 5.f, 5.f);
        mb.quad_y(0.3f, 5.f, 0.f, 10.f, 5.f);   // 0.3m 台阶 ≤ 0.35 → 可达
        nav_grid g;
        check(bake(mb.input(), default_profile(), g) == nav_error::ok, "step: low bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 2.5f }, math::v3{ 9.f, 0.3f, 2.5f }, 1.f);
        check(r.error == nav_error::ok, "step: 0.3m reachable");
    }
    {
        ntest::mesh_builder mb;
        mb.quad_y(0.f, 0.f, 0.f, 5.f, 5.f);
        mb.quad_y(0.5f, 5.f, 0.f, 10.f, 5.f);   // 0.5m 台阶 > 0.35 → 不可达
        nav_grid g;
        check(bake(mb.input(), default_profile(), g) == nav_error::ok, "step: high bake ok");
        const path_result r = find_path(g, math::v3{ 1.f, 0.f, 2.5f }, math::v3{ 9.f, 0.5f, 2.5f }, 1.f);
        check(r.error == nav_error::unreachable, "step: 0.5m unreachable");
    }
}

void test_clearance_low_ceiling()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 10.f, 10.f);
    mb.box(2.f, 1.2f, 2.f, 8.f, 2.5f, 8.f);   // 悬浮盒：底面 1.2 处净空 < 1.8 → 下方不可走
    nav_grid g;
    check(bake(mb.input(), default_profile(), g) == nav_error::ok, "ceiling: bake ok");

    math::v3 proj{};
    check(nearest_walkable(g, math::v3{ 5.f, 0.f, 5.f }, 0.4f, proj) == nav_error::invalid_target,
          "ceiling: under-box target invalid");

    const path_result r = find_path(g, math::v3{ 1.f, 0.f, 1.f }, math::v3{ 9.f, 0.f, 9.f }, 1.f);
    check(r.error == nav_error::ok, "ceiling: corner-to-corner ok");
    bool crosses = false;
    for (const auto& w : r.waypoints)
        if (w.x > 2.2f && w.x < 7.8f && w.z > 2.2f && w.z < 7.8f && w.y < 1.f) crosses = true;
    check(!crosses, "ceiling: path avoids blocked area");
}

void test_all_unwalkable()
{
    ntest::mesh_builder mb;
    mb.quad_slope(0.f, 0.f, 1.f, 3.f, 0.f, 2.f);   // 只有陡坡（63°>45°）
    nav_grid g;
    check(bake(mb.input(), default_profile(), g) == nav_error::ok, "all-unwalkable: bake ok");
    check(g.walkable_span_count == 0, "all-unwalkable: zero walkable");
    const path_result r = find_path(g, math::v3{ 0.2f, 0.f, 1.5f }, math::v3{ 0.8f, 2.f, 1.5f }, 1.f);
    check(r.error == nav_error::agent_too_large, "all-unwalkable: query typed error");
}

void test_input_validation()
{
    ntest::mesh_builder mb;
    mb.quad_y(0.f, 0.f, 0.f, 2.f, 2.f);
    nav_grid g;

    bake_input bad = mb.input();
    bad.indices = nullptr;
    check(bake(bad, default_profile(), g) == nav_error::null_input, "validate: null indices");

    bad = mb.input();
    bad.index_count = 4;   // 非 3 倍数
    check(bake(bad, default_profile(), g) == nav_error::null_input, "validate: index count %3");

    bad = mb.input();
    std::vector<f32> poisoned(bad.positions, bad.positions + bad.position_count * 3);
    poisoned[1] = std::nanf("");
    bad.positions = poisoned.data();
    check(bake(bad, default_profile(), g) == nav_error::null_input, "validate: NaN position");

    bad = mb.input();
    bad.cell_size = 0.f;
    check(bake(bad, default_profile(), g) == nav_error::null_input, "validate: zero cell size");

    agent_profile badp = default_profile();
    badp.radius = -1.f;
    check(bake(mb.input(), badp, g) == nav_error::null_input, "validate: bad profile");

    std::vector<u32> oob = { 0, 1, 999 };
    bake_input oob_in = mb.input();
    oob_in.indices = oob.data();
    oob_in.index_count = 3;
    check(bake(oob_in, default_profile(), g) == nav_error::null_input, "validate: index out of range");

    check(g.empty(), "validate: failures leave grid untouched");
}

} // anonymous namespace

int main()
{
    std::cout << "[TestNavigationBake] === begin ===" << std::endl;
    test_flat_plane();
    test_islands_unreachable();
    test_slope_boundary();
    test_gap_vs_agent_radius();
    test_step_boundary();
    test_clearance_low_ceiling();
    test_all_unwalkable();
    test_input_validation();
    std::cout << "[TestNavigationBake] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
