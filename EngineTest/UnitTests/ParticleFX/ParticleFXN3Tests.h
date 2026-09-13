#pragma once

// ParticleFXN3Tests.h — PFX N3 门禁测试（刚体消费者 + 物理查询，架构文档 §7-N3）。
// 覆盖：simulation::raycast（地面命中/法线）、RigidBodySpawner 生灭闭环（ADR3 物理权威
//       回写）、预算否决（reject/retire，ADR6）、确定性哈希门禁（沿物理文档纪律）。

#include "Engine/ParticleFX/interop/PhysicsQuery.h"
#include "Engine/ParticleFX/interop/RigidBodySpawner.h"
#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Simulation/SimulationAPI.h"

#include <cassert>
#include <cmath>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

namespace sim = primal::simulation;
using interop::rigid_body_spawner;
using interop::rigid_spawn_policy;

namespace {

// 地面：静态大盒，顶面 y=0
u64 make_ground() {
    sim::body_desc ground{};
    ground.m = sim::body_desc::mode::static_body;
    ground.half_extents = math::v3{ 20.f, 0.5f, 20.f };
    ground.position = math::v3{ 0.f, -0.5f, 0.f };
    const u64 id = sim::body_create(ground);
    return id;
}

} // namespace

inline void test_raycast_simulation() {
    std::cout << "Testing simulation::raycast ground hit..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    const u64 ground = make_ground();
    assert(ground != 0);
    interop::install_simulation_adapter();   // N3：Simulation 适配器注入

    interop::raycast_hit hit;
    const bool ok = interop::physics_query::raycast(math::v3{ 0.f, 5.f, 0.f }, math::v3{ 0.f, -1.f, 0.f },
                                                    100.f, hit);
    assert(ok);
    assert(std::fabs(hit.point.y - 0.f) < 1e-4f);              // 顶面 y=0
    assert(std::fabs(hit.normal.y - 1.f) < 1e-3f);             // 法线 +Y
    assert(std::fabs(hit.distance - 5.f) < 1e-3f);

    // 背向射线不命中
    assert(!interop::physics_query::raycast(math::v3{ 0.f, 5.f, 0.f }, math::v3{ 0.f, 1.f, 0.f }, 100.f,
                                            hit));
    // 零向量防御
    assert(!interop::physics_query::raycast(math::v3{ 0.f, 5.f, 0.f }, math::v3{ 0.f, 0.f, 0.f }, 1.f, hit));

    sim::shutdown();
    std::cout << "  PASS: simulation::raycast" << std::endl;
}

inline void test_spawner_lifecycle_and_writeback() {
    std::cout << "Testing RigidBodySpawner lifecycle + physics write-back..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    make_ground();
    sim::set_running(true);

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = emitter_create(emitter_init{ 8, 5, 0, nullptr });
    pfx_emitter* em = emitter_get(id);

    rigid_body_spawner spawner(id, rigid_spawn_policy{});

    // 3 粒：不同高度、向下初速
    for (u32 i = 0; i < 3; ++i) {
        const f32 y = 2.f + i * 0.5f;
        em->spawn(math::v3{ (f32)i, y, 0.f }, math::v3{ 0.f, -1.f, 0.f }, math::v4{ 1.f, 1.f, 1.f, 1.f },
                  math::v2{ 1.f, 1.f }, 10.f);
    }
    const f32 y0 = em->store().v4_channel(attr::position)[0].y;

    em->tick(0.f);              // 产生 birth 事件（不推进 age）
    spawner.tick(0.f, em->store());
    assert(spawner.live_bodies() == 3);

    // 物理权威回写（ADR3）：step 后粒子位置跟随 body 下落
    for (u32 i = 0; i < 30; ++i) {
        sim::step(1.f / 60.f);
        spawner.tick(0.f, em->store());
    }
    const f32 y1 = em->store().v4_channel(attr::position)[0].y;
    assert(y1 < y0 - 0.5f);     // 明显下落 = 物理在写粒子通道
    assert(spawner.live_bodies() == 3);

    // 粒子死亡 → body 按策略销毁
    for (u32 r = 0; r < 8; ++r) {
        if (em->store().is_row_alive(r)) em->store().free(em->store().row_handle(r));
    }
    spawner.tick(0.f, em->store());
    assert(spawner.live_bodies() == 0);

    // body 确实被移除：determinism 基线只剩地面
    shutdown();

    std::cout << "  PASS: RigidBodySpawner lifecycle + write-back" << std::endl;
}

inline void test_spawner_budget() {
    std::cout << "Testing RigidBodySpawner budget (reject/retire)..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    make_ground();

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = emitter_create(emitter_init{ 16, 7, 0, nullptr });
    pfx_emitter* em = emitter_get(id);

    // reject：预算 2，生成 5 → 2 活体 + 3 拒绝（粒子仍活）
    rigid_spawn_policy policy{};
    policy.max_live_bodies = 2;
    policy.overflow_policy = rigid_spawn_policy::overflow::reject_newest;
    rigid_body_spawner reject_spawner(id, policy);
    for (u32 i = 0; i < 5; ++i) {
        em->spawn(math::v3{ (f32)i * 0.1f, 3.f, 0.f }, math::v3{ 0.f, 0.f, 0.f },
                  math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 10.f);
    }
    em->tick(0.f);
    reject_spawner.tick(0.f, em->store());
    assert(reject_spawner.live_bodies() == 2);
    assert(reject_spawner.rejected_total() == 3);
    assert(em->alive_count() == 5);   // 粒子不受否决影响
    emitter_destroy(id);
    sim::shutdown();

    // retire：预算 2，生成 3 → 最老被挤（其粒子寿命被缩到当前 age）
    assert(sim::initialize(s));
    make_ground();
    const emitter_id id2 = emitter_create(emitter_init{ 16, 7, 0, nullptr });
    pfx_emitter* em2 = emitter_get(id2);
    rigid_spawn_policy retire_policy{};
    retire_policy.max_live_bodies = 2;
    retire_policy.overflow_policy = rigid_spawn_policy::overflow::retire_oldest;
    rigid_body_spawner retire_spawner(id2, retire_policy);
    for (u32 i = 0; i < 3; ++i) {
        em2->spawn(math::v3{ (f32)i * 0.1f, 3.f, 0.f }, math::v3{ 0.f, 0.f, 0.f },
                   math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 10.f);
    }
    em2->tick(0.f);
    retire_spawner.tick(0.f, em2->store());
    assert(retire_spawner.live_bodies() == 2);
    assert(retire_spawner.retired_total() == 1);
    // 被挤粒子的寿命 == 当前 age ⇒ 下一 emitter tick 到期回收（death 事件闭环）
    const math::v4* pos = em2->store().v4_channel(attr::position);
    const math::v4* vel = em2->store().v4_channel(attr::velocity);
    const f32 age0 = pos[0].w;
    assert(std::fabs(vel[0].w - age0) < 1e-6f);   // row 0 = 最老

    sim::shutdown();
    std::cout << "  PASS: RigidBodySpawner budget (reject/retire)" << std::endl;
}

inline void test_spawner_determinism() {
    std::cout << "Testing RigidBodySpawner determinism hash gate..." << std::endl;

    constexpr f32 dt = 1.f / 60.f;
    auto run = [&]() -> u64 {
        sim::settings s{};
        s.gravity = math::v3{ 0.f, -9.81f, 0.f };
        if (!sim::initialize(s)) return 0;
        make_ground();
        sim::set_running(true);

        assert(initialize(system_config{ 4, 4096 }));
        const char* json = R"({"capacity":32,"stages":{
            "spawn":[{"module":"SpawnBurst","count":6,"period":100}],
            "init":[{"module":"SetLifetime","min":30,"max":30},
                    {"module":"ShapeLocation","shape":"sphere_volume","radius":0.3},
                    {"module":"AddVelocity","mode":"random_sphere","speed_min":1,"speed_max":2}]}})";
        emitter_program prog;
        std::string err;
        assert(compile_emitter_from_text(json, std::strlen(json), prog, err));
        const emitter_id id = emitter_create_from_program(prog);
        pfx_emitter* em = emitter_get(id);

        rigid_body_spawner spawner(id, rigid_spawn_policy{});
        for (u32 f = 0; f < 120; ++f) {
            sim::step(dt);
            em->tick(dt);
            spawner.tick(dt, em->store());
        }
        const u64 h = sim::determinism_hash();
        shutdown();
        sim::shutdown();
        return h;
    };

    const u64 h1 = run();
    const u64 h2 = run();
    assert(h1 != 0);
    assert(h1 == h2);   // 同种子同步骤 ⇒ 位级一致
    std::cout << "  PASS: Spawner determinism (hash=" << h1 << ")" << std::endl;
}

inline void run_all_n3_tests() {
    test_raycast_simulation();
    test_spawner_lifecycle_and_writeback();
    test_spawner_budget();
    test_spawner_determinism();
    std::cout << "All ParticleFX N3 tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
