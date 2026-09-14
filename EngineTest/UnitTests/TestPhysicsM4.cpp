// TestPhysicsM4.cpp — M4 组件/事件/扩展碰撞体单测（无 GPU）。
//
// 覆盖（设计文档 §9 M4-17/19）：
//   1. 接触事件：球落到静态盒 → drain_contacts 捕获接触（body id 与 added 标志正确）
//   2. 凸包碰撞体：8 角点凸包地台 → 盒在其上着地
//   3. 三角网碰撞体：两三角四边形地台 → 盒在其上着地（MeshShape 仅静态）
// 可视化孪生：同源行为的渲染验证由 T0/T1 兜底（§8.0 纪律）。

#include "CommonHeaders.h"
#include "Simulation/SimulationAPI.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace primal::simulation;
namespace math = primal::math;
namespace simulation = primal::simulation;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestPhysicsM4] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestPhysicsM4] PASS: " << name << std::endl;
    }
}

// 公共：静态地台 + 动态盒从 y=3 落下 N 帧，返回盒终态 y
struct land_result
{
    f32 box_y;
    u64 box_body;
    u64 ground_body;
};

land_result drop_on_ground(u32 frames, bool use_convex, bool use_mesh)
{
    land_result r{ -1000.f, 0, 0 };

    simulation::settings s{};
    s.gravity = math::v3{ 0.f, -9.81f, 0.f };
    if (!simulation::initialize(s)) return r;
    simulation::set_running(true);

    // 地台：普通盒 / 凸包（8 角点）/ 三角网（两三角）
    simulation::body_desc gd{};
    gd.m = simulation::body_desc::mode::static_body;
    gd.position = math::v3{ 0.f, -0.5f, 0.f };
    gd.restitution = 0.05f;

    if (use_convex) {
        math::v3 pts[8] = {
            { -10.f, -0.5f, -10.f }, { 10.f, -0.5f, -10.f },
            { 10.f, -0.5f, 10.f },  { -10.f, -0.5f, 10.f },
            { -10.f, 0.5f, -10.f }, { 10.f, 0.5f, -10.f },
            { 10.f, 0.5f, 10.f },  { -10.f, 0.5f, 10.f },
        };
        r.ground_body = simulation::body_create_convex(gd, pts, 8);
    } else if (use_mesh) {
        // 顶点为 body 局部坐标（body position 再给世界偏移 -0.5）；环绕正面朝 +Y
        const math::v3 verts[4] = {
            { -10.f, 0.f, -10.f }, { 10.f, 0.f, -10.f },
            { 10.f, 0.f, 10.f },  { -10.f, 0.f, 10.f },
        };
        const u32 idx[6] = { 0, 2, 1, 0, 3, 2 };
        r.ground_body = simulation::body_create_mesh(gd, verts, 4, idx, 6);
    } else {
        gd.half_extents = math::v3{ 10.f, 0.5f, 10.f };
        r.ground_body = simulation::body_create(gd);
    }

    // 动态盒
    simulation::body_desc bd{};
    bd.m = simulation::body_desc::mode::dynamic;
    bd.half_extents = math::v3{ 0.5f, 0.5f, 0.5f };
    bd.position = math::v3{ 0.f, 3.f, 0.f };
    bd.restitution = 0.1f;
    r.box_body = simulation::body_create(bd);

    if (r.ground_body == 0 || r.box_body == 0) { simulation::shutdown(); return r; }

    constexpr f32 dt = 1.f / 60.f;
    for (u32 i = 0; i < frames; ++i) simulation::step(dt);

    simulation::body_transform t{};
    if (simulation::body_get_transform(r.box_body, t)) r.box_y = t.position.y;
    simulation::shutdown();
    return r;
}

void test_contacts()
{
    simulation::settings s{};
    s.gravity = math::v3{ 0.f, -9.81f, 0.f };
    check(simulation::initialize(s), "contacts: physics initialize");

    simulation::body_desc gd{};
    gd.m = simulation::body_desc::mode::static_body;
    gd.half_extents = math::v3{ 10.f, 0.5f, 10.f };
    gd.position = math::v3{ 0.f, -0.5f, 0.f };
    const u64 ground = simulation::body_create(gd);

    simulation::body_desc bd{};
    bd.half_extents = math::v3{ 0.5f, 0.5f, 0.5f };
    bd.position = math::v3{ 0.f, 3.f, 0.f };
    const u64 box = simulation::body_create(bd);

    check(ground != 0 && box != 0, "contacts: bodies created");
    simulation::set_running(true);
    for (int i = 0; i < 90; ++i) simulation::step(1.f / 60.f);   // 1.5s：下落+首次接触

    simulation::contact_event evs[64] = {};
    const u32 n = simulation::drain_contacts(evs, 64);
    check(n > 0, "contact events captured");
    if (n > 0) {
        bool ids_ok = true, added_ok = false;
        for (u32 i = 0; i < n; ++i) {
            if ((evs[i].body_a != box && evs[i].body_a != ground) ||
                (evs[i].body_b != box && evs[i].body_b != ground)) ids_ok = false;
            if (evs[i].added == 1) added_ok = true;
        }
        check(ids_ok, "contact events reference dropped pair");
        check(added_ok, "at least one contact-added event");
    }

    // drain 取走后清空
    check(simulation::drain_contacts(evs, 64) == 0, "drain empties queue");

    // 清理（drain 剩余防扰下个用例）
    simulation::shutdown();
}

void test_convex_ground()
{
    const land_result r = drop_on_ground(120, true, false);
    check(r.ground_body != 0, "convex ground created");
    check(r.box_y > 0.35f && r.box_y < 0.65f,
          "box lands on convex hull ground (y ~ 0.5)");
    std::cout << "[TestPhysicsM4] convex ground box_y=" << r.box_y << std::endl;
}

void test_mesh_ground()
{
    const land_result r = drop_on_ground(120, false, true);
    check(r.ground_body != 0, "mesh ground created");
    check(r.box_y > -0.15f && r.box_y < 0.15f,
          "box lands on triangle mesh ground (y ~ 0.0，网格平面在 -0.5)");
    std::cout << "[TestPhysicsM4] mesh ground box_y=" << r.box_y << std::endl;
}

} // anonymous namespace

int main()
{
    std::cout << "[TestPhysicsM4] === begin ===" << std::endl;
    test_contacts();
    test_convex_ground();
    test_mesh_ground();
    std::cout << "[TestPhysicsM4] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
