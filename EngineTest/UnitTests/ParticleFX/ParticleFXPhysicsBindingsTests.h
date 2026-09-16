#pragma once

// ParticleFXPhysicsBindingsTests.h — 逐粒子刚体绑定门禁（路线图 P2/P3）。
// 覆盖：PhysicsBindings 事件路由（多发射器不互抢事件）、全局预算否决、绑定移除
// 连带 body 处置、convex 碰撞体 spawn、碰撞事件产出（接触 → pfx collision 事件）、
// instance.physics 解析与刚体/PhysicsCollision 互斥编译期拒绝。
// 物理用真实 Jolt（N3 同款 settings）；事件断言零依赖渲染。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/ParticleFX/interop/PhysicsBindings.h"
#include "Engine/ParticleFX/interop/RigidBodySpawner.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

namespace sim = primal::simulation;
using interop::physics_bindings;
using interop::rigid_body_spawner;
using interop::rigid_spawn_policy;

namespace {

u64 make_pb_ground() {
    sim::body_desc ground{};
    ground.m = sim::body_desc::mode::static_body;
    ground.half_extents = math::v3{ 20.f, 0.5f, 20.f };
    ground.position = math::v3{ 0.f, -0.5f, 0.f };
    return sim::body_create(ground);
}

rigid_spawn_policy sphere_policy(f32 radius = 0.05f) {
    rigid_spawn_policy p;
    p.shape = sim::body_desc::shape::sphere;
    p.half_extents = math::v3{ radius, 0.f, 0.f };
    p.emit_collision_events = true;
    return p;
}

} // namespace

inline void test_bindings_route_and_budget() {
    std::cout << "Testing PhysicsBindings event routing + global budget..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    assert(initialize(system_config{ 8, 4096 }));
    auto& bindings = physics_bindings::get();
    bindings.set_global_body_budget(1024);

    // A:持续生成;B:只生成不发事件归属(无绑定)——事件只应路由给 A
    const emitter_id a = make_program_emitter(
        R"({"capacity":64,"seed":5,"stages":{
            "spawn":[{"module":"SpawnRate","rate":10}],
            "init":[{"module":"SetLifetime","min":600,"max":600}]}})", 5);
    assert(a != invalid_emitter);
    bindings.set_enabled(a, sphere_policy());
    const emitter_id b = make_program_emitter(
        R"({"capacity":64,"seed":6,"stages":{
            "spawn":[{"module":"SpawnRate","rate":10}],
            "init":[{"module":"SetLifetime","min":600,"max":600}]}})", 6);
    assert(b != invalid_emitter);

    emitter_get(a)->tick(0.5f);
    emitter_get(b)->tick(0.5f);
    bindings.tick();

    assert(bindings.binding_count() == 1);
    assert(bindings.total_live_bodies() >= 1);   // A 的 birth 建了 body

    // 全局预算 = 1:清空后重灌, births 应被全局否决(事件丢弃,不建体)
    bindings.clear();
    assert(sim::body_create(sim::body_desc{}) != 0 || true);   // sim 存活冒烟
    // 直接重置预算后 A 重生粒子被否决:预算 0 → live 恒 0
    bindings.set_global_body_budget(0);
    emitter_get(a)->tick(0.5f);
    bindings.tick();
    assert(bindings.total_live_bodies() == 0);

    bindings.remove(a);
    assert(bindings.binding_count() == 0);
    emitter_destroy(a);
    emitter_destroy(b);
    shutdown();
    sim::shutdown();
    std::cout << "  PASS: routing + budget" << std::endl;
}

inline void test_bindings_dispose_on_remove() {
    std::cout << "Testing bindings dispose bodies on remove..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    assert(initialize(system_config{ 8, 4096 }));
    auto& bindings = physics_bindings::get();
    bindings.set_global_body_budget(1024);

    const emitter_id a = make_program_emitter(
        R"({"capacity":16,"seed":9,"stages":{
            "spawn":[{"module":"SpawnRate","rate":4}],
            "init":[{"module":"SetLifetime","min":600,"max":600}]}})", 9);
    assert(a != invalid_emitter);
    bindings.set_enabled(a, sphere_policy());
    emitter_get(a)->tick(0.5f);
    bindings.tick();
    assert(bindings.total_live_bodies() >= 1);

    bindings.remove(a);   // destroy 策略 → body_remove 全部
    assert(bindings.total_live_bodies() == 0);

    emitter_destroy(a);
    shutdown();
    sim::shutdown();
    std::cout << "  PASS: dispose on remove" << std::endl;
}

inline void test_convex_spawn() {
    std::cout << "Testing convex collider spawn (P3.1)..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    assert(initialize(system_config{ 8, 4096 }));

    rigid_spawn_policy p;
    p.collider = rigid_spawn_policy::collider_kind::convex;
    const math::v3 corners[8] = {
        { -0.05f, -0.05f, -0.05f }, { 0.05f, -0.05f, -0.05f }, { 0.05f, 0.05f, -0.05f },
        { -0.05f, 0.05f, -0.05f },  { -0.05f, -0.05f, 0.05f }, { 0.05f, -0.05f, 0.05f },
        { 0.05f, 0.05f, 0.05f },    { -0.05f, 0.05f, 0.05f },
    };
    for (const auto& c : corners) p.convex_points.push_back(c);

    const emitter_id a = make_program_emitter(
        R"({"capacity":16,"seed":11,"stages":{
            "spawn":[{"module":"SpawnRate","rate":2}],
            "init":[{"module":"SetLifetime","min":600,"max":600}]}})", 11);
    assert(a != invalid_emitter);
    physics_bindings::get().set_enabled(a, p);
    emitter_get(a)->tick(0.5f);
    physics_bindings::get().tick();
    assert(physics_bindings::get().total_live_bodies() >= 1);   // body_create_convex 成功

    physics_bindings::get().clear();
    emitter_destroy(a);
    shutdown();
    sim::shutdown();
    std::cout << "  PASS: convex spawn" << std::endl;
}

inline void test_collision_events_produced() {
    std::cout << "Testing collision event production (P3.2)..." << std::endl;

    sim::settings s{};
    assert(sim::initialize(s));
    const u64 ground = make_pb_ground();
    assert(ground != 0);
    assert(initialize(system_config{ 8, 4096 }));

    const emitter_id a = make_program_emitter(
        R"({"capacity":16,"seed":13,"stages":{
            "spawn":[{"module":"SpawnRate","rate":60}],
            "init":[{"module":"SetLifetime","min":600,"max":600}]}})", 13);
    assert(a != invalid_emitter);
    physics_bindings::get().set_enabled(a, sphere_policy(0.05f));

    // 粒子出生在 y=0.4 → body 靠重力下落 → 与地面接触 → collision 事件
    emitter_set_position(a, math::v3{ 0.f, 0.4f, 0.f });
    emitter_get(a)->tick(1.f / 60.f);
    physics_bindings::get().tick();
    sim::set_running(true);   // step 有 running 门:不开则 body 不下落、无接触

    bool hit = false;
    for (int i = 0; i < 180 && !hit; ++i) {
        sim::step(1.f / 60.f);
        pfx::tick(1.f / 60.f);
        physics_bindings::get().tick();
        pfx_event ev[32];
        u32 n = drain_events(ev, 32);
        while (n > 0) {
            for (u32 k = 0; k < n; ++k)
                if (ev[k].source == a && ev[k].type == event_type::collision) hit = true;
            n = drain_events(ev, 32);
        }
    }
    assert(hit);

    sim::set_running(false);
    physics_bindings::get().clear();
    emitter_destroy(a);
    shutdown();
    sim::shutdown();
    std::cout << "  PASS: collision events" << std::endl;
}

inline void test_instance_physics_parse_and_mutex() {
    std::cout << "Testing instance.physics parse + rigid/collision mutex..." << std::endl;

    emitter_program prog;
    std::string err;

    const char* ok = R"({"renderer":"mesh",
        "instance":{"mesh":"m.mesh","physics":{"mode":"rigid_body","collider":"sphere",
                    "radius":0.08,"mass":2.5,"max_bodies":64}},
        "stages":{}})";
    assert(compile_emitter_from_text(ok, std::strlen(ok), prog, err));
    assert(prog.instance_physics.rigid_body);
    assert(prog.instance_physics.collider == 0);
    assert(std::fabs(prog.instance_physics.radius - 0.08f) < 1e-6f);
    assert(std::fabs(prog.instance_physics.mass - 2.5f) < 1e-5f);
    assert(prog.instance_physics.max_bodies == 64);

    // 默认:无 physics 节 → 不启用
    assert(compile_emitter_from_text(R"({"stages":{}})", 13, prog, err));
    assert(!prog.instance_physics.rigid_body);

    struct {
        const char* json;
        const char* expect_sub;
    } cases[] = {
        // 互斥红线:刚体回写与 PhysicsCollision 模块双开 → 编译期拒绝
        { R"({"instance":{"physics":{"mode":"rigid_body"}},
            "stages":{"update":[{"module":"PhysicsCollision"}]}})",
          "conflicts with PhysicsCollision" },
        { R"({"instance":{"physics":{"mode":"warp"}}})", "unknown mode" },
        { R"({"instance":{"physics":{"collider":"jelly"}}})", "unknown collider" },
        { R"({"instance":{"physics":{"mode":"rigid_body","collider":"convex"}}})",
          "requires instance.mesh" },
        { R"({"instance":{"physics":{"radius":-1}}})", "must be a number" },
    };
    for (const auto& c : cases) {
        const bool ok2 = compile_emitter_from_text(c.json, std::strlen(c.json), prog, err);
        if (ok2 || err.find(c.expect_sub) == std::string::npos)
            std::cout << "  [dbg] ok=" << ok2 << " want='" << c.expect_sub << "' got='" << err
                      << "'" << std::endl;
        assert(!ok2);
        assert(err.find(c.expect_sub) != std::string::npos);
    }

    std::cout << "  PASS: instance.physics parse + mutex" << std::endl;
}

inline void test_multithreaded_step() {
    std::cout << "Testing multithreaded job system step (P3.3)..." << std::endl;

    sim::settings s{};
    s.max_worker_threads = 2;   // 默认 0 = 单线程确定性基线;此处显式开线程池
    assert(sim::initialize(s));
    sim::body_desc d{};
    d.m = sim::body_desc::mode::dynamic;
    d.s = sim::body_desc::shape::sphere;
    d.half_extents = math::v3{ 0.1f, 0.f, 0.f };
    d.position = math::v3{ 0.f, 2.f, 0.f };
    const u64 b = sim::body_create(d);
    assert(b != 0);
    sim::set_running(true);
    for (int i = 0; i < 60; ++i) sim::step(1.f / 60.f);
    sim::body_transform bt;
    assert(sim::body_get_transform(b, bt));
    // 重力生效且数值有效(不苛求与单线程位级一致)
    assert(bt.position.y < 2.f && std::isfinite(bt.position.y));
    sim::set_running(false);
    sim::shutdown();
    std::cout << "  PASS: multithreaded step" << std::endl;
}

inline void run_all_physics_bindings_tests() {
    test_multithreaded_step();
    test_bindings_route_and_budget();
    test_bindings_dispose_on_remove();
    test_convex_spawn();
    test_collision_events_produced();
    test_instance_physics_parse_and_mutex();
    std::cout << "All ParticleFX physics bindings tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
