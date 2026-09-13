#pragma once

// ParticleFXN1Tests.h — PFX N1 门禁测试（模块堆栈 + JSON 编译器，架构文档 §7-N1）。
// 覆盖：JSON 解析与行号报错、编译期校验失败路径、内置模块行为、形状分布界、
//       暴露参数绑定、SpawnRate/Burst 节奏、热重载、确定性、schema 反射。

#include "Engine/ParticleFX/core/EmitterProgram.h"
#include "Engine/ParticleFX/core/PfxJson.h"
#include "Engine/ParticleFX/core/PfxSystem.h"
#include "Engine/ParticleFX/interop/LegacyPresetConvert.h"
#include "Engine/Particles/ParticlePreset.h"
#include "Engine/Particles/ParticleSystem.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::test {

using json::value;

// ---- JSON 解析器 -------------------------------------------------------------

inline void test_json_parse_ok() {
    std::cout << "Testing JSON parser happy path..." << std::endl;

    const char* text = R"({
        "name": "jet",
        "capacity": 2048,
        "rate": 12.5,
        "neg": -3,
        "flag": true,
        "off": false,
        "nothing": null,
        "esc": "a\"b\\c\n",
        "arr": [1, 2, 3],
        // 行注释容忍
        "obj": {"inner": "v"}
    })";
    const json::parse_result r = json::parse(text, std::strlen(text));
    assert(r.ok);
    const value& root = *r.root;
    assert(root.is_object());
    assert(root.find("name")->as_string() == "jet");
    assert(root.find("capacity")->as_u32() == 2048);
    assert(std::fabs(root.find("rate")->as_f32() - 12.5f) < 1e-6f);
    assert(root.find("neg")->as_f32() == -3.f);
    assert(root.find("flag")->as_bool());
    assert(!root.find("off")->as_bool());
    assert(root.find("nothing")->is_null());
    assert(root.find("esc")->as_string() == "a\"b\\c\n");
    assert(root.find("arr")->array_size() == 3);
    assert(root.find("arr")->array_at(2)->as_f32() == 3.f);
    assert(root.find("obj")->find("inner")->as_string() == "v");
    // 行号：name 在第 2 行
    assert(root.find("name")->line() == 2);
    std::cout << "  PASS: JSON parse happy path" << std::endl;
}

inline void test_json_parse_errors() {
    std::cout << "Testing JSON parser error reporting..." << std::endl;

    struct case_ {
        const char* text;
        const char* expect_sub;
        u32 expect_line;
    };
    const case_ cases[] = {
        { "{\n  \"a\" 1\n}", "expected ':'", 2 },
        { "{\n  \"a\": \"unterminated\n}", "control character in string", 2 },
        { "[1, 2", "unterminated array", 1 },
        { "{\"a\": 1} extra", "trailing content", 1 },
        { "{\n\n  \"a\": tru x}", "invalid literal", 3 },
    };
    for (const case_& c : cases) {
        const json::parse_result r = json::parse(c.text, std::strlen(c.text));
        assert(!r.ok);
        assert(r.error.find(c.expect_sub) != std::string::npos);
        assert(r.line == c.expect_line);
    }
    // 空输入防御
    assert(!json::parse(nullptr, 0).ok);
    assert(!json::parse("", 0).ok);

    std::cout << "  PASS: JSON parse errors (line numbers)" << std::endl;
}

// ---- 编译器 ------------------------------------------------------------------

inline void test_compile_minimal() {
    std::cout << "Testing emitter program compile (minimal)..." << std::endl;

    const char* text = R"({
        "name": "minimal",
        "capacity": 500,
        "seed": 7,
        "stages": { "spawn": [ { "module": "SpawnRate", "rate": 100 } ] }
    })";
    emitter_program prog;
    std::string err;
    assert(compile_emitter_from_text(text, std::strlen(text), prog, err));
    assert(prog.name == "minimal");
    assert(prog.capacity == 500);
    assert(prog.seed == 7);
    assert(prog.stages[0].size() == 1);
    assert(prog.stages[1].empty() && prog.stages[2].empty());
    assert(prog.state_bytes == 0);   // SpawnRate 无状态

    std::cout << "  PASS: Compile minimal program" << std::endl;
}

inline void test_compile_error_paths() {
    std::cout << "Testing emitter program compile error paths..." << std::endl;

    std::string err;
    struct case_ {
        const char* json;
        const char* expect_sub;
    };
    const case_ cases[] = {
        { "{\"stages\":{\"spawn\":[{\"module\":\"Nope\"}]}}", "unknown module" },
        { "{\"stages\":{\"spawn\":[{\"rate\":10}]}}", "missing field 'module'" },
        { "{\"stages\":{\"spawn\":[{\"module\":\"SpawnRate\",\"rate\":\"x\"}]}}", "must be a number or param binding" },
        { "{\"stages\":{\"update\":[{\"module\":\"SpawnRate\"}]}}", "belongs to stage 'spawn'" },
        { "{\"stages\":[]}", "must be an object" },
        { "{\"attributes\":[{\"name\":\"a\"},{\"name\":\"a\"}]}", "duplicate attribute" },
        { "{\"capacity\":0}", "must be positive" },
        { "{\"exposed\":[{\"name\":\"X\"}],\"stages\":{\"spawn\":[{\"module\":\"SpawnRate\",\"rate\":{\"param\":\"Y\"}}]}}",
          "unknown exposed param" },
        { "[]", "must be a JSON object" },
    };
    for (const case_& c : cases) {
        emitter_program prog;
        err.clear();
        const bool ok = compile_emitter_from_text(c.json, std::strlen(c.json), prog, err);
        if (ok || err.find(c.expect_sub) == std::string::npos)
            std::cout << "  [dbg] ok=" << ok << " want='" << c.expect_sub << "' got='" << err << "'"
                      << std::endl;
        assert(!ok);
        assert(err.find(c.expect_sub) != std::string::npos);
        // 报错带行号标注
        assert(err.find("line ") != std::string::npos);
    }

    std::cout << "  PASS: Compile error paths" << std::endl;
}

// ---- 模块行为 ----------------------------------------------------------------

inline emitter_id make_program_emitter(const char* json_text, u32 seed = 42) {
    // 注入 seed：文本里不带 seed 时由包装器替换 —— 简化：文本自带 "seed"
    emitter_program prog;
    std::string err;
    if (!compile_emitter_from_text(json_text, std::strlen(json_text), prog, err)) {
        std::cout << "  [test-compile-fail] " << err << std::endl;
        return invalid_emitter;
    }
    if (seed != 0xFFFFFFFFu) prog.seed = seed;
    return emitter_create_from_program(prog);
}

inline void test_program_spawn_rate() {
    std::cout << "Testing program SpawnRate pacing..." << std::endl;

    assert(initialize(system_config{ 8, 4096 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":1000,"seed":1,"stages":{"spawn":[{"module":"SpawnRate","rate":100}],
            "init":[{"module":"SetLifetime","min":2,"max":2}]}})", 1);
    assert(id != invalid_emitter);
    pfx_emitter* em = emitter_get(id);

    em->tick(1.0f);
    assert(em->alive_count() == 100);
    em->tick(0.5f);
    assert(em->alive_count() == 150);   // 50 = 0.5s 累积（首帧余数并入）
    em->tick(0.005f);
    assert(em->alive_count() == 150);   // 0.5 个未成整

    u32 n = drain_events(nullptr, 0);   // cap=0 安全
    (void)n;
    shutdown();

    std::cout << "  PASS: Program SpawnRate pacing" << std::endl;
}

inline void test_program_spawn_burst() {
    std::cout << "Testing program SpawnBurst..." << std::endl;

    assert(initialize(system_config{ 8, 4096 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":1000,"seed":1,"stages":{"spawn":[
            {"module":"SpawnBurst","count":50,"period":2}],
            "init":[{"module":"SetLifetime","min":10,"max":10}]}})", 1);
    pfx_emitter* em = emitter_get(id);

    em->tick(0.1f);
    assert(em->alive_count() == 50);    // 首发立即
    em->tick(1.0f);
    assert(em->alive_count() == 50);
    em->tick(1.0f);                     // timer 2.0 ≥ period 2
    assert(em->alive_count() == 100);
    shutdown();

    std::cout << "  PASS: Program SpawnBurst" << std::endl;
}

inline void test_program_init_modules() {
    std::cout << "Testing program init modules (ShapeLocation/Lifetime/Color/Scale)..." << std::endl;

    assert(initialize(system_config{ 8, 8192 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":100000,"seed":3,"stages":{
            "spawn":[{"module":"SpawnRate","rate":0}]}})", 3);
    // 直接 spawn 模式做 init 检查更繁琐 —— 用 rate 一次性生成：SpawnRate rate 大 + 1 tick
    emitter_destroy(id);
    const emitter_id id2 = make_program_emitter(
        R"({"capacity":100000,"seed":3,"stages":{
            "spawn":[{"module":"SpawnRate","rate":100000}],
            "init":[
                {"module":"ShapeLocation","shape":"sphere_surface","radius":5},
                {"module":"SetLifetime","min":2,"max":3},
                {"module":"SetColor","color":[1,0,0,1],"color_end":[0,0,1,1]},
                {"module":"SetScale","scale":[1,2],"scale_end":[3,4]}
            ]}})", 3);
    assert(id2 != invalid_emitter);
    pfx_emitter* em = emitter_get(id2);
    em->tick(1.0f);
    assert(em->alive_count() == 100000);

    // 分布界：球面半径恒 5；寿命 ∈ [2,3]；颜色/缩放在插值域内。
    const math::v4* pos = em->store().v4_channel(attr::position);
    const math::v4* vel = em->store().v4_channel(attr::velocity);
    const math::v4* col = em->store().v4_channel(attr::color);
    const math::v4* scl = em->store().v4_channel(attr::scale_rotation);
    f32 max_r_err = 0.f;
    for (u32 i = 0; i < 100000; ++i) {
        const f32 r = std::sqrt(pos[i].x * pos[i].x + pos[i].y * pos[i].y + pos[i].z * pos[i].z);
        max_r_err = std::fabs(r - 5.f) > max_r_err ? std::fabs(r - 5.f) : max_r_err;
        assert(vel[i].w >= 2.f && vel[i].w <= 3.f);
        assert(col[i].x >= 0.f && col[i].x <= 1.f);
        assert(scl[i].x >= 1.f && scl[i].x <= 3.f && scl[i].y >= 2.f && scl[i].y <= 4.f);
    }
    assert(max_r_err < 1e-3f);   // 形状分布门禁：球面半径全落 5±1e-3

    shutdown();
    std::cout << "  PASS: Program init modules (sphere radius max_err=" << max_r_err << ")" << std::endl;
}

inline void test_program_update_forces() {
    std::cout << "Testing program update forces (Gravity/Drag/KillAge)..." << std::endl;

    assert(initialize(system_config{ 8, 4096 }));
    const emitter_id id = make_program_emitter(
        R"({"capacity":16,"seed":5,"stages":{
            "spawn":[{"module":"SpawnRate","rate":10}],
            "init":[{"module":"SetLifetime","min":2,"max":2}],
            "update":[{"module":"GravityForce","gravity":[0,-10,0]},{"module":"DragForce","coefficient":10}]}})",
        5);
    pfx_emitter* em = emitter_get(id);
    em->tick(1.0f);
    assert(em->alive_count() == 10);

    // 半隐式欧拉一步 dt=0.1：v = g*dt 后阻尼 *0（c*dt=1 → 全阻尼）
    em->tick(0.1f);
    const math::v4* vel = em->store().v4_channel(attr::velocity);
    assert(vel[0].y == 0.f);   // -1 被完全阻尼

    // 重开一个无阻尼发射器验证重力积分
    const emitter_id id2 = make_program_emitter(
        R"({"capacity":16,"seed":5,"stages":{
            "spawn":[{"module":"SpawnRate","rate":10}],
            "init":[{"module":"SetLifetime","min":10,"max":10},{"module":"AddVelocity","mode":"fixed","direction":[0,0,0],"speed_min":0,"speed_max":0}],
            "update":[{"module":"GravityForce","gravity":[0,-10,0]}]}})", 5);
    pfx_emitter* em2 = emitter_get(id2);
    em2->tick(1.0f);
    em2->tick(0.5f);
    // 半隐式欧拉逐帧语义：生成帧即积分（-10*1.0），次帧 -10*0.5 ⇒ 合计 -15
    const math::v4* vel2 = em2->store().v4_channel(attr::velocity);
    for (u32 i = 0; i < 10; ++i) assert(std::fabs(vel2[i].y - (-15.f)) < 1e-4f);

    // KillAge：寿命 10s、normalized_max 0.15（=1.5s 提前杀）
    const emitter_id id3 = make_program_emitter(
        R"({"capacity":16,"seed":5,"stages":{
            "spawn":[{"module":"SpawnBurst","count":10,"period":10}],
            "init":[{"module":"SetLifetime","min":10,"max":10}],
            "update":[{"module":"KillAge","normalized_max":0.15}]}})", 5);
    pfx_emitter* em3 = emitter_get(id3);
    // 模块在 age 推进之前评估：tick 内 KillAge 看到的是上帧末的 age
    em3->tick(1.0f);
    assert(em3->alive_count() == 10);   // 模块时 age=0
    em3->tick(0.4f);
    assert(em3->alive_count() == 10);   // 模块时 age=1.0/10 = 0.1 < 0.15
    em3->tick(0.2f);
    assert(em3->alive_count() == 10);   // 模块时 age=1.4/10 = 0.14 < 0.15
    em3->tick(0.2f);
    assert(em3->alive_count() == 0);    // 模块时 age=1.6/10 = 0.16 ≥ 0.15 → 杀

    shutdown();
    std::cout << "  PASS: Program update forces" << std::endl;
}

inline void test_program_determinism() {
    std::cout << "Testing program determinism (same seed)..." << std::endl;

    const char* text = R"({"capacity":64,"stages":{
        "spawn":[{"module":"SpawnRate","rate":10}],
        "init":[{"module":"SetLifetime","min":10,"max":10},
                {"module":"ShapeLocation","shape":"sphere_volume","radius":2},
                {"module":"AddVelocity","mode":"random_sphere","speed_min":1,"speed_max":3}]}})";
    assert(initialize(system_config{ 8, 4096 }));
    const emitter_id a = make_program_emitter(text, 777);
    const emitter_id b = make_program_emitter(text, 777);
    emitter_get(a)->tick(1.0f);
    emitter_get(b)->tick(1.0f);
    const math::v4* pa = emitter_get(a)->store().v4_channel(attr::position);
    const math::v4* pb = emitter_get(b)->store().v4_channel(attr::position);
    const math::v4* va = emitter_get(a)->store().v4_channel(attr::velocity);
    const math::v4* vb = emitter_get(b)->store().v4_channel(attr::velocity);
    for (u32 i = 0; i < 10; ++i) {
        assert(pa[i].x == pb[i].x && pa[i].z == pb[i].z);
        assert(va[i].y == vb[i].y);
    }
    shutdown();
    std::cout << "  PASS: Program determinism" << std::endl;
}

// ---- 暴露参数与热重载 ----------------------------------------------------------

inline void test_exposed_params_and_reload() {
    std::cout << "Testing exposed params binding and hot reload..." << std::endl;

    const char* text = R"({
        "capacity":1000,"seed":9,
        "exposed":[{"name":"Intensity","default":5}],
        "stages":{"spawn":[{"module":"SpawnRate","rate":{"param":"Intensity"}}],
                  "init":[{"module":"SetLifetime","min":10,"max":10}]}})";
    assert(initialize(system_config{ 8, 4096 }));
    const emitter_id id = make_program_emitter(text, 9);
    pfx_emitter* em = emitter_get(id);

    // 绑定生效：exposed 默认 5 → 每秒 5 个
    em->tick(1.0f);
    assert(em->alive_count() == 5);

    // runtime 改暴露参数
    const u32 h = hash_name("Intensity");
    assert(emitter_set_exposed_param(id, h, 25.f));
    f32 got = 0.f;
    assert(emitter_get_exposed_param(id, h, got) && got == 25.f);
    em->tick(1.0f);
    assert(em->alive_count() == 30);
    assert(!emitter_set_exposed_param(id, hash_name("Missing"), 1.f));

    // 热重载：改默认值 7 → 同 id、状态清零、默认生效
    const char* text2 = R"({
        "capacity":2000,"seed":11,
        "exposed":[{"name":"Intensity","default":7}],
        "stages":{"spawn":[{"module":"SpawnRate","rate":{"param":"Intensity"}}],
                  "init":[{"module":"SetLifetime","min":10,"max":10}]}})";
    emitter_program prog2;
    std::string err;
    assert(compile_emitter_from_text(text2, std::strlen(text2), prog2, err));
    assert(em->rebind_program(prog2));
    assert(em->elapsed() == 0.f);
    assert(em->alive_count() == 0);
    assert(em->capacity() == 2000);
    em->tick(1.0f);
    assert(em->alive_count() == 7);

    // 直接 spawn 模式不支持热重载
    const emitter_id legacy = emitter_create(emitter_init{ 8, 1, 0, nullptr });
    assert(!emitter_get(legacy)->rebind_program(prog2));

    shutdown();
    std::cout << "  PASS: Exposed params and hot reload" << std::endl;
}

// ---- schema 反射 ---------------------------------------------------------------

inline void test_schema_json() {
    std::cout << "Testing schema reflection JSON..." << std::endl;

    const std::string schema = emitter_schema_json();
    assert(schema.find("\"SpawnRate\"") != std::string::npos);
    assert(schema.find("\"ShapeLocation\"") != std::string::npos);
    assert(schema.find("\"KillAge\"") != std::string::npos);
    assert(schema.find("\"stage\":\"spawn\"") != std::string::npos);
    assert(schema.find("\"params\":[{\"name\":\"rate\",\"type\":\"param_ref\"") != std::string::npos);
    assert(schema.find("\"standard_attributes\"") != std::string::npos);
    std::cout << "  PASS: Schema reflection (" << schema.size() << " bytes)" << std::endl;
}

// ---- 资产文件加载 ---------------------------------------------------------------

inline void test_asset_file_load() {
    std::cout << "Testing emitter asset file load..." << std::endl;

    const char* path = "/tmp/pfx_test_asset.pfx_emitter";
    FILE* f = std::fopen(path, "wb");
    assert(f != nullptr);
    const char* text = R"({"name":"file_asset","capacity":64,"stages":{
        "spawn":[{"module":"SpawnRate","rate":10}]}})";
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);

    emitter_program prog;
    std::string err;
    assert(load_emitter_asset(path, prog, err));
    assert(prog.name == "file_asset");

    err.clear();
    assert(!load_emitter_asset("/tmp/pfx_nonexistent.pfx_emitter", prog, err));
    assert(err.find("cannot open") != std::string::npos);
    std::remove(path);

    std::cout << "  PASS: Asset file load" << std::endl;
}

inline void test_convert_legacy_presets() {
    std::cout << "Testing legacy preset conversion (pfx.convert)..." << std::endl;

    // 幂等补齐旧预设系统（8 个内置预设）
    assert(particles::initialize());
    if (!particles::preset_system::is_initialized()) {
        assert(particles::preset_system::initialize());
        particles::default_presets::register_defaults(*particles::preset_system::get());
    }
    const u32 legacy_count = (u32)particles::preset_system::get()->get_all_preset_names().size();
    assert(legacy_count == 8);   // 门禁：内置 8 预设

    std::system("mkdir -p /tmp/pfx_converted && rm -f /tmp/pfx_converted/*.pfx_emitter");
    std::string err;
    const u32 n = pfx::convert::convert_all_builtins("/tmp/pfx_converted", err);
    assert(n == 8);              // 全部转换成功
    assert(err.empty());

    // 往返：每个生成的资产都能加载并编译
    for (const std::string& name : particles::preset_system::get()->get_all_preset_names()) {
        emitter_program prog;
        std::string load_err;
        const std::string path = "/tmp/pfx_converted/" + name + ".pfx_emitter";
        assert(load_emitter_asset(path.c_str(), prog, load_err));
        assert(prog.stages[0].size() >= 1);   // 至少一个 spawn 模块
    }

    std::cout << "  PASS: Legacy preset conversion (8 presets, round-trip compile)" << std::endl;
}

inline void run_all_n1_tests() {
    test_json_parse_ok();
    test_json_parse_errors();
    test_compile_minimal();
    test_compile_error_paths();
    test_program_spawn_rate();
    test_program_spawn_burst();
    test_program_init_modules();
    test_program_update_forces();
    test_program_determinism();
    test_exposed_params_and_reload();
    test_schema_json();
    test_asset_file_load();
    test_convert_legacy_presets();
    std::cout << "All ParticleFX N1 tests passed." << std::endl;
}

} // namespace primal::pfx::test

#endif // !DISABLE_PARTICLE_FX
