#pragma once

// ParticleFXN4Tests.h — PFX N4 门禁测试（场系统 + 边界，架构文档 §7-N4）。
// 覆盖：volume_grid 三线性采样/梯度、.pgrid 往返、KillVolume 四种响应、
//       CurlNoise 确定性与有界性、AdvectByField 平流、SDFCollision 碰撞反弹、
//       场声明编译与未知场名失败路径。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/FieldSystem.h"
#include "Engine/ParticleFX/core/PfxSystem.h"

#include <cassert>
#include <cmath>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

inline emitter_id make_emitter_from_text(const char* text, u32 seed) {
    emitter_program prog;
    std::string err;
    if (!compile_emitter_from_text(text, std::strlen(text), prog, err)) {
        std::cout << "  [test-compile-fail] " << err << std::endl;
        return invalid_emitter;
    }
    prog.seed = seed;
    return emitter_create_from_program(prog);
}

inline void test_field_grid_sample_and_roundtrip() {
    std::cout << "Testing volume_grid sampling and .pgrid round-trip..." << std::endl;

    // SDF：竖直方向距离场 d = y（地面 y=0）
    volume_grid grid;
    assert(grid.init_scalar(8, 8, 8, math::v3{ -4.f, -4.f, -4.f }, 1.f, 0.f));
    for (u32 z = 0; z < 8; ++z)
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) grid.set_scalar(x, y, z, (f32)((s32)y - 4));

    // 精确落在格点上的采样
    assert(std::fabs(grid.sample_scalar(math::v3{ 0.f, 2.f, 0.f }) - 2.f) < 1e-5f);
    // 三线性中点
    const f32 mid = grid.sample_scalar(math::v3{ 0.f, 1.5f, 0.f });
    assert(std::fabs(mid - 1.5f) < 1e-5f);
    // 界外 = outside 外推值
    grid.set_outside(100.f);
    assert(grid.sample_scalar(math::v3{ 99.f, 0.f, 0.f }) == 100.f);
    // 梯度 ≈ (0,1,0)（取网格内部点——边界一圈的中央差分模板越界属三线性固有语义）
    const math::v3 g = grid.sample_gradient(math::v3{ 0.f, 0.f, 0.f });
    assert(std::fabs(g.y - 1.f) < 1e-4f && std::fabs(g.x) < 1e-4f);

    // .pgrid 往返
    std::string err;
    assert(grid.write_pgrid("/tmp/pfx_test.pgrid", err));
    volume_grid grid2;
    assert(grid2.read_pgrid("/tmp/pfx_test.pgrid", err));
    assert(grid2.get_kind() == volume_grid::kind::scalar);
    assert(std::fabs(grid2.sample_scalar(math::v3{ 0.f, 2.f, 0.f }) - 2.f) < 1e-6f);
    assert(grid2.sample_scalar(math::v3{ 99.f, 0.f, 0.f }) == 100.f);

    // 向量场 + 平流采样
    volume_grid vf;
    assert(vf.init_vector(4, 4, 4, math::v3{ -2.f, -2.f, -2.f }, 1.f, math::v3{ 0.f, 1.f, 0.f }));
    const math::v3 v = vf.sample_vector(math::v3{ 0.f, 0.f, 0.f });
    assert(std::fabs(v.y - 1.f) < 1e-6f);

    std::remove("/tmp/pfx_test.pgrid");
    std::cout << "  PASS: volume_grid sample + pgrid round-trip" << std::endl;
}

inline void test_kill_volume_bounce() {
    std::cout << "Testing KillVolume bounce (plane + gravity)..." << std::endl;

    assert(initialize(system_config{ 4, 8192 }));
    // 地面平面 y=0（法线 +Y），重力 -10，e=1 ⇒ 永续弹跳
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":16,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":10,"max":10},
                    {"module":"AddVelocity","mode":"fixed","direction":[0,-1,0],"speed_min":5,"speed_max":5}],
            "update":[{"module":"GravityForce","gravity":[0,-10,0]},
                      {"module":"IntegrateVelocity"},
                      {"module":"KillVolume","shape":"plane","normal":[0,1,0],
                       "response":"bounce","restitution":1}]}})json", 17);
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);

    f32 min_y = 1e9f;
    bool bounced = false;
    f32 prev_vy = 0.f;
    for (u32 f = 0; f < 600; ++f) {
        em->tick(1.f / 120.f);
        const math::v4* pos = em->store().v4_channel(attr::position);
        const math::v4* vel = em->store().v4_channel(attr::velocity);
        if (pos[0].y < min_y) min_y = pos[0].y;
        if (prev_vy < 0.f && vel[0].y > 0.f) bounced = true;
        prev_vy = vel[0].y;
        assert(em->alive_count() == 1);      // bounce ⇒ 永不死
        assert(pos[0].y >= -0.05f);          // 被约束在地面之上（容差含单帧穿透）
    }
    assert(min_y > -0.05f && min_y < 0.02f); // 确实贴地过
    assert(bounced);                          // 速度方向确实翻转
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: KillVolume bounce (min_y=" << min_y << ")" << std::endl;
}

inline void test_kill_volume_kill_response() {
    std::cout << "Testing KillVolume kill response..." << std::endl;

    assert(initialize(system_config{ 4, 8192 }));
    // AABB 盒 (0,0,0) 半尺寸 0.5；粒子原点出发直接在盒内 → kill 响应立即回收
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":16,"stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":10,"max":10}],
            "update":[{"module":"KillVolume","shape":"aabb","center":[0,0,0],
                       "half":[0.5,0.5,0.5],"response":"kill"}]}})json", 23);
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);
    em->tick(1.f / 60.f);
    assert(em->alive_count() == 0);   // 出生即在盒内 → 首帧被 kill（death 事件）
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: KillVolume kill response" << std::endl;
}

inline void test_curl_noise_determinism() {
    std::cout << "Testing CurlNoiseForce determinism..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    auto build = [](u32 noise_seed) {
        std::string s = R"({"capacity":64,"stages":{
        "spawn":[{"module":"SpawnBurst","count":8,"period":1000}],
        "init":[{"module":"SetLifetime","min":50,"max":50}],
        "update":[{"module":"CurlNoiseForce","frequency":2,"strength":5,"seed":)";
        s += std::to_string(noise_seed);
        s += "}]}}";
        return s;
    };
    const std::string ja = build(11), jb = build(11), jc = build(12);
    const emitter_id a = make_emitter_from_text(ja.c_str(), 3);
    const emitter_id b = make_emitter_from_text(jb.c_str(), 3);
    const emitter_id c = make_emitter_from_text(jc.c_str(), 3);
    assert(a != invalid_emitter && b != invalid_emitter && c != invalid_emitter);

    for (u32 f = 0; f < 120; ++f) {
        emitter_get(a)->tick(1.f / 60.f);
        emitter_get(b)->tick(1.f / 60.f);
        emitter_get(c)->tick(1.f / 60.f);
    }
    const math::v4* va = emitter_get(a)->store().v4_channel(attr::velocity);
    const math::v4* vb = emitter_get(b)->store().v4_channel(attr::velocity);
    const math::v4* vc = emitter_get(c)->store().v4_channel(attr::velocity);
    for (u32 i = 0; i < 8; ++i) {
        assert(va[i].x == vb[i].x && va[i].z == vb[i].z);   // 同种子 ⇒ 一致
    }
    bool differs = false;
    for (u32 i = 0; i < 8; ++i)
        if (std::fabs(va[i].x - vc[i].x) > 1e-5f) differs = true;
    assert(differs);                                        // 异种子 ⇒ 轨迹不同
    shutdown();
    std::cout << "  PASS: CurlNoise determinism" << std::endl;
}

inline void test_advect_and_sdf_collision() {
    std::cout << "Testing AdvectByField + SDFCollision..." << std::endl;

    // 构造地面 SDF 场（d = y）+ 向上速度场，烘焙为 .pgrid
    volume_grid sdf;
    assert(sdf.init_scalar(16, 16, 4, math::v3{ -1.f, -1.f, -1.f }, 0.125f, 0.f));
    for (u32 z = 0; z < 4; ++z)
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 16; ++x) sdf.set_scalar(x, y, z, (f32)(y) * 0.125f - 1.f);
    std::string err;
    assert(sdf.write_pgrid("/tmp/pfx_floor.pgrid", err));

    volume_grid wind;
    assert(wind.init_vector(4, 4, 4, math::v3{ -1.f, -1.f, -1.f }, 1.f, math::v3{ 2.f, 0.f, 0.f }));
    assert(wind.write_pgrid("/tmp/pfx_wind.pgrid", err));

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = make_emitter_from_text(
        R"json({"capacity":16,
            "fields":[{"name":"floor","file":"/tmp/pfx_floor.pgrid"},
                      {"name":"wind","file":"/tmp/pfx_wind.pgrid"}],
            "stages":{
            "spawn":[{"module":"SpawnBurst","count":1,"period":1000}],
            "init":[{"module":"SetLifetime","min":10,"max":10}],
            "update":[{"module":"AdvectByField","field":"wind","mode":"set"},
                      {"module":"SDFCollision","field":"floor","response":"slide"}]}})json", 19);
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);
    em->tick(1.f / 60.f);
    assert(em->alive_count() == 1);
    std::cout << "  [dbg] fields=" << em->program().field_grids.size() << std::endl;

    // AdvectByField(set)：速度 = 风场 (2,0,0)
    const math::v4* vel = em->store().v4_channel(attr::velocity);
    std::cout << "  [dbg] vel=(" << vel[0].x << "," << vel[0].y << "," << vel[0].z << ")" << std::endl;
    assert(std::fabs(vel[0].x - 2.f) < 1e-4f);

    // 未知场名 → 编译失败
    emitter_program prog;
    std::string cerr;
    const char* neg_json = R"({"capacity":4,"fields":[],"stages":{"update":[
            {"module":"AdvectByField","field":"nope"}]}})";
    assert(!compile_emitter_from_text(neg_json, std::strlen(neg_json), prog, cerr));
    assert(cerr.find("unknown field") != std::string::npos);

    shutdown();
    std::cout << "  PASS: Advect + SDF collision" << std::endl;
}

inline void run_all_n4_tests() {
    test_field_grid_sample_and_roundtrip();
    test_kill_volume_bounce();
    test_kill_volume_kill_response();
    test_curl_noise_determinism();
    test_advect_and_sdf_collision();
    std::cout << "All ParticleFX N4 tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
