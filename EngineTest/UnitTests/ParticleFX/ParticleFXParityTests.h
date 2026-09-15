#pragma once

// ParticleFXParityTests.h — Niagara/POP 对齐批次 1 的门禁测试：
// PointForce（点引力/斥力 + 半径衰减）、VortexForce（切向涡旋 + 径向内聚）、
// BoundsWrap（越界回绕）。覆盖：JSON 编译、力方向、半径衰减截断、回绕。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/PfxSystem.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

inline void test_point_force_attracts() {
    std::cout << "Testing PointForce attraction..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // 粒子出生在原点（ShapeLocation point 默认）；引力中心 (5,0,0) ⇒ vx 增长
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"PointForce","center":[5,0,0],"strength":10}]}})json", 5);
    assert(id != invalid_emitter);

    emitter_get(id)->tick(1.f / 60.f);   // spawn + init + 首帧 update
    const f32 vx = emitter_get(id)->store().v4_channel(attr::velocity)[0].x;
    assert(vx > 0.f);                    // 朝中心 (+x) 加速
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: PointForce attraction (vx=" << vx << ")" << std::endl;
}

inline void test_point_force_radius_falloff_and_repel() {
    std::cout << "Testing PointForce repel + radius cutoff..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // 负 strength = 排斥；radius=2：粒子在 x=1（dist=1）→ 衰减 0.5，仍被推离
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"PointForce","center":[0,0,0],"strength":-10,"radius":2}]}})json", 6);
    assert(id != invalid_emitter);
    emitter_get(id)->tick(1.f / 60.f);

    // 手动把粒子放到 x=1 处，再 tick 一次验证斥力方向 + 衰减后的量级
    emitter_get(id)->store().v4_channel(attr::position)[0].x = 1.f;
    emitter_get(id)->tick(1.f / 60.f);
    const f32 vx = emitter_get(id)->store().v4_channel(attr::velocity)[0].x;
    assert(vx > 0.f);                                      // 推离中心 (+x)
    // 衰减 0.5、两帧累积 → |vx| < strength*dt*2 = 10*(1/60)*2 ≈ 0.34
    assert(vx < 0.34f);
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: PointForce repel + falloff (vx=" << vx << ")" << std::endl;
}

inline void test_vortex_force_swirl_direction() {
    std::cout << "Testing VortexForce swirl direction..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // 粒子在 (1,0,0)，轴 +Y，中心原点：切向 = axis×r = (0,0,-1) ⇒ vz 减小
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"VortexForce","center":[0,0,0],"axis":[0,1,0],"strength":30}]}})json", 7);
    assert(id != invalid_emitter);

    emitter_get(id)->tick(1.f / 60.f);   // 先生成粒子（row 0 存在）
    emitter_get(id)->store().v4_channel(attr::position)[0].x = 1.f;   // 放到 (1,0,0)
    emitter_get(id)->tick(1.f / 60.f);   // 涡旋：切向 −z
    const f32 vz = emitter_get(id)->store().v4_channel(attr::velocity)[0].z;
    const f32 vx = emitter_get(id)->store().v4_channel(attr::velocity)[0].x;
    assert(vz < 0.f);                      // 切向 −z
    assert(std::fabs(vx) < 1e-5f);         // 纯涡旋不改变径向速度
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: VortexForce swirl (vz=" << vz << ")" << std::endl;
}

inline void test_bounds_wrap() {
    std::cout << "Testing BoundsWrap..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"BoundsWrap","min":[-10,-10,-10],"max":[10,10,10]}]}})json", 8);
    assert(id != invalid_emitter);

    emitter_get(id)->tick(1.f / 60.f);
    // 手动越界：x=15 → 回绕到 −5（x = 15 − 20）
    emitter_get(id)->store().v4_channel(attr::position)[0].x = 15.f;
    emitter_get(id)->tick(1.f / 60.f);
    const f32 x = emitter_get(id)->store().v4_channel(attr::position)[0].x;
    assert(x <= 10.f && x >= -10.f);
    assert(std::fabs(x - (-5.f)) < 1e-4f);
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: BoundsWrap (x=" << x << ")" << std::endl;
}

inline void test_emitter_position_offset() {
    std::cout << "Testing emitter position offset (PfxSetEmitterPosition)..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 9);
    assert(id != invalid_emitter);

    // 实体同步回填：发射器世界位置 (10, 5, 0)
    emitter_set_position(id, math::v3{ 10.f, 5.f, 0.f });
    emitter_get(id)->tick(1.f / 60.f);

    const math::v4* p = emitter_get(id)->store().v4_channel(attr::position);
    const f32 px = p[0].x, py = p[0].y, pz = p[0].z;
    assert(std::fabs(px - 10.f) < 1e-4f);
    assert(std::fabs(py - 5.f) < 1e-4f);
    assert(std::fabs(pz) < 1e-4f);
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: emitter position offset (" << px << "," << py << "," << pz << ")" << std::endl;
}

inline void test_packed_frame_pool_blend_buckets() {
    std::cout << "Testing packed_frame_pool blend bucketing..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // additive（默认 blend 0）×1 + alpha（blend 1）×2 + premultiplied（blend 3）×1
    const emitter_id a = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 11);
    const emitter_id b = make_emitter_from_text(
        R"json({"blend":"alpha","capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":2,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 12);
    const emitter_id c = make_emitter_from_text(
        R"json({"blend":"premultiplied","capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 13);
    assert(a != invalid_emitter && b != invalid_emitter && c != invalid_emitter);
    emitter_get(a)->tick(1.f / 60.f);
    emitter_get(b)->tick(1.f / 60.f);
    emitter_get(c)->tick(1.f / 60.f);

    u32 total = 0;
    u32 counts[4] = {};
    const auto* data = packed_frame_pool(0, &total, counts);
    assert(data != nullptr);
    assert(total == 4);
    assert(counts[0] == 1 && counts[1] == 2 && counts[2] == 0 && counts[3] == 1);
    // 桶序 = blend 升序：前 1 个 additive，随后 2 个 alpha，最后 1 个 premultiplied
    emitter_destroy(a);
    emitter_destroy(b);
    emitter_destroy(c);
    shutdown();
    std::cout << "  PASS: packed_frame_pool buckets (" << counts[0] << "/" << counts[1]
              << "/" << counts[2] << "/" << counts[3] << ")" << std::endl;
}

inline void test_kill_speed() {
    std::cout << "Testing KillSpeed..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // 无初速度（speed=0 < 阈值 0.5）→ 首帧即回收
    const emitter_id dead = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"KillSpeed","threshold":0.5}]}})json", 14);
    assert(dead != invalid_emitter);
    emitter_get(dead)->tick(1.f / 60.f);
    assert(emitter_get(dead)->alive_count() == 0);
    emitter_destroy(dead);

    // 有初速度（speed=3 ≥ 0.5）→ 存活
    const emitter_id alive = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"AddVelocity","mode":"fixed","direction":[0,1,0],"speed_min":3,"speed_max":3},
                    {"module":"SetLifetime","min":50,"max":50}],
            "update":[{"module":"KillSpeed","threshold":0.5}]}})json", 15);
    assert(alive != invalid_emitter);
    emitter_get(alive)->tick(1.f / 60.f);
    assert(emitter_get(alive)->alive_count() == 1);
    emitter_destroy(alive);
    shutdown();
    std::cout << "  PASS: KillSpeed (below=dead, above=alive)" << std::endl;
}

inline void test_mesh_renderer_type_routing() {
    std::cout << "Testing mesh renderer type routing..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    // renderer:"mesh" ⇒ 不进 billboard 桥（ParticleFXMeshPass 负责），mesh 实例可打包
    const emitter_id mesh = make_emitter_from_text(
        R"json({"renderer":"mesh","capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 16);
    const emitter_id sprite = make_emitter_from_text(
        R"json({"capacity":8,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":50,"max":50}]}})json", 17);
    assert(mesh != invalid_emitter && sprite != invalid_emitter);
    emitter_get(mesh)->tick(1.f / 60.f);
    emitter_get(sprite)->tick(1.f / 60.f);

    u32 total = 0;
    u32 counts[4] = {};
    [[maybe_unused]] const auto* data = packed_frame_pool(0, &total, counts);
    assert(total == 1);   // 只有 sprite 进 billboard 桥

    using primal::pfx::render::mesh_instance_data;
    mesh_instance_data inst[8] = {};
    const u32 packed = primal::pfx::render::pack_mesh_instances(
        emitter_get(mesh)->store(), inst, 8);
    assert(packed == 1);
    emitter_destroy(mesh);
    emitter_destroy(sprite);
    shutdown();
    std::cout << "  PASS: mesh routing (billboard=1, mesh instances=1)" << std::endl;
}

inline void run_all_parity_tests() {
    test_point_force_attracts();
    test_point_force_radius_falloff_and_repel();
    test_vortex_force_swirl_direction();
    test_bounds_wrap();
    test_emitter_position_offset();
    test_packed_frame_pool_blend_buckets();
    test_kill_speed();
    test_mesh_renderer_type_routing();
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
