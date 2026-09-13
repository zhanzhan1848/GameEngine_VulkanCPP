#pragma once

// ParticleFXN6Tests.h — PFX N4 补充（GeometryCache/转换器骨架）与 N6 门禁
// （预算管理器/LOD），架构文档 §7。

#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/ParticleFX/render/RibbonTrail.h"
#include "Engine/ParticleFX/host/BudgetManager.h"
#include "Engine/ParticleFX/interop/LegacyPresetConvert.h"
#include "Engine/Particles/ParticlePreset.h"
#include "Engine/ParticleFX/interop/EmissiveGI.h"
#include "Engine/ParticleFX/interop/CacheConverters.h"
#include "Engine/ParticleFX/interop/GeometryCache.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

inline void test_geometry_cache_roundtrip() {
    std::cout << "Testing geometry cache (.pcache) round-trip..." << std::endl;

    interop::geometry_cache cache;
    // 两帧各 3 点（帧0 在 y=0，帧1 在 y=1）
    f32 frame0[9] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 2.f };
    f32 frame1[9] = { 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 2.f };
    assert(cache.add_frame(frame0, 3) == 0);
    assert(cache.add_frame(frame1, 3) == 1);

    std::string err;
    assert(cache.save("/tmp/pfx_test.pcache", err));
    interop::geometry_cache loaded;
    assert(loaded.load("/tmp/pfx_test.pcache", err));
    assert(loaded.frame_count() == 2 && loaded.point_count() == 3);

    // 采样：t=0 → 帧0；t=0.5s @2fps → 帧1；越界取末端（loop=false）
    math::v3 pts[3];
    assert(loaded.sample(0.f, 2.f, false, pts, 3) == 3);
    assert(pts[1].x == 1.f && pts[1].y == 0.f);
    assert(loaded.sample(0.5f, 2.f, false, pts, 3) == 3);
    assert(pts[0].y == 1.f);
    assert(loaded.sample(99.f, 2.f, false, pts, 3) == 3);
    // 容量不足 → 负的需求值
    assert(loaded.sample(0.f, 2.f, false, nullptr, 0) == -3);
    std::remove("/tmp/pfx_test.pcache");

    std::cout << "  PASS: GeometryCache round-trip" << std::endl;
}

inline void test_converter_skeletons() {
    std::cout << "Testing cache converter skeletons (failure semantics)..." << std::endl;

    std::string err;
    assert(!pfx::convert::vdb_converter_available());
    assert(!pfx::convert::alembic_converter_available());
    assert(!pfx::convert::vdb_to_pgrid("in.vdb", "vel", "out.pgrid", err));
    assert(err.find("OpenVDB") != std::string::npos);
    assert(!pfx::convert::alembic_to_pcache("in.abc", "/obj/points", 24.f, "out.pcache", err));
    assert(err.find("Alembic") != std::string::npos);

    std::cout << "  PASS: Converter skeletons" << std::endl;
}

inline void test_budget_manager() {
    std::cout << "Testing budget manager (LOD + approvals)..." << std::endl;

    host::budget_manager budget;

    // LOD 因子：near 内 1.0；far 外 min；中间线性
    assert(budget.lod_factor(5.f) == 1.f);
    assert(std::fabs(budget.lod_factor(200.f) - 0.1f) < 1e-5f);
    const f32 mid = budget.lod_factor(70.f);   // (70-20)/100 = 0.5 → 1 - 0.5*0.9 = 0.55
    assert(std::fabs(mid - 0.55f) < 1e-5f);

    // 预算否决
    assert(budget.approve_spawn(1000, 500) == 500);
    assert(budget.approve_spawn(199900, 500) == 100);
    assert(budget.approve_spawn(200000, 1) == 0);
    assert(budget.approve_emitter(31) && !budget.approve_emitter(32));

    std::cout << "  PASS: Budget manager" << std::endl;
}

inline void test_ribbon_trail() {
    std::cout << "Testing ribbon trail (ring/death-truncation/packing)..." << std::endl;

    assert(initialize(system_config{ 4, 4096 }));
    const emitter_id id = emitter_create(emitter_init{ 4, 3, 0, nullptr });
    pfx_emitter* em = emitter_get(id);
    const particle_handle h = em->spawn(math::v3{ 0.f, 0.f, 0.f }, math::v3{ 1.f, 0.f, 0.f },
                                        math::v4{ 1.f, 1.f, 1.f, 1.f }, math::v2{ 1.f, 1.f }, 10.f);
    assert(h.is_valid());

    render::ribbon_trail trail;
    trail.configure(8, 1.f / 60.f, 0.2f);
    trail.attach(h, &em->store());
    assert(trail.is_attached());

    // 记录 16 帧（dt 1/60）：环形容量 8 ⇒ 样本数封顶 8
    for (u32 f = 0; f < 16; ++f) {
        em->tick(1.f / 60.f);
        trail.record(1.f / 60.f);
    }
    assert(trail.sample_count() == 8);

    // 打包拓扑：8 样本 → 16 顶点、42 索引；首尾 uv 正确
    render::ribbon_vertex verts[16];
    u16 indices[64];
    const u32 vc = trail.build_strip(math::v3{ 1.f, 0.f, 0.f }, verts, 16, indices, 64);
    assert(vc == 8);
    assert(trail.sample_count() * 2 == 16);
    assert(std::fabs(verts[0].uv[0] - 0.f) < 1e-6f && std::fabs(verts[0].uv[1] - 0.f) < 1e-6f);
    assert(std::fabs(verts[15].uv[0] - 1.f) < 1e-6f && std::fabs(verts[15].uv[1] - 1.f) < 1e-6f);
    assert(indices[0] == 0 && indices[1] == 1 && indices[2] == 2 && indices[3] == 0);
    // 横向展开沿 camera_right：左顶点 x < 右顶点 x
    assert(verts[0].pos[0] < verts[1].pos[0]);

    // 死亡截断：粒子死亡 ⇒ 记录清空、attach 解除
    em->store().free(h);
    trail.record(1.f / 60.f);
    assert(!trail.is_attached());
    assert(trail.sample_count() == 0);
    // 死亡后打包 = 0 顶点
    assert(trail.build_strip(math::v3{ 1.f, 0.f, 0.f }, verts, 16, indices, 64) == 0);

    shutdown();
    std::cout << "  PASS: Ribbon trail (wrap/truncation/topology)" << std::endl;
}

inline void test_emissive_gi_injector() {
    std::cout << "Testing emissive->GI injector (aggregate + clamp)..." << std::endl;

    interop::emissive_gi_config cfg{};   // reserved 1023, scale 1, min_alive 1
    interop::emissive_gi_injector inj(cfg);

    // 记录写入
    struct writer : interop::emissive_writer {
        u32 id = u32_invalid_id;
        math::v3 rgb{};
        int calls = 0;
        void write(u32 i, math::v3 v) override { id = i; rgb = v; ++calls; }
    } w;

    // 两光斑聚合：color(1,0,0)×2 + color(0,1,0)×3 ⇒ (2,3,0)
    std::vector<interop::light_sample> ls = {
        { math::v3{ 1.f, 0.f, 0.f }, 2.f },
        { math::v3{ 0.f, 1.f, 0.f }, 3.f },
    };
    assert(inj.tick(ls, w));
    assert(w.calls == 1 && w.id == 1023);
    assert(std::fabs(w.rgb.x - 2.f) < 1e-5f && std::fabs(w.rgb.y - 3.f) < 1e-5f);

    // 8.0 钳制：radiance_scale 放大使聚合超限
    interop::emissive_gi_config big_cfg{};
    big_cfg.radiance_scale = 100.f;
    interop::emissive_gi_injector big(big_cfg);
    assert(big.tick(ls, w));
    assert(w.rgb.x == interop::kMaxEmissiveRadiance && w.rgb.y == interop::kMaxEmissiveRadiance);
    assert(w.rgb.z == 0.f);   // 零通道不因钳制抬升

    // 负强度防御：不产生负 radiance
    std::vector<interop::light_sample> neg = { { math::v3{ 1.f, 1.f, 1.f }, -5.f } };
    interop::emissive_gi_config zero_cfg{};
    zero_cfg.min_alive = 1;
    interop::emissive_gi_injector zero(zero_cfg);
    math::v3 agg;
    assert(zero.aggregate(neg, agg));
    assert(agg.x == 0.f && agg.y == 0.f && agg.z == 0.f);

    std::cout << "  PASS: Emissive->GI injector (clamp 8.0, T4.4 门禁复用)" << std::endl;
}

inline void test_convert_emits_overlife_modules() {
    std::cout << "Testing converted presets emit over-life modules..." << std::endl;

    if (!primal::particles::preset_system::is_initialized()) {
        primal::particles::preset_system::initialize();
        primal::particles::default_presets::register_defaults(*primal::particles::preset_system::get());
    }
    u32 with_blend = 0;
    for (const auto& name : primal::particles::preset_system::get()->get_all_preset_names()) {
        auto* preset = primal::particles::preset_system::get()->get_preset(name);
        if (!preset) continue;
        std::string json, err;
        assert(pfx::convert::convert_preset_to_json(*preset, json, err));
        if (preset->use_scale_curve) assert(json.find("ScaleOverLife") != std::string::npos);
        if (preset->use_alpha_curve) assert(json.find("AlphaOverLife") != std::string::npos);
        if (preset->use_force_curve) assert(json.find("ForceOverLife") != std::string::npos);
        if (json.find("\"blend\"") != std::string::npos) ++with_blend;
    }
    assert(with_blend > 0);
    std::cout << "  PASS: Converted presets emit over-life modules (" << with_blend << " with blend)" << std::endl;
}

inline void run_all_n6_tests() {
    test_geometry_cache_roundtrip();
    test_converter_skeletons();
    test_budget_manager();
    test_ribbon_trail();
    test_emissive_gi_injector();
    test_convert_emits_overlife_modules();
    std::cout << "All ParticleFX N6 tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
