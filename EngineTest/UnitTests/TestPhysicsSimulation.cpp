// TestPhysicsSimulation.cpp — Engine/Simulation L0 单测（无 GPU 依赖）。
//
// 覆盖（Docs/2026-09-11-physics-simulation-design.md §8.0 L0 层）：
//   1. fixed_step_accumulator 纯逻辑（步数分派 / 钳制 / 丢帧语义）
//   2. layers::can_collide 碰撞过滤矩阵 + broadphase 归属
//   3. 确定性：同序构造 + 相同步数 → determinism_hash 逐位一致；位姿不同 → 哈希不同
//
// 注意：本测试是"可视化孪生"纪律下唯一允许无窗口的层——所有"物理行为"断言
// 由 TestPhysicsSmoke / TestPhysicsMultiBody 的渲染可视化层兜底。

#include "CommonHeaders.h"
#include "Simulation/SimulationAPI.h"

#include <cmath>
#include <iostream>

using namespace primal::simulation;
namespace math = primal::math;

namespace {

int failures = 0;

void check(bool ok, const char* name)
{
    if (!ok) {
        std::cerr << "[TestPhysicsSimulation] FAIL: " << name << std::endl;
        ++failures;
    } else {
        std::cout << "[TestPhysicsSimulation] PASS: " << name << std::endl;
    }
}

void test_accumulator()
{
    fixed_step_accumulator acc{};
    check(acc.fixed_dt > 0.f, "accumulator default fixed_dt positive");

    // 60 次 1/60 → 恰好 60 个子步，余量归零
    u32 total{ 0 };
    for (int i = 0; i < 60; ++i) total += acc.advance(1.f / 60.f);
    check(total == 60, "60x dt=1/60 -> 60 steps");

    // 一次 1/30 → 2 步
    acc.reset();
    check(acc.advance(1.f / 30.f) == 2, "dt=1/30 -> 2 steps");

    // 巨大 dt → 钳到 0.25s，命中 max_sub_steps 后丢弃余量
    acc.reset();
    check(acc.advance(10.f) == acc.max_sub_steps, "huge dt clamped to max_sub_steps");
    check(acc.accumulator == 0.f, "excess dropped after cap");

    // 零/负 dt → 0 步
    acc.reset();
    check(acc.advance(0.f) == 0, "dt=0 -> 0 steps");
    check(acc.advance(-1.f) == 0, "dt<0 -> 0 steps");

    // 小步累积：1/120 × 2 → 1 步
    acc.reset();
    const u32 s1 = acc.advance(1.f / 120.f);
    const u32 s2 = acc.advance(1.f / 120.f);
    check(s1 + s2 == 1, "2x dt=1/120 -> 1 step");
}

void test_layer_matrix()
{
    using namespace primal::simulation::layers;
    check(!can_collide(kStatic, kStatic), "static-static no collide");
    check(can_collide(kStatic, kDynamic), "static-dynamic collide");
    check(!can_collide(kStatic, kDebris), "static-debris no collide");
    check(can_collide(kStatic, kTrigger), "static-trigger collide");
    check(can_collide(kDynamic, kDynamic), "dynamic-dynamic collide");
    check(can_collide(kDynamic, kDebris), "dynamic-debris collide");
    check(can_collide(kDynamic, kTrigger), "dynamic-trigger collide");
    check(can_collide(kDebris, kDebris), "debris-debris collide");
    check(can_collide(kDebris, kTrigger), "debris-trigger collide");
    check(!can_collide(kTrigger, kTrigger), "trigger-trigger no collide");

    bool symmetric{ true };
    for (u32 a = 0; a < kCount; ++a)
        for (u32 b = 0; b < kCount; ++b)
            symmetric = symmetric && (can_collide(a, b) == can_collide(b, a));
    check(symmetric, "matrix symmetric");

    check(broad_phase_layer_for(kStatic) == kBroadPhaseStatic, "static -> bp static");
    check(broad_phase_layer_for(kTrigger) == kBroadPhaseStatic, "trigger -> bp static");
    check(broad_phase_layer_for(kDynamic) == kBroadPhaseMoving, "dynamic -> bp moving");
    check(broad_phase_layer_for(kDebris) == kBroadPhaseMoving, "debris -> bp moving");
}

void test_determinism()
{
    constexpr f32 dt = 1.f / 60.f;

    auto build_and_step = [&](u32 steps, f32 y_offset) -> u64 {
        settings s{};
        s.gravity = math::v3{ 0.f, -9.81f, 0.f };
        if (!initialize(s)) return 0;

        body_desc ground{};
        ground.m = body_desc::mode::static_body;
        ground.half_extents = math::v3{ 10.f, 0.5f, 10.f };
        ground.position = math::v3{ 0.f, -0.5f, 0.f };
        ground.restitution = 0.1f;
        const u64 ground_id = body_create(ground);

        body_desc box{};
        box.m = body_desc::mode::dynamic;
        box.half_extents = math::v3{ 0.5f, 0.5f, 0.5f };
        box.position = math::v3{ 0.f, 3.f + y_offset, 0.f };
        box.restitution = 0.1f;
        const u64 box_id = body_create(box);

        if (ground_id == 0 || box_id == 0) {
            shutdown();
            return 0;
        }
        set_running(true);
        for (u32 i = 0; i < steps; ++i) step(dt);
        const u64 h = determinism_hash();
        shutdown();
        return h;
    };

    const u64 h1 = build_and_step(30, 0.f);
    const u64 h2 = build_and_step(30, 0.f);
    check(h1 != 0, "determinism hash non-zero");
    check(h1 == h2, "same setup+steps -> identical hash");

    const u64 h3 = build_and_step(30, 0.01f);
    check(h3 != h1, "different initial pose -> different hash");
}

} // anonymous namespace

int main()
{
    std::cout << "[TestPhysicsSimulation] === begin ===" << std::endl;
    test_accumulator();
    test_layer_matrix();
    test_determinism();
    std::cout << "[TestPhysicsSimulation] === end, failures=" << failures << " ===" << std::endl;
    return failures ? 1 : 0;
}
