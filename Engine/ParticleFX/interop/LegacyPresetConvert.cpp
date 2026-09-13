// LegacyPresetConvert.cpp — 见 LegacyPresetConvert.h。

#include "LegacyPresetConvert.h"
#include "../core/EmitterProgram.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>
#include <cstdio>

namespace primal::pfx::convert {

namespace {

void append_param_ref(std::string& s, f32 v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
    s += buf;
}

void append_v2(std::string& s, f32 x, f32 y) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[%.6g,%.6g]", (double)x, (double)y);
    s += buf;
}

void append_v3(std::string& s, const math::v3& v) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "[%.6g,%.6g,%.6g]", (double)v.x, (double)v.y, (double)v.z);
    s += buf;
}

void append_v4(std::string& s, const math::v4& v) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "[%.6g,%.6g,%.6g,%.6g]", (double)v.x, (double)v.y, (double)v.z,
                  (double)v.w);
    s += buf;
}

const char* blend_name(particles::blend_mode m) {
    switch (m) {
        case particles::blend_mode::alpha: return "alpha";
        case particles::blend_mode::multiply: return "multiply";
        case particles::blend_mode::premultiplied: return "premultiplied";
        case particles::blend_mode::additive:
        default: return "additive";
    }
}

} // namespace

bool convert_preset_to_json(const particles::particle_preset& preset, std::string& out_json, std::string& err) {
    const particles::emitter_config& cfg = preset.config;
    if (preset.name.empty()) {
        err = "preset name empty";
        return false;
    }

    std::string s;
    s += "{\n";
    s += "  \"name\": \"" + preset.name + "\",\n";
    s += "  \"blend\": \"" + std::string(blend_name(cfg.blending)) + "\",\n";
    s += "  \"capacity\": 10000,\n";
    s += "  \"seed\": 1,\n";

    // 速度矩形随机 → 方向 + 速率范围（有损近似，原值保留在 legacy_render）
    const math::v3 vel_mid{ (cfg.velocity_min.x + cfg.velocity_max.x) * 0.5f,
                            (cfg.velocity_min.y + cfg.velocity_max.y) * 0.5f,
                            (cfg.velocity_min.z + cfg.velocity_max.z) * 0.5f };
    f32 dir_len = std::sqrt(vel_mid.x * vel_mid.x + vel_mid.y * vel_mid.y + vel_mid.z * vel_mid.z);
    math::v3 dir{ 0.f, 1.f, 0.f };
    if (dir_len > 1e-6f) dir = math::v3{ vel_mid.x / dir_len, vel_mid.y / dir_len, vel_mid.z / dir_len };
    const f32 sp_min = std::sqrt(cfg.velocity_min.x * cfg.velocity_min.x + cfg.velocity_min.y * cfg.velocity_min.y +
                                 cfg.velocity_min.z * cfg.velocity_min.z);
    const f32 sp_max = std::sqrt(cfg.velocity_max.x * cfg.velocity_max.x + cfg.velocity_max.y * cfg.velocity_max.y +
                                 cfg.velocity_max.z * cfg.velocity_max.z);

    s += "  \"stages\": {\n";
    s += "    \"spawn\": [\n";
    s += "      { \"module\": \"SpawnRate\",\n        \"rate\": ";
    append_param_ref(s, cfg.spawn_rate);
    s += " }";
    if (cfg.mode == particles::emission_mode::burst && cfg.burst_count > 0) {
        s += ",\n      { \"module\": \"SpawnBurst\", \"count\": " + std::to_string(cfg.burst_count) +
             ", \"period\": 1.0 }";
    }
    s += "\n    ],\n";

    s += "    \"init\": [\n";
    s += "      { \"module\": \"ShapeLocation\",\n        \"shape\": \"point\" },\n";
    s += "      { \"module\": \"AddVelocity\",\n        \"mode\": \"fixed\",\n        \"direction\": ";
    append_v3(s, dir);
    s += ",\n        \"speed_min\": ";
    append_param_ref(s, sp_min);
    s += ",\n        \"speed_max\": ";
    append_param_ref(s, sp_max);
    s += ",\n        \"inherit_source_velocity\": 0 },\n";
    s += "      { \"module\": \"SetLifetime\",\n        \"min\": ";
    append_param_ref(s, cfg.lifetime_min);
    s += ",\n        \"max\": ";
    append_param_ref(s, cfg.lifetime_max);
    s += " },\n";
    s += "      { \"module\": \"SetColor\",\n        \"color\": ";
    append_v4(s, cfg.color_start);
    s += ",\n        \"color_end\": ";
    append_v4(s, cfg.color_end);
    s += " },\n";
    s += "      { \"module\": \"SetScale\",\n        \"scale\": ";
    append_v2(s, cfg.scale_min.x, cfg.scale_min.y);
    s += ",\n        \"scale_end\": ";
    append_v2(s, cfg.scale_max.x, cfg.scale_max.y);
    s += " }\n";
    s += "    ],\n";

    s += "    \"update\": [\n";
    s += "      { \"module\": \"GravityForce\",\n        \"gravity\": ";
    append_v3(s, cfg.gravity);
    s += " }";
    std::string update_extra;   // AnimateUv 等运行时推进模块的 JSON 片段
    if (cfg.drag > 0.f) {
        s += ",\n      { \"module\": \"DragForce\",\n        \"coefficient\": ";
        append_param_ref(s, cfg.drag);
        s += " }";
    }

    if (cfg.texture_frame_rate > 0.f) {
        char uvbuf[128];
        std::snprintf(uvbuf, sizeof(uvbuf),
            ",\n      { \"module\": \"AnimateUv\", \"frame_rate\": %g, \"frames\": %u }",
            (double)cfg.texture_frame_rate, cfg.texture_frames_x * cfg.texture_frames_y);
        // 追加到 update（在 force 之后，见下方 overlife 拼接前统一处理）
        update_extra = uvbuf;
    }

    // 生命期曲线（N6 收口：legacy 曲线 → PFX over-life 模块）
    auto emit_float_curve = [](const particles::particle_preset::curve_data& cd, std::string& out) {
        for (size_t k = 0; k < cd.keyframes.size(); ++k) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "[%g,%g]%s", (double)cd.keyframes[k].first,
                          (double)cd.keyframes[k].second,
                          k + 1 < cd.keyframes.size() ? "," : "");
            out += buf;
        }
    };
    std::string overlife;
    if (preset.use_scale_curve && !preset.scale_curve.keyframes.empty()) {
        overlife += ", { \"module\": \"ScaleOverLife\", \"curve\": [";
        emit_float_curve(preset.scale_curve, overlife);
        overlife += "] }";
    }
    if (preset.use_alpha_curve && !preset.alpha_curve.keyframes.empty()) {
        overlife += ", { \"module\": \"AlphaOverLife\", \"curve\": [";
        emit_float_curve(preset.alpha_curve, overlife);
        overlife += "] }";
    }
    if (preset.use_rotation_curve && !preset.rotation_curve.keyframes.empty()) {
        overlife += ", { \"module\": \"RotationOverLife\", \"curve\": [";
        emit_float_curve(preset.rotation_curve, overlife);
        overlife += "] }";
    }
    if (preset.use_force_curve && !preset.force_curve.keys.empty()) {
        overlife += ", { \"module\": \"ForceOverLife\", \"curve\": [";
        for (size_t k = 0; k < preset.force_curve.keys.size(); ++k) {
            char buf[160];
            const auto& kv = preset.force_curve.keys[k];
            std::snprintf(buf, sizeof(buf), "[%g,%g,%g,%g]%s", (double)kv.first,
                          (double)kv.second.x, (double)kv.second.y, (double)kv.second.z,
                          k + 1 < preset.force_curve.keys.size() ? "," : "");
            overlife += buf;
        }
        overlife += "] }";
    }
    s += overlife;
    s += update_extra;
    s += "\n    ]\n";
    s += "  },\n";

    // 旧渲染/曲线参数的无损保留（N2 渲染器与曲线 DI 落地后升级为正式表达）
    s += "  \"legacy_render\": {\n";
    s += "    \"blend\": \"" + std::string(blend_name(cfg.blending)) + "\",\n";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "    \"texture_frames\": [%u,%u,%.6g],\n", cfg.texture_frames_x,
                  cfg.texture_frames_y, (double)cfg.texture_frame_rate);
    s += buf;
    std::snprintf(buf, sizeof(buf), "    \"atlas\": [%u,%u,%u,%u],\n", cfg.texture_atlas_columns,
                  cfg.texture_atlas_rows, cfg.texture_first_frame, cfg.texture_frame_count);
    s += buf;
    std::snprintf(buf, sizeof(buf), "    \"scale_multiplier\": %.6g,\n    \"depth_write\": %s,\n"
                                    "    \"receive_shadows\": %s,\n    \"emit_in_local_space\": %s\n",
                  (double)cfg.scale_multiplier, cfg.depth_write ? "true" : "false",
                  cfg.receive_shadows ? "true" : "false", cfg.emit_in_local_space ? "true" : "false");
    s += buf;
    s += "  },\n";

    s += "  \"legacy_curves\": {\n";
    s += "    \"scale\": " + std::to_string(preset.use_scale_curve) + ",\n";
    s += "    \"alpha\": " + std::to_string(preset.use_alpha_curve) + ",\n";
    s += "    \"color_gradient\": " + std::to_string(preset.use_color_gradient) + ",\n";
    s += "    \"velocity\": " + std::to_string(preset.use_velocity_curve) + ",\n";
    s += "    \"force\": " + std::to_string(preset.use_force_curve) + ",\n";
    s += "    \"rotation\": " + std::to_string(preset.use_rotation_curve) + "\n";
    s += "  }\n";
    s += "}";

    out_json = std::move(s);

    // 自校验：转换结果必须能被编译器接受
    emitter_program prog;
    std::string compile_err;
    if (!compile_emitter_from_text(out_json.data(), out_json.size(), prog, compile_err)) {
        err = "converted preset '" + preset.name + "' failed to compile: " + compile_err;
        return false;
    }
    return true;
}

u32 convert_all_builtins(const char* out_dir, std::string& err) {
    if (!particles::preset_system::is_initialized()) {
        if (!particles::preset_system::initialize()) {
            err = "legacy preset system init failed";
            return 0;
        }
        particles::default_presets::register_defaults(*particles::preset_system::get());
    }
    particles::preset_manager& mgr = *particles::preset_system::get();
    const utl::vector<std::string> names = mgr.get_all_preset_names();

    u32 ok_count = 0;
    for (const std::string& name : names) {
        const particles::particle_preset* preset = mgr.get_preset(name);
        if (!preset) continue;
        std::string json_text;
        std::string local_err;
        if (!convert_preset_to_json(*preset, json_text, local_err)) {
            PRIMAL_LOG_STREAM(Warn, "ParticleFX")
                << "convert '" << name << "' failed: " << local_err << std::endl;
            if (err.empty()) err = local_err;
            continue;
        }
        const std::string path = std::string(out_dir) + "/" + name + ".pfx_emitter";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            if (err.empty()) err = "cannot write '" + path + "'";
            continue;
        }
        std::fwrite(json_text.data(), 1, json_text.size(), f);
        std::fclose(f);
        ++ok_count;
    }
    return ok_count;
}

} // namespace primal::pfx::convert

#endif // !DISABLE_PARTICLE_FX
