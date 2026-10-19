// Lightmap C ABI 实现（2026-10-19 lightmap 计划，M4）。契约见 LightmapAPI.h。
// 零渲染帧依赖：LightmapBake 收集当前渲染场景（帧外同步）；页面加载经
// 管线接口（下一帧生效）。

#include "LightmapAPI.h"

#include "Graphics/RenderPipeline/StandardRenderPipeline.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace primal::graphics {
namespace {
// ShaderCompilerAPI.cpp 同款访问器（RenderPipeline::Get() 单例；渲染帧外
// 调用安全）。
StandardRenderPipeline* GetPipeline() {
    auto* p = RenderPipeline::Get();
    return p ? static_cast<StandardRenderPipeline*>(p) : nullptr;
}

void AppendEscaped(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
}

} // namespace
} // namespace primal::graphics

using namespace primal::graphics;

EDITOR_INTERFACE unsigned int LightmapBake(const struct LightmapBakeSettingsC* settings,
                                           LightmapProgressCallbackC progress,
                                           void* progress_user,
                                           char* out_status_json,
                                           unsigned int status_cap) {
    auto fail = [&](const char* why) {
        if (out_status_json && status_cap)
            std::snprintf(out_status_json, status_cap, "{\"ok\":false,\"error\":\"%s\"}", why);
        return 0u;
    };
    StandardRenderPipeline* p = GetPipeline();
    if (!p) return fail("no pipeline");
    if (!settings) return fail("null settings");
    if (!settings->output_dir[0]) return fail("empty output_dir");

    // C 镜像 → 引擎结构（布局同构，逐字段拷贝防未来漂移）。
    baking::LightmapBakeSettings s{};
    s.atlas_size = settings->atlas_size;
    s.samples_per_texel = settings->samples_per_texel;
    s.bounces = settings->bounces;
    s.texel_density = settings->texel_density;
    s.denoise = settings->denoise;
    s.dilation_px = settings->dilation_px;
    std::memcpy(s.output_dir, settings->output_dir, sizeof(s.output_dir));

    std::string path, err;
    std::vector<baking::LightmapMeshEntry> entries;
    const bool ok = p->BakeSceneLightmaps(s, &path, &err, &entries,
                                          reinterpret_cast<baking::LightmapProgressCallback>(progress),
                                          progress_user);
    if (!ok) {
        std::string safeErr;
        AppendEscaped(safeErr, err);
        if (out_status_json && status_cap)
            std::snprintf(out_status_json, status_cap,
                          "{\"ok\":false,\"error\":\"%s\"}", safeErr.c_str());
        return 0u;
    }
    // 成功：路径 + per-mesh 清单（编辑器据此持久化 LightmapBaked 组件）。
    std::string json = "{\"ok\":true,\"path\":\"";
    AppendEscaped(json, path);
    json += "\",\"entries\":[";
    char item[512];
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        std::snprintf(item, sizeof(item),
                      "%s{\"name\":\"%s\",\"atlas_index\":%u,"
                      "\"scale_bias\":[%.9g,%.9g,%.9g,%.9g]}",
                      i ? "," : "", e.GetName().c_str(), e.atlas_index,
                      e.scale_x, e.scale_y, e.bias_x, e.bias_y);
        json += item;
    }
    json += "]}";
    if (out_status_json && status_cap) {
        std::snprintf(out_status_json, status_cap, "%s", json.c_str());
    }
    return 1u;
}

EDITOR_INTERFACE unsigned int PipelineSetLightmapPage(const char* path) {
    StandardRenderPipeline* p = GetPipeline();
    if (!p || !path || !path[0]) return 0u;
    return p->SetLightmapPage(path) ? 1u : 0u;
}

EDITOR_INTERFACE void PipelineClearLightmap() {
    StandardRenderPipeline* p = GetPipeline();
    if (p) p->ClearLightmapPage();
}
