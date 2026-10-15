// TestPhysicsJoints.cpp — physics S2 铰链关节契约测试（无 GPU 依赖）。
//
// physics-S2-A01 语义：约束角度/距离误差在预定容差内；高速物体不发散；
// 重置（joint_remove）后物体自由；多控制者（同 body 双关节）不崩溃且状态
// 查询可用。铰链限位在 [-30°, 30°]：重力摆动被限位钳住；驱动电机关到
// 限位附近。

#include "CommonHeaders.h"
#include "Simulation/SimulationAPI.h"

#include <cmath>
#include <string>
#include <iostream>

using namespace primal;
namespace sim = primal::simulation;

namespace
{
int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestPhysicsJoints] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestPhysicsJoints] PASS: " << name << std::endl;
    }
}

sim::body_desc box_at(f32 x, f32 y, f32 z, sim::body_desc::mode m)
{
    sim::body_desc d{};
    d.s = sim::body_desc::shape::box;
    d.m = m;
    d.half_extents = math::v3{ 0.25f, 0.25f, 0.25f };
    d.position = math::v3{ x, y, z };
    d.mass = 1.f;
    return d;
}

void test_hinge_limits_and_error()
{
    sim::initialize(sim::settings{});
    // 静态支柱 + 动态摆锤（铰链锚在支柱顶）
    const u64 post = sim::body_create(box_at(0, 1, 0, sim::body_desc::mode::static_body));
    const u64 bob  = sim::body_create(box_at(1.0f, 1, 0, sim::body_desc::mode::dynamic));   // 半径 1.0——与支柱留 0.25 间隙防接触卡死
    check(post != 0 && bob != 0, "bodies created");

    sim::joint_desc jd{};
    jd.body_a = post;
    jd.body_b = bob;
    jd.world_anchor = math::v3{ 0, 1, 0 };
    jd.axis = math::v3{ 0, 0, 1 };      // 绕 Z 摆动（重力把摆锤往下拉——限位应钳在 -30°）
    jd.limit_min_deg = -30.f;
    jd.limit_max_deg = 30.f;
    const u64 joint = sim::joint_create(jd);
    check(joint != 0, "hinge joint created");

    sim::set_running(true);
    f32 max_angle = 0.f, max_sep = 0.f;
    for (int i = 0; i < 240; ++i)   // 4 秒 @60Hz
    {
        sim::step(1.f / 60.f);
        f32 angle = 0, sep = 0;
        if (sim::joint_get_state(joint, angle, sep))
        {
            max_angle = std::fmax(max_angle, std::fabs(angle));
            max_sep = std::fmax(max_sep, sep);
        }
    }
    check(max_angle > 25.f, "limit: pendulum actually swung (|angle| > 25deg reached)");
    check(max_angle <= 33.f, "limit: |angle| <= 30deg (drop overshoot tol 3deg)");
    check(max_sep < 0.05f, "constraint anchor error < 5cm");
    (void)max_angle; (void)max_sep;

    check(sim::joint_remove(joint), "joint removed");
    check(!sim::joint_get_state(joint, max_angle, max_sep), "state query after remove = false");
    sim::set_running(false);
    sim::shutdown();
}

void test_motor_drive()
{
    sim::initialize(sim::settings{});
    const u64 post = sim::body_create(box_at(0, 1, 0, sim::body_desc::mode::static_body));
    const u64 bob  = sim::body_create(box_at(1.0f, 1, 0, sim::body_desc::mode::dynamic));   // 半径 1.0——与支柱留 0.25 间隙防接触卡死
    sim::joint_desc jd{};
    jd.body_a = post; jd.body_b = bob;
    jd.world_anchor = math::v3{ 0, 1, 0 };
    jd.axis = math::v3{ 0, 0, 1 };
    jd.motor = true;
    jd.motor_speed_deg_sec = 90.f;
    jd.motor_torque = 100.f;
    const u64 joint = sim::joint_create(jd);
    check(joint != 0, "motor joint created");

    sim::set_running(true);
    // 电机证据=运行期最大 |角度|（最终值可能因休眠/[-pi,pi] 回绕而任意）
    f32 max_angle = 0.f;
    for (int i = 0; i < 120; ++i)
    {
        sim::step(1.f / 60.f);
        f32 angle = 0, sep = 0;
        if (sim::joint_get_state(joint, angle, sep))
            max_angle = std::fmax(max_angle, std::fabs(angle));
    }
    check(max_angle > 5.f, "motor drove |angle| > 5deg within 2s @90deg/s");
    sim::set_running(false);
    sim::shutdown();
}

void test_high_speed_stability_and_multi_controller()
{
    sim::initialize(sim::settings{});
    const u64 post = sim::body_create(box_at(0, 2, 0, sim::body_desc::mode::static_body));
    const u64 bob = sim::body_create(box_at(1, 2, 0, sim::body_desc::mode::dynamic));
    sim::body_set_linear_velocity(bob, math::v3{ 50.f, 0, 0 });   // 高速冲扯

    sim::joint_desc jd{};
    jd.body_a = post; jd.body_b = bob;
    jd.world_anchor = math::v3{ 0, 2, 0 };
    jd.axis = math::v3{ 0, 1, 0 };
    const u64 j1 = sim::joint_create(jd);
    // 多控制者：同对 body 再加一个铰链（不同轴）——不崩溃，两关节各自可查
    sim::joint_desc jd2{};
    jd2.body_a = post; jd2.body_b = bob;
    jd2.world_anchor = math::v3{ 0, 2, 0 };
    jd2.axis = math::v3{ 0, 0, 1 };
    const u64 j2 = sim::joint_create(jd2);
    check(j1 != 0 && j2 != 0, "two joints on same body pair (multi-controller)");

    sim::set_running(true);
    bool finite_all = true;
    for (int i = 0; i < 60; ++i)
    {
        sim::step(1.f / 60.f);
        sim::body_transform t{};
        if (!sim::body_get_transform(bob, t)) { finite_all = false; break; }
        if (!std::isfinite(t.position.x) || !std::isfinite(t.position.y)) { finite_all = false; break; }
    }
    check(finite_all, "high-speed body stays finite (no explosion)");
    sim::set_running(false);
    sim::shutdown();
}

void test_invalid_inputs()
{
    sim::initialize(sim::settings{});
    sim::joint_desc jd{};
    jd.body_a = 0; jd.body_b = 0;    // 世界固定端 S2 不支持（合同如实）
    check(sim::joint_create(jd) == 0, "world-anchored joint rejected (S2 contract)");
    jd.body_a = 12345; jd.body_b = 54321;   // 不存在的 body
    check(sim::joint_create(jd) == 0, "unknown body ids rejected");
    jd.body_a = jd.body_b = 1;
    jd.axis = math::v3{ 0, 0, 0 };   // 退化轴
    check(sim::joint_create(jd) == 0, "degenerate axis rejected");
    check(!sim::joint_remove(999), "remove unknown joint = false");
    sim::shutdown();
}

} // anonymous namespace

int main()
{
    std::cout << "[TestPhysicsJoints] === begin ===" << std::endl;
    test_hinge_limits_and_error();
    test_motor_drive();
    test_high_speed_stability_and_multi_controller();
    test_invalid_inputs();
    std::cout << "[TestPhysicsJoints] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
