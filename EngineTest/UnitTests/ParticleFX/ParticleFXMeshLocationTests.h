#pragma once

// ParticleFXMeshLocationTests.h — ShapeLocation mesh 发射源门禁（mesh_surface 形状 +
// scatter 四模式 + 权重通道/bias + param_ref 化回归）。
// 覆盖：OBJ 加载与 CDF 烘焙（含退化路径）、四种 scatter 的分布界、last_normal 契约、
//       bias 幂极化、编译期错误路径（缺 mesh/路径不存在/mesh_volume 拒绝）、
//       radius/height/angle param_ref 化后的暴露参数覆盖、schema 反射。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/MeshGeometry.h"
#include "Engine/ParticleFX/core/PfxSystem.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

// ---- 测试资产：单位正方形 z=0（两三角形，对角线 x=y） -------------------------
// mode 0: 纯几何；mode 1: 顶点色（A=1 / B=0.1）+ uv0.u（A=1 / B=0.25），顶点按三角形复制
inline const char* write_test_obj(const char* path, int mode) {
    FILE* f = std::fopen(path, "wb");
    assert(f);
    if (mode == 0) {
        std::fputs("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                   "f 1 2 3\nf 1 3 4\n",
                   f);
    } else {
        // A: (0,0)(1,0)(1,1) r=1 u=1；B: (0,0)(1,1)(0,1) r=0.1 u=0.25
        std::fputs("v 0 0 0 1 1 1\nv 1 0 0 1 1 1\nv 1 1 0 1 1 1\n"
                   "v 0 0 0 0.1 0.1 0.1\nv 1 1 0 0.1 0.1 0.1\nv 0 1 0 0.1 0.1 0.1\n"
                   "vt 1 0\nvt 1 0\nvt 1 0\nvt 0.25 0\nvt 0.25 0\nvt 0.25 0\n"
                   "f 1/1 2/2 3/3\nf 4/4 5/5 6/6\n",
                   f);
    }
    std::fclose(f);
    return path;
}

inline void test_mesh_load_and_cdf() {
    std::cout << "Testing mesh_geometry OBJ load + CDF bake..." << std::endl;

    mesh_geometry m;
    std::string err;
    write_test_obj("/tmp/pfx_mesh_test_square.obj", 1);
    assert(m.load_obj("/tmp/pfx_mesh_test_square.obj", err));
    assert(m.triangle_count() == 2);
    assert(m.positions.size() == 6);

    // 面积分布：正方形两三角形等面积 → total = 1
    assert(m.area.valid);
    assert(std::fabs(m.area.total - 1.f) < 1e-5f);
    // 顶点色通道：A 密度 1、B 密度 0.1 → 概率权重比 10:1
    assert(m.color.valid);
    assert(std::fabs(m.color.max_dens - 1.f) < 1e-6f);
    assert(std::fabs(m.color.dens[1] - 0.1f) < 1e-6f);
    assert(m.color.total > m.area.total * 0.54f && m.color.total < m.area.total * 0.56f);
    // uv 通道：u 比 1:0.25
    assert(m.uv.valid);
    assert(std::fabs(m.uv.dens[1] - 0.25f) < 1e-6f);

    // CDF 抽取的边界
    assert(m.sample_triangle(m.area, 0.f) == 0);
    assert(m.sample_triangle(m.area, m.area.total * 0.999f) == 1);
    assert(m.sample_triangle(m.area, m.area.total) == 1);   // 夹到末项

    // 退化路径：无面 / 文件不存在
    {
        FILE* f = std::fopen("/tmp/pfx_mesh_test_noface.obj", "wb");
        assert(f);
        std::fputs("v 0 0 0\nv 1 0 0\nv 0 1 0\n", f);
        std::fclose(f);
    }
    mesh_geometry bad;
    // 无面 OBJ：tinyobj 不产出 shape → "no geometry"；有 shape 无三角形 → "no triangles"
    assert(!bad.load_obj("/tmp/pfx_mesh_test_noface.obj", err));
    assert(err.find("no geometry") != std::string::npos || err.find("no triangles") != std::string::npos);
    assert(!bad.load_obj("/tmp/pfx_mesh_test_missing.obj", err) && !err.empty());

    std::remove("/tmp/pfx_mesh_test_square.obj");
    std::remove("/tmp/pfx_mesh_test_noface.obj");
    std::cout << "  PASS: mesh_geometry load + CDF" << std::endl;
}

// 发射 N 个粒子到 store，返回 emitter（调用方负责 destroy）
inline pfx_emitter* spawn_mesh_particles(const char* obj_path, const char* scatter,
                                         const char* weight_channel, const char* bias,
                                         bool with_normal_velocity, u32 count) {
    char text[1024];
    if (with_normal_velocity) {
        std::snprintf(text, sizeof(text),
                      R"({"capacity":200000,"seed":13,"stages":{
            "spawn":[{"module":"SpawnRate","rate":%u}],
            "init":[{"module":"ShapeLocation","shape":"mesh_surface","mesh":"%s","scatter":"%s",
                     "weight_channel":"%s","bias":%s},
                    {"module":"SetLifetime","min":60,"max":60},
                    {"module":"AddVelocity","mode":"shape_normal","speed_min":2,"speed_max":2}]}})",
                      count, obj_path, scatter, weight_channel, bias);
    } else {
        std::snprintf(text, sizeof(text),
                      R"({"capacity":200000,"seed":13,"stages":{
            "spawn":[{"module":"SpawnRate","rate":%u}],
            "init":[{"module":"ShapeLocation","shape":"mesh_surface","mesh":"%s","scatter":"%s",
                     "weight_channel":"%s","bias":%s},
                    {"module":"SetLifetime","min":60,"max":60}]}})",
                      count, obj_path, scatter, weight_channel, bias);
    }
    const emitter_id id = make_program_emitter(text, 13);
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);
    em->tick(1.0f);
    assert(em->alive_count() == count);
    return em;
}

// 供 spawn_mesh_particles 的调用方销毁（helper 返回 em，destroy 走 id）
inline void destroy_emitter(pfx_emitter* em) { emitter_destroy(em->get_id()); }

// 三角形归属：单位正方形对角线 x=y，A 为 x≥y 侧、B 为 x<y 侧。
// 返回 1 = 在 A 侧（用于密度占比统计）。
inline u32 on_triangle_a(const math::v4& p) { return p.x >= p.y ? 1u : 0u; }

inline void test_mesh_surface_scatter_modes() {
    std::cout << "Testing mesh_surface scatter modes (area/triangle/vertex)..." << std::endl;

    assert(initialize(system_config{ 8, 200000 }));
    const char* path = write_test_obj("/tmp/pfx_mesh_test_square.obj", 0);
    constexpr u32 N = 100000;

    {
        pfx_emitter* em = spawn_mesh_particles(path, "area_uniform", "none", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) {
            // 面上：z=0 平面、单位正方形内
            assert(std::fabs(pos[i].z) < 1e-5f);
            assert(pos[i].x >= -1e-4f && pos[i].x <= 1.f + 1e-4f);
            assert(pos[i].y >= -1e-4f && pos[i].y <= 1.f + 1e-4f);
            count_a += on_triangle_a(pos[i]);
        }
        // 等面积 → 两三角形各约一半（±3% 门禁）
        const f32 frac = (f32)count_a / (f32)N;
        assert(frac > 0.47f && frac < 0.53f);
        destroy_emitter(em);
    }
    {
        pfx_emitter* em = spawn_mesh_particles(path, "triangle_uniform", "none", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) {
            assert(std::fabs(pos[i].z) < 1e-5f);
            count_a += on_triangle_a(pos[i]);
        }
        const f32 frac = (f32)count_a / (f32)N;
        assert(frac > 0.47f && frac < 0.53f);   // 每三角形等概率
        destroy_emitter(em);
    }
    {
        pfx_emitter* em = spawn_mesh_particles(path, "vertex", "none", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        for (u32 i = 0; i < N; ++i) {
            // 严格落在 4 顶点之一
            const bool v = (pos[i].x == 0.f || pos[i].x == 1.f) && (pos[i].y == 0.f || pos[i].y == 1.f)
                           && pos[i].z == 0.f;
            assert(v);
        }
        destroy_emitter(em);
    }

    std::remove(path);
    shutdown();
    std::cout << "  PASS: mesh_surface scatter modes" << std::endl;
}

inline void test_mesh_density_weighted_and_bias() {
    std::cout << "Testing density_weighted channels + bias polarization..." << std::endl;

    assert(initialize(system_config{ 8, 200000 }));
    const char* path = write_test_obj("/tmp/pfx_mesh_test_square.obj", 1);
    constexpr u32 N = 100000;

    {
        // 顶点色 1:0.1 → 约 90.9% 落 A
        pfx_emitter* em = spawn_mesh_particles(path, "density_weighted", "vertex_color_r", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) count_a += on_triangle_a(pos[i]);
        const f32 frac = (f32)count_a / (f32)N;
        assert(frac > 0.88f && frac < 0.94f);
        destroy_emitter(em);
    }
    {
        // bias=1 → power 16，B 的接受概率 (0.1)^15 → 实际全落 A
        pfx_emitter* em = spawn_mesh_particles(path, "density_weighted", "vertex_color_r", "1", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) count_a += on_triangle_a(pos[i]);
        assert((f32)count_a / (f32)N > 0.999f);
        destroy_emitter(em);
    }
    {
        // uv0_u 通道 1:0.25 → 约 80% 落 A；且权重通道不影响 area_uniform scatter
        pfx_emitter* em = spawn_mesh_particles(path, "density_weighted", "uv0_u", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) count_a += on_triangle_a(pos[i]);
        const f32 frac = (f32)count_a / (f32)N;
        assert(frac > 0.78f && frac < 0.82f);
        destroy_emitter(em);
    }
    {
        // 权重通道缺席（mode 0 OBJ 无顶点色）→ density_weighted 退化为面积均匀
        const char* plain = write_test_obj("/tmp/pfx_mesh_test_plain.obj", 0);
        pfx_emitter* em = spawn_mesh_particles(plain, "density_weighted", "vertex_color_r", "0", false, N);
        const math::v4* pos = em->store().v4_channel(attr::position);
        u32 count_a = 0;
        for (u32 i = 0; i < N; ++i) count_a += on_triangle_a(pos[i]);
        const f32 frac = (f32)count_a / (f32)N;
        assert(frac > 0.47f && frac < 0.53f);
        destroy_emitter(em);
        std::remove(plain);
    }

    std::remove(path);
    shutdown();
    std::cout << "  PASS: density_weighted + bias" << std::endl;
}

inline void test_mesh_normal_contract() {
    std::cout << "Testing mesh_surface last_normal contract..." << std::endl;

    assert(initialize(system_config{ 8, 200000 }));
    const char* path = write_test_obj("/tmp/pfx_mesh_test_square.obj", 0);
    // 两三角形绕向一致 → 面法线同为 +z；AddVelocity(shape_normal) 速度应恒 (0,0,2)
    pfx_emitter* em = spawn_mesh_particles(path, "area_uniform", "none", "0", true, 50000);
    const math::v4* vel = em->store().v4_channel(attr::velocity);
    for (u32 i = 0; i < 50000; ++i) {
        assert(std::fabs(vel[i].x) < 1e-4f && std::fabs(vel[i].y) < 1e-4f);
        assert(std::fabs(vel[i].z - 2.f) < 1e-4f);
    }
    destroy_emitter(em);
    std::remove(path);
    shutdown();
    std::cout << "  PASS: last_normal contract" << std::endl;
}

inline void test_mesh_compile_errors() {
    std::cout << "Testing mesh compile error paths..." << std::endl;

    struct {
        const char* json;
        const char* expect_sub;
    } cases[] = {
        // mesh_surface 缺 mesh 路径
        { R"({"stages":{"init":[{"module":"ShapeLocation","shape":"mesh_surface"}]}})",
          "requires a 'mesh'" },
        // 路径不存在
        { R"({"stages":{"init":[{"module":"ShapeLocation","shape":"mesh_surface",
                                   "mesh":"/tmp/pfx_mesh_test_missing.obj"}]}})",
          "load failed" },
        // mesh_volume v1 显式拒绝
        { R"({"stages":{"init":[{"module":"ShapeLocation","shape":"mesh_volume",
                                   "mesh":"/tmp/pfx_mesh_test_square.obj"}]}})",
          "not implemented" },
        // 未知 scatter 枚举
        { R"({"stages":{"init":[{"module":"ShapeLocation","shape":"point",
                                   "scatter":"whatever"}]}})",
          "unknown value" },
    };
    write_test_obj("/tmp/pfx_mesh_test_square.obj", 0);
    for (const auto& c : cases) {
        emitter_program prog;
        std::string err;
        const bool ok = compile_emitter_from_text(c.json, std::strlen(c.json), prog, err);
        if (ok || err.find(c.expect_sub) == std::string::npos)
            std::cout << "  [dbg] ok=" << ok << " want='" << c.expect_sub << "' got='" << err << "'"
                      << std::endl;
        assert(!ok);
        assert(err.find(c.expect_sub) != std::string::npos);
        assert(err.find("line ") != std::string::npos);   // 报错带行号
    }

    // 同路径两次引用 → 表内去重（一个 mesh_entry）
    const char* dup = R"({"stages":{"init":[
        {"module":"ShapeLocation","shape":"mesh_surface","mesh":"/tmp/pfx_mesh_test_square.obj"},
        {"module":"ShapeLocation","shape":"mesh_surface","mesh":"/tmp/pfx_mesh_test_square.obj"}]}})";
    emitter_program prog;
    std::string err;
    assert(compile_emitter_from_text(dup, std::strlen(dup), prog, err));
    assert(prog.meshes.size() == 1);
    assert(prog.mesh_views.size() == 1);

    std::remove("/tmp/pfx_mesh_test_square.obj");
    std::cout << "  PASS: mesh compile error paths" << std::endl;
}

inline void test_param_ref_shapes_regression() {
    std::cout << "Testing ShapeLocation param_ref exposure (radius/angle upgrade)..." << std::endl;

    // 旧资产形态（裸数字）继续可编译
    const char* legacy = R"({"capacity":1000,"seed":3,"stages":{
        "spawn":[{"module":"SpawnRate","rate":1000}],
        "init":[{"module":"ShapeLocation","shape":"sphere_surface","radius":5}]}})";
    assert(initialize(system_config{ 8, 8192 }));
    const emitter_id legacy_id = make_program_emitter(legacy, 3);
    assert(legacy_id != invalid_emitter);
    {
        pfx_emitter* em = emitter_get(legacy_id);
        em->tick(1.0f);
        const math::v4* pos = em->store().v4_channel(attr::position);
        for (u32 i = 0; i < em->alive_count(); ++i) {
            const f32 r = std::sqrt(pos[i].x * pos[i].x + pos[i].y * pos[i].y + pos[i].z * pos[i].z);
            assert(std::fabs(r - 5.f) < 1e-3f);
        }
    }
    emitter_destroy(legacy_id);

    // 绑定形态：exposed 覆盖 radius
    const char* bound = R"({"capacity":8000,"seed":3,
        "exposed":[{"name":"SpawnRadius","default":1}],
        "stages":{"spawn":[{"module":"SpawnRate","rate":5000}],
                  "init":[{"module":"ShapeLocation","shape":"sphere_surface",
                           "radius":{"param":"SpawnRadius"}}]}})";
    const emitter_id id = make_program_emitter(bound, 3);
    assert(id != invalid_emitter);
    assert(emitter_set_exposed_param(id, hash_name("SpawnRadius"), 3.f));
    {
        pfx_emitter* em = emitter_get(id);
        em->tick(1.0f);
        const math::v4* pos = em->store().v4_channel(attr::position);
        for (u32 i = 0; i < em->alive_count(); ++i) {
            const f32 r = std::sqrt(pos[i].x * pos[i].x + pos[i].y * pos[i].y + pos[i].z * pos[i].z);
            assert(std::fabs(r - 3.f) < 1e-3f);
        }
    }
    emitter_destroy(id);
    shutdown();
    std::cout << "  PASS: param_ref shapes regression" << std::endl;
}

inline void test_schema_mesh_hints() {
    std::cout << "Testing schema reflection of mesh hints..." << std::endl;

    const std::string schema = build_schema_json();
    assert(schema.find("mesh_surface") != std::string::npos);   // 枚举值在 note 内,不带引号
    assert(schema.find("mesh_volume") != std::string::npos);
    assert(schema.find("density_weighted") != std::string::npos);
    // radius/height/angle 升级为 param_ref 类型
    assert(schema.find("{\"name\":\"radius\",\"type\":\"param_ref\"") != std::string::npos);
    assert(schema.find("{\"name\":\"bias\",\"type\":\"param_ref\"") != std::string::npos);
    // mesh 参数为 string 类型
    assert(schema.find("{\"name\":\"mesh\",\"type\":\"string\"") != std::string::npos);

    std::cout << "  PASS: schema mesh hints" << std::endl;
}

inline void run_all_mesh_location_tests() {
    test_mesh_load_and_cdf();
    test_mesh_surface_scatter_modes();
    test_mesh_density_weighted_and_bias();
    test_mesh_normal_contract();
    test_mesh_compile_errors();
    test_param_ref_shapes_regression();
    test_schema_mesh_hints();
    std::cout << "All ParticleFX mesh location tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
