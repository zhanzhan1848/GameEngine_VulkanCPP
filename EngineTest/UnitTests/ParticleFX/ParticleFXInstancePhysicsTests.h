#pragma once

// ParticleFXInstancePhysicsTests.h — 渲染 instance 声明 + PhysicsCollision 模块门禁。
// 覆盖：instance.mesh 解析（正常/类型错带行号/缺省回退）、PhysicsCollision 的
// bounce 反射 / friction 切向衰减 / kill_on_hit / 未安装适配器降级（脚本化 raycast，
// 假地板 y=0，不依赖 Jolt）、schema 反射含 PhysicsCollision。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/ParticleFX/interop/PhysicsQuery.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

using interop::raycast_hit;

// ---- 脚本化假地板 y=0（仅向下射线命中；不依赖 Simulation/Jolt） -----------------

struct floor_context {
    f32 y0{ 0.f };
};

inline bool floor_raycast(const math::v3& from, const math::v3& dir, f32 max_dist,
                          raycast_hit& out, void* user) {
    auto* fc = (floor_context*)user;
    if (dir.y >= -1e-6f) return false;         // 只向下打
    if (from.y <= fc->y0) return false;        // 已在/低于地板
    const f32 t = (from.y - fc->y0) / -dir.y;
    if (t > max_dist) return false;
    out.distance = t;
    out.point = math::v3{ from.x + dir.x * t, fc->y0, from.z + dir.z * t };
    out.normal = math::v3{ 0.f, 1.f, 0.f };
    return true;
}

inline void test_instance_mesh_parse() {
    std::cout << "Testing instance.mesh declaration parsing..." << std::endl;

    emitter_program prog;
    std::string err;

    const char* ok = R"({"renderer":"mesh",
        "instance":{"mesh":"Assets/Meshes/rock.mesh","guid":"abc"},
        "stages":{}})";
    assert(compile_emitter_from_text(ok, std::strlen(ok), prog, err));
    assert(prog.instance_mesh_path == "Assets/Meshes/rock.mesh");
    assert(prog.renderer_type == emitter_program::renderer_mesh);

    const char* no_instance = R"({"stages":{}})";
    assert(compile_emitter_from_text(no_instance, std::strlen(no_instance), prog, err));
    assert(prog.instance_mesh_path.empty());   // 缺席 = 内置立方体回退

    struct {
        const char* json;
        const char* expect_sub;
    } cases[] = {
        { R"({"instance":"rock"})", "must be an object" },       // 节非对象
        { R"({"instance":{"mesh":5}})", "must be a string" },    // mesh 非字符串
    };
    for (const auto& c : cases) {
        const bool ok2 = compile_emitter_from_text(c.json, std::strlen(c.json), prog, err);
        if (ok2 || err.find(c.expect_sub) == std::string::npos)
            std::cout << "  [dbg] ok=" << ok2 << " want='" << c.expect_sub << "' got='" << err
                      << "'" << std::endl;
        assert(!ok2);
        assert(err.find(c.expect_sub) != std::string::npos);
        assert(err.find("line ") != std::string::npos);   // 报错带行号
    }

    std::cout << "  PASS: instance.mesh parsing" << std::endl;
}

// 发射 1 粒子：point 出生于发射器位置，初速 (vx,vy,0)（自动归一方向 + 模长），
// update = [PhysicsCollision, IntegrateVelocity]（碰撞先于积分——probe 预积分位置）。dt=1/步。
inline emitter_id make_falling_emitter_unit(const char* module_json, f32 y0, f32 vx, f32 vy) {
    const f32 len = std::sqrt(vx * vx + vy * vy);
    char text[768];
    std::snprintf(text, sizeof(text),
                  R"({"capacity":16,"seed":7,
                      "stages":{
                        "spawn":[{"module":"SpawnRate","rate":1}],
                        "init":[{"module":"AddVelocity","mode":"fixed",
                                 "direction":[%f,%f,0],"speed_min":%f,"speed_max":%f},
                        {"module":"SetLifetime","min":600,"max":600}],
                        "update":[%s,{"module":"IntegrateVelocity"}]}})",
                  vx / len, vy / len, len, len, module_json);
    const emitter_id id = make_program_emitter(text, 7);
    assert(id != invalid_emitter);
    emitter_set_position(id, math::v3{ 0.f, y0, 0.f });
    return id;
}

// 同 make_falling_emitter_unit，但 spawn 用 SpawnBurst 单发（整个测试窗口仅 1 颗）。
inline emitter_id make_burst_falling_emitter(const char* module_json_inner, f32 y0, f32 vx, f32 vy) {
    const f32 len = std::sqrt(vx * vx + vy * vy);
    char text[768];
    std::snprintf(text, sizeof(text),
                  R"({"capacity":16,"seed":7,
                      "stages":{
                        "spawn":[{"module":"SpawnBurst","count":1,"period":600}],
                        "init":[{"module":"AddVelocity","mode":"fixed",
                                 "direction":[%f,%f,0],"speed_min":%f,"speed_max":%f},
                                {"module":"SetLifetime","min":600,"max":600}],
                        "update":[{%s},{"module":"IntegrateVelocity"}]}})",
                  vx / len, vy / len, len, len, module_json_inner);
    const emitter_id id = make_program_emitter(text, 7);
    assert(id != invalid_emitter);
    emitter_set_position(id, math::v3{ 0.f, y0, 0.f });
    return id;
}

inline void test_physics_collision_bounce() {
    std::cout << "Testing PhysicsCollision bounce reflection..." << std::endl;

    assert(initialize(system_config{ 4, 256 }));
    floor_context fc{};
    interop::physics_query::install(floor_raycast, &fc);

    // 从 y=5 以 (0,-2,0) 下落；bounce=1 全反射。dt=1，每步位移 2、探测距离 2.05，
    // 归一方向 |dir.y|=1 → t = from.y：
    // tick1 从 y=5 探测 t=5 > 2.05 未命中 → 积分 y=3；
    // tick2 从 y=3 探测 t=3 > 2.05 未命中 → 积分 y=1；
    // tick3 从 y=1 探测 t=1 ≤ 2.05 命中 → 反弹 v=(0,2,0)、pos=(0,0.05) → 积分 y=2.05。
    const emitter_id id = make_falling_emitter_unit(
        R"({"module":"PhysicsCollision","bounce":1.0,"friction":0.0})", 5.f, 0.f, -2.f);
    pfx_emitter* em = emitter_get(id);
    const math::v4* pos = em->store().v4_channel(attr::position);
    const math::v4* vel = em->store().v4_channel(attr::velocity);

    em->tick(1.f);
    assert(std::fabs(pos[0].y - 3.f) < 1e-4f);   // 尚未命中
    assert(vel[0].y < -1.9f);
    em->tick(1.f);
    assert(std::fabs(pos[0].y - 1.f) < 1e-4f);   // 尚未命中
    em->tick(1.f);                                // 命中并反弹
    assert(vel[0].y > 1.9f);                      // vy 反转为 +2（bounce=1）
    assert(pos[0].y > 1.5f && pos[0].y < 2.5f);   // ≈2.05（命中点 0.05 + 反弹积分 2）

    interop::physics_query::install(nullptr, nullptr);
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: bounce reflection" << std::endl;
}

inline void test_physics_collision_slide_friction() {
    std::cout << "Testing PhysicsCollision slide + tangential friction..." << std::endl;

    assert(initialize(system_config{ 4, 256 }));
    floor_context fc{};
    interop::physics_query::install(floor_raycast, &fc);

    // (1,-1,0)·√2 速度；bounce=0（去法向）、friction=0.5（切向减半）。
    // 速度模 √2，每步位移 √2；命中步后 vx 减半、vy≈0。
    const emitter_id id = make_falling_emitter_unit(
        R"({"module":"PhysicsCollision","bounce":0.0,"friction":0.5})", 5.f, 1.f, -1.f);
    pfx_emitter* em = emitter_get(id);
    const math::v4* vel = em->store().v4_channel(attr::velocity);

    f32 prev_vx = 2.f;
    bool bounced = false;
    for (u32 i = 0; i < 12; ++i) {
        em->tick(1.f);
        if (!bounced && std::fabs(vel[0].y) < 1e-4f) {
            bounced = true;
            // 命中步：vy≈0（bounce=0 去法向），vx 减半（friction=0.5）
            assert(vel[0].x > 0.45f && vel[0].x < 0.78f);   // ≈0.5~0.707（浮点路径差）
            assert(prev_vx > vel[0].x);                      // 确实衰减
        }
        prev_vx = vel[0].x;
    }
    assert(bounced);

    interop::physics_query::install(nullptr, nullptr);
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: slide + friction" << std::endl;
}

inline void test_physics_collision_kill_and_degrade() {
    std::cout << "Testing PhysicsCollision kill_on_hit + no-adapter degrade..." << std::endl;

    assert(initialize(system_config{ 4, 256 }));
    floor_context fc{};
    interop::physics_query::install(floor_raycast, &fc);

    emitter_id id = make_burst_falling_emitter(
        R"("module":"PhysicsCollision","kill_on_hit":true)", 3.f, 0.f, -1.f);
    pfx_emitter* em = emitter_get(id);
    em->tick(1.f);   // y=2（探测 t=3 > 1.05 未命中）
    if (em->alive_count() != 1)
        std::cout << "  [dbg] alive=" << em->alive_count() << std::endl;
    assert(em->alive_count() == 1);
    em->tick(1.f);   // y=1（t=2 > 1.05 未命中）
    assert(em->alive_count() == 1);
    em->tick(1.f);   // 从 y=1 探测 t=1 ≤ 1.05 → 命中 → kill
    assert(em->alive_count() == 0);
    emitter_destroy(id);

    // 降级路径：未安装适配器 → 模块无操作，粒子照常积分存活
    interop::physics_query::install(nullptr, nullptr);
    id = make_burst_falling_emitter(
        R"("module":"PhysicsCollision","kill_on_hit":true)", 3.f, 0.f, -1.f);
    pfx_emitter* em2 = emitter_get(id);
    const math::v4* pos = em2->store().v4_channel(attr::position);
    for (int i = 0; i < 4; ++i) em2->tick(1.f);
    assert(em2->alive_count() == 1);            // 不 kill
    assert(pos[0].y < 0.f);                     // 穿透地板（无碰撞降级）
    emitter_destroy(id);

    shutdown();
    std::cout << "  PASS: kill_on_hit + degrade" << std::endl;
}

inline void test_physics_collision_schema_and_parse() {
    std::cout << "Testing PhysicsCollision parse + schema reflection..." << std::endl;

    emitter_program prog;
    std::string err;
    const char* ok = R"({"exposed":[{"name":"HitRadius","default":0.05}],
        "stages":{"update":[
        {"module":"PhysicsCollision","radius":{"param":"HitRadius"},"bounce":0.5,
         "kill_on_hit":false}]}})";
    assert(compile_emitter_from_text(ok, std::strlen(ok), prog, err));

    struct {
        const char* json;
        const char* expect_sub;
    } cases[] = {
        { R"({"stages":{"update":[{"module":"PhysicsCollision","bounce":"x"}]}})",
          "must be a number or param binding" },
        { R"({"stages":{"update":[{"module":"PhysicsCollision","kill_on_hit":"yes"}]}})",
          "must be a bool" },
        { R"({"stages":{"update":[{"module":"PhysicsCollision","radius":{"param":"Missing"}}]}})",
          "unknown exposed param" },
    };
    for (const auto& c : cases) {
        const bool ok2 = compile_emitter_from_text(c.json, std::strlen(c.json), prog, err);
        assert(!ok2);
        assert(err.find(c.expect_sub) != std::string::npos);
    }

    const std::string schema = build_schema_json();
    assert(schema.find("\"PhysicsCollision\"") != std::string::npos);

    std::cout << "  PASS: parse + schema" << std::endl;
}

inline void run_all_instance_physics_tests() {
    test_instance_mesh_parse();
    test_physics_collision_bounce();
    test_physics_collision_slide_friction();
    test_physics_collision_kill_and_degrade();
    test_physics_collision_schema_and_parse();
    std::cout << "All ParticleFX instance/physics tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
