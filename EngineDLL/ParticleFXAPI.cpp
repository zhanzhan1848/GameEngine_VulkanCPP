// ParticleFXAPI.cpp — PFX（可编程粒子框架）的 C ABI 门面，N1 第一批导出。
//
// 架构文档：Docs/2026-09-12-particle-framework-architecture.md §6
// 纪律：导出只增不改；资产式接口（编辑器传资产路径/整文档，不做结构性编辑）。
// 事件读取暂不导出（消费者同进程；需要时追加只读导出）。

#include "Common.h"
#include "CommonHeaders.h"
#include "Common/CoreLogStream.h"
#include "ParticleFX/core/PfxSystem.h"
#include "ParticleFX/interop/LegacyPresetConvert.h"

#include <cmath>
#include <cstring>
#include <string>

namespace pfx = primal::pfx;
namespace math = primal::math;

extern "C" {

// 幂等补齐 PFX 系统。返回 1 = 就绪。
EDITOR_INTERFACE u32 PfxSystemEnsure(void)
{
    return pfx::initialize({}) ? 1u : 0u;
}

// 从 .pfx_emitter JSON 资产创建发射器。返回发射器 id（0 = 失败；id 从 1 起）。
EDITOR_INTERFACE u32 PfxEmitterCreateFromAsset(const char* json_path)
{
    if (!json_path) return 0;
    return pfx::emitter_create_from_asset(json_path, nullptr);
}

// 移除发射器。未知 id 返回 0；成功 1。
EDITOR_INTERFACE u32 PfxEmitterRemove(u32 emitter_id)
{
    if (pfx::emitter_get(emitter_id) == nullptr) return 0;
    pfx::emitter_destroy(emitter_id);
    return 1u;
}

// 编辑期热预览：整文档推送重编译，同 id 重启发射器。失败返回 0（原状保留）。
EDITOR_INTERFACE u32 PfxEmitterReloadFromMemory(u32 emitter_id, const char* json_text, u32 length)
{
    if (!json_text || length == 0) return 0;
    return pfx::emitter_reload_from_memory(emitter_id, json_text, length, nullptr) ? 1u : 0u;
}

// 设置暴露参数（name_hash 用 PfxGetEmitterSchema 中名字的 Murmur3-32）。
EDITOR_INTERFACE u32 PfxSetExposedParam(u32 emitter_id, u32 name_hash, f32 value)
{
    return pfx::emitter_set_exposed_param(emitter_id, name_hash, value) ? 1u : 0u;
}

// 发射器统计（debug HUD 与 CLI 共用）。未知 id 返回 0。
EDITOR_INTERFACE u32 PfxGetEmitterStats(u32 emitter_id, u32* out_active, u32* out_capacity)
{
    const pfx::pfx_emitter* em = pfx::emitter_get(emitter_id);
    if (!em) return 0;
    if (out_active) *out_active = em->alive_count();
    if (out_capacity) *out_capacity = em->capacity();
    return 1u;
}

// schema 反射：模块清单/参数提示/标准通道（编辑器表单与 CLI 校验的单一事实源）。
// 惯例同 EmitterGetConfig：out 可为 null 查长度；cap 不足时返回 0 并填 out_len。
EDITOR_INTERFACE u32 PfxGetEmitterSchema(char* out, u32 cap, u32* out_len)
{
    const std::string schema = pfx::emitter_schema_json();
    if (out_len) *out_len = (u32)schema.size();
    if (!out) return 1u;   // 仅查长度
    if (cap < schema.size()) return 0;
    std::memcpy(out, schema.data(), schema.size());
    return 1u;
}

// pfx.convert：全部旧内置预设 → out_dir 下 <name>.pfx_emitter。返回成功数。
EDITOR_INTERFACE u32 PfxConvertLegacyPresets(const char* out_dir)
{
    if (!out_dir) return 0;
    std::string err;
    const u32 n = pfx::convert::convert_all_builtins(out_dir, err);
    if (!err.empty()) {
        PRIMAL_LOG_STREAM(Warn, "ParticleFX") << "convert_all_builtins: " << err << std::endl;
    }
    return n;
}

// pfx.debug dump（架构文档 §7-N2）：离线驱动发射器 N 帧，输出 JSON 统计
// （活粒数、位置包围盒、速率统计、归一化寿命直方图、位置指纹）。
// stdout JSON 契约由 primal-engine pfx-debug verb 消费。返回 1 = 成功。
EDITOR_INTERFACE u32 PfxDebugDump(const char* asset_path, u32 frames, f32 dt,
                                  char* out, u32 cap, u32* out_len)
{
    if (!asset_path || frames == 0 || dt <= 0.f) return 0;
    if (!pfx::initialize({})) return 0;

    pfx::emitter_program prog;
    std::string load_err;
    if (!pfx::load_emitter_asset(asset_path, prog, load_err)) {
        if (out_len) *out_len = 0;
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "pfx-debug: " << load_err << std::endl;
        return 0;
    }
    const pfx::emitter_id id = pfx::emitter_create_from_program(prog);
    if (id == pfx::invalid_emitter) return 0;
    pfx::pfx_emitter* em = pfx::emitter_get(id);
    if (!em) return 0;

    // 离线驱动
    for (u32 f = 0; f < frames; ++f) em->tick(dt);

    // 统计
    const u32 capacity = em->store().capacity();
    const math::v4* pos = em->store().v4_channel(pfx::attr::position);
    const math::v4* vel = em->store().v4_channel(pfx::attr::velocity);
    f32 pmin[3] = { 1e30f, 1e30f, 1e30f }, pmax[3] = { -1e30f, -1e30f, -1e30f };
    f32 smin = 1e30f, smax = -1e30f, ssum = 0.f;
    u32 alive = 0;
    u32 hist[10] = {};
    u64 fingerprint = 1469598103934665603ull;   // FNV-1a 64 基值
    for (u32 row = 0; row < capacity; ++row) {
        if (!em->store().is_row_alive(row)) continue;
        ++alive;
        const math::v4& p = pos[row];
        const f32 comps[3] = { p.x, p.y, p.z };
        for (int a = 0; a < 3; ++a) {
            if (comps[a] < pmin[a]) pmin[a] = comps[a];
            if (comps[a] > pmax[a]) pmax[a] = comps[a];
            fingerprint = (fingerprint ^ (u32)(comps[a] * 4096.f)) * 1099511628211ull;
        }
        const math::v4& v4 = vel[row];
        const f32 speed = std::sqrt(v4.x * v4.x + v4.y * v4.y + v4.z * v4.z);
        if (speed < smin) smin = speed;
        if (speed > smax) smax = speed;
        ssum += speed;
        const f32 norm = v4.w > 0.f ? p.w / v4.w : 0.f;
        u32 bin = (u32)(norm * 10.f);
        if (bin > 9) bin = 9;
        ++hist[bin];
    }

    std::string json = "{\"asset\":\"" + std::string(asset_path) + "\",";
    json += "\"frames\":" + std::to_string(frames) + ",";
    json += "\"dt\":" + std::to_string(dt) + ",";
    json += "\"emitter_id\":" + std::to_string(id) + ",";
    json += "\"alive\":" + std::to_string(alive) + ",";
    if (alive) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "\"position\":{\"min\":[%.4g,%.4g,%.4g],\"max\":[%.4g,%.4g,%.4g]},",
                      pmin[0], pmin[1], pmin[2], pmax[0], pmax[1], pmax[2]);
        json += buf;
        std::snprintf(buf, sizeof(buf),
                      "\"speed\":{\"min\":%.4g,\"max\":%.4g,\"avg\":%.4g},",
                      smin, smax, ssum / (f32)alive);
        json += buf;
        json += "\"age_histogram\":[";
        for (int i = 0; i < 10; ++i) json += std::to_string(hist[i]) + (i < 9 ? "," : "");
        json += "],";
        std::snprintf(buf, sizeof(buf), "\"position_fingerprint\":%llu}", (unsigned long long)fingerprint);
        json += buf;
    } else {
        json += "\"note\":\"no alive particles at dump time\"}";
    }

    if (out_len) *out_len = (u32)json.size();
    if (out && cap >= json.size()) {
        std::memcpy(out, json.data(), json.size());
    } else if (out) {
        return 0;   // cap 不足
    }

    pfx::emitter_destroy(id);
    return 1u;
}

} // extern "C"
