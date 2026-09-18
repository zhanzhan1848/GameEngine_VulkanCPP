#include "GIEditorPanel.h"
// include 顺序敏感：StandardRenderPipeline.h（及其 utl::vector 消费链）必须在
// StaticProbeBaker.h 之前 —— 后者经 BVH.hpp 打开 primal::graphics::utl，会遮蔽
// 后续头文件里 primal::utl 的名字查找（RenderSceneSnapshot/RenderGraph 同理）。
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/Lumen/DDGIFitGrid.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeBaker.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace primal::graphics {
namespace gi_panel {

// ============================================================================
// 内部工具
// ============================================================================

namespace {

// 面板显示用短状态回写（截断保安全）
void ReportStatus(char* status, u32 cap, const char* fmt, ...) {
    if (!status || cap == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status, cap, fmt, args);
    va_end(args);
}

// copy-modify-commit：返回 settings 副本，宿主改完经 CommitSettings 写回。
// 提交 = UpdateSettings（置 settings_dirty_ → 下一渲染帧 ApplyConfigChanges
// 自动 Shutdown/Initialize Lumen passes —— volume counts/spacing 变更所需的
// 重初始化由渲染循环自带机制完成，面板只标记）。
bool CommitSettings(StandardRenderPipeline* pipeline,
                    const RenderPipelineSettings& s) {
    if (!pipeline) return false;
    pipeline->UpdateSettings(s);
    return true;
}

// P1-A（Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md §5.6/§8）：fit 启用
// 时把 near 体积的编辑/显示口径同步为拟合后的实际网格——优先从 pass 回读
// （运行中的真实 counts/spacing/min-corner origin），pass 未建时按 config
// 推导兜底。origin 走 LumenVolumeConfig::origin_override（gizmo 显示契约），
// 不与 pass 侧的 runtime params 混用。
void ApplyFitToNearVolume(StandardRenderPipeline* pipeline,
                          const lumen::LumenConfig& lc,
                          lumen::LumenVolumeConfig& near_v) {
    if (!lc.ddgi_fit_to_bounds) return;
    if (lumen::LumenDDGIPass* ddgi = pipeline ? pipeline->GetDDGIPass() : nullptr) {
        const lumen::DDGIRuntimeParams& p = ddgi->GetParams();
        near_v.probe_count_x = p.probe_count_x;
        near_v.probe_count_y = p.probe_count_y;
        near_v.probe_count_z = p.probe_count_z;
        near_v.probe_spacing = p.probe_spacing;
        near_v.rays_per_probe = p.rays_per_probe;
        near_v.ray_max_distance = p.ray_max_distance;
        if (p.use_probe_origin) {
            near_v.origin_override = p.probe_origin;
            near_v.use_origin_override = true;
        }
        return;
    }
    const lumen::DDGIFitGrid g = lumen::DDGIFitGridFromBounds(
        lc.ddgi_fit_bounds_min, lc.ddgi_fit_bounds_max,
        lc.ddgi_probe_spacing, lc.ddgi_fit_margin);
    near_v.probe_count_x = g.count_x;
    near_v.probe_count_y = g.count_y;
    near_v.probe_count_z = g.count_z;
    near_v.origin_override = g.origin;
    near_v.use_origin_override = true;
}

// clamp 到描述符表声明的合法范围（与 GetVolumeParamDescriptors 一致）
lumen::LumenVolumeConfig ClampVolume(const lumen::LumenVolumeConfig& v) {
    lumen::LumenVolumeConfig c = v;
    c.probe_count_x  = std::clamp(c.probe_count_x, 4u, 64u);
    c.probe_count_y  = std::clamp(c.probe_count_y, 4u, 64u);
    c.probe_count_z  = std::clamp(c.probe_count_z, 4u, 64u);
    c.rays_per_probe = std::clamp(c.rays_per_probe, 8u, 256u);
    c.update_budget_divisor = std::clamp(c.update_budget_divisor, 1u, 16u);
    c.probe_spacing      = std::clamp(c.probe_spacing, 0.5f, 20.0f);
    c.ray_max_distance   = std::clamp(c.ray_max_distance, 5.0f, 200.0f);
    return c;
}

} // anonymous namespace

// ============================================================================
// 描述符表 — GIDebugParams（运行时热调面，offset 相对 GIDebugParams）
// ============================================================================

#define GOFF(field) static_cast<u32>(offsetof(lumen::GIDebugParams, field))
#define GSZ(field)  static_cast<u32>(sizeof(((lumen::GIDebugParams*)0)->field))

static const ParamDescriptor kGIDebugParams[] = {
    // --- 逐 pass 开关（写 GIDebug() 单例即时生效：AddPass 时读取） ---
    {"DDGI Trace",        "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(ddgi_trace),      GSZ(ddgi_trace),      nullptr},
    {"DDGI Far Volume",   "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(ddgi_far_volume), GSZ(ddgi_far_volume), nullptr},
    {"SC Capture",        "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(sc_capture),      GSZ(sc_capture),      nullptr},
    {"SC Lighting",       "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(sc_lighting),     GSZ(sc_lighting),     nullptr},
    {"SC Shadow",         "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(sc_shadow),       GSZ(sc_shadow),       nullptr},
    {"GI Gather",         "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(gi_gather),       GSZ(gi_gather),       nullptr},
    {"Fusion",            "GI Debug: Passes", ParamType::Bool,  {0, 1, 1},        GOFF(fusion),          GSZ(fusion),          nullptr},

    // --- 权重热调（CPU 侧即时生效） ---
    {"Sky Color Intensity",    "GI Debug: Weights", ParamType::Float, {0.0f, 4.0f, 0.01f},  GOFF(sky_color_intensity),     GSZ(sky_color_intensity),     nullptr},
    {"Albedo Intensity",       "GI Debug: Weights", ParamType::Float, {0.0f, 4.0f, 0.01f},  GOFF(albedo_intensity),        GSZ(albedo_intensity),        nullptr},
    {"Probe Hysteresis",       "GI Debug: Weights", ParamType::Float, {0.0f, 1.0f, 0.01f},  GOFF(probe_hysteresis),        GSZ(probe_hysteresis),        nullptr},
    {"Fusion Glossy Strength", "GI Debug: Weights", ParamType::Float, {0.0f, 2.0f, 0.01f},  GOFF(fusion_glossy_strength), GSZ(fusion_glossy_strength), nullptr},

    // --- freeze frame（A/B 对比） ---
    {"Freeze Probe Updates", "GI Debug: Freeze", ParamType::Bool, {0, 1, 1},       GOFF(freeze_probe_updates), GSZ(freeze_probe_updates), nullptr},

    // --- probe 调试可视化（T6.3 sub-A 已接线 Vulkan gather kernel 数据面：
    //     Off 默认零开销/零输出变化，见 GIDebugParams.h probe_vis 注释） ---
    {"Probe Visualization", "GI Debug: Vis", ParamType::Enum, {0, 3, 1},           GOFF(probe_vis),            GSZ(probe_vis),            "Off,IrradianceAtlas,DepthAtlas,Confidence"},
};

#undef GOFF
#undef GSZ

const ParamDescriptor* GetDebugParamDescriptors(u32& out_count) {
    out_count = sizeof(kGIDebugParams) / sizeof(kGIDebugParams[0]);
    return kGIDebugParams;
}

// ============================================================================
// 描述符表 — LumenVolumeConfig（volume 列表条目字段，offset 相对
// LumenVolumeConfig；宿主按 volume 条目 × 字段展开控件）
// ============================================================================

#define VOFF(field) static_cast<u32>(offsetof(lumen::LumenVolumeConfig, field))
#define VSZ(field)  static_cast<u32>(sizeof(((lumen::LumenVolumeConfig*)0)->field))

static const ParamDescriptor kGIVolumeParams[] = {
    {"Probe Count X",         "GI Volume", ParamType::UInt,  {4, 64, 1},           VOFF(probe_count_x),         VSZ(probe_count_x),         nullptr},
    {"Probe Count Y",         "GI Volume", ParamType::UInt,  {4, 64, 1},           VOFF(probe_count_y),         VSZ(probe_count_y),         nullptr},
    {"Probe Count Z",         "GI Volume", ParamType::UInt,  {4, 64, 1},           VOFF(probe_count_z),         VSZ(probe_count_z),         nullptr},
    {"Probe Spacing",         "GI Volume", ParamType::Float, {0.5f, 20.0f, 0.5f},  VOFF(probe_spacing),         VSZ(probe_spacing),         nullptr},
    {"Rays Per Probe",        "GI Volume", ParamType::UInt,  {8, 256, 8},          VOFF(rays_per_probe),        VSZ(rays_per_probe),        nullptr},
    {"Ray Max Distance",      "GI Volume", ParamType::Float, {5, 200, 5},          VOFF(ray_max_distance),      VSZ(ray_max_distance),      nullptr},
    {"Update Budget Divisor", "GI Volume", ParamType::UInt,  {1, 16, 1},           VOFF(update_budget_divisor), VSZ(update_budget_divisor), nullptr},
    {"Origin Override",       "GI Volume", ParamType::Vec3,  {-10000, 10000, 10},  VOFF(origin_override),       VSZ(origin_override),       nullptr},
    {"Use Origin Override",   "GI Volume", ParamType::Bool,  {0, 1, 1},            VOFF(use_origin_override),   VSZ(use_origin_override),   nullptr},
};

#undef VOFF
#undef VSZ

const ParamDescriptor* GetVolumeParamDescriptors(u32& out_count) {
    out_count = sizeof(kGIVolumeParams) / sizeof(kGIVolumeParams[0]);
    return kGIVolumeParams;
}

// ============================================================================
// volume 列表编辑（cap 4）
// ============================================================================

u32 GetVolumeOverrideCount(StandardRenderPipeline* pipeline) {
    if (!pipeline) return 0;
    return static_cast<u32>(pipeline->GetSettings().lumen.volume_overrides.size());
}

bool GetVolumeOverride(StandardRenderPipeline* pipeline, u32 index,
                       lumen::LumenVolumeConfig* out) {
    if (!pipeline || !out) return false;
    const auto& vols = pipeline->GetSettings().lumen.volume_overrides;
    if (index >= vols.size()) return false;
    *out = vols[index];
    return true;
}

bool SetVolumeOverride(StandardRenderPipeline* pipeline, u32 index,
                       const lumen::LumenVolumeConfig& v) {
    if (!pipeline) return false;
    RenderPipelineSettings s = pipeline->GetSettings();   // copy
    if (index >= s.lumen.volume_overrides.size()) return false;
    s.lumen.volume_overrides[index] = ClampVolume(v);
    // 提交即标记重初始化（下帧 ApplyConfigChanges 重建 Lumen passes）。
    return CommitSettings(pipeline, s);
}

bool AppendVolumeOverride(StandardRenderPipeline* pipeline) {
    if (!pipeline) return false;
    RenderPipelineSettings s = pipeline->GetSettings();
    if (s.lumen.volume_overrides.size() >= kMaxVolumes) return false;
    // 新条目从 near 默认起步（拷贝 LumenVolumeConfig 默认值）
    s.lumen.volume_overrides.push_back(lumen::LumenVolumeConfig{});
    return CommitSettings(pipeline, s);
}

bool RemoveVolumeOverride(StandardRenderPipeline* pipeline, u32 index) {
    if (!pipeline) return false;
    RenderPipelineSettings s = pipeline->GetSettings();
    if (index >= s.lumen.volume_overrides.size()) return false;
    s.lumen.volume_overrides.erase(s.lumen.volume_overrides.begin() + index);
    return CommitSettings(pipeline, s);
}

bool MaterializeDerivedVolumes(StandardRenderPipeline* pipeline) {
    if (!pipeline) return false;
    RenderPipelineSettings s = pipeline->GetSettings();
    if (!s.lumen.volume_overrides.empty()) return true;   // 已显式化
    const lumen::LumenConfig& lc = s.lumen;

    // 与 StandardRenderPipeline::InitializeLumenPasses 相同的推导：near 默认
    // + far（ddgi_enable_far_volume 时）。物化后宿主逐条编辑。
    std::vector<lumen::LumenVolumeConfig> vols;
    vols.reserve(2);
    lumen::LumenVolumeConfig near_v{};
    near_v.probe_count_x = lc.ddgi_probe_count_x;
    near_v.probe_count_y = lc.ddgi_probe_count_y;
    near_v.probe_count_z = lc.ddgi_probe_count_z;
    near_v.rays_per_probe = lc.ddgi_rays_per_probe;
    near_v.probe_spacing = lc.ddgi_probe_spacing;
    near_v.ray_max_distance = lc.ddgi_ray_max_distance;
    ApplyFitToNearVolume(pipeline, lc, near_v);   // P1-A：fit 时回读实际体积
    vols.push_back(near_v);
    if (lc.ddgi_enable_far_volume) {
        lumen::LumenVolumeConfig far_v{};
        far_v.probe_count_x = lc.ddgi_far_probe_count_x;
        far_v.probe_count_y = lc.ddgi_far_probe_count_y;
        far_v.probe_count_z = lc.ddgi_far_probe_count_z;
        far_v.rays_per_probe = lc.ddgi_rays_per_probe;
        far_v.probe_spacing = lc.ddgi_far_probe_spacing;
        far_v.ray_max_distance = lc.ddgi_far_ray_max_distance;
        far_v.update_budget_divisor = 4;
        vols.push_back(far_v);
    }
    s.lumen.volume_overrides = std::move(vols);
    return CommitSettings(pipeline, s);
}

bool GetEffectiveVolumeConfig(StandardRenderPipeline* pipeline, u32 index,
                              lumen::LumenVolumeConfig* out) {
    if (!pipeline || !out) return false;
    const lumen::LumenConfig& lc = pipeline->GetSettings().lumen;
    if (!lc.volume_overrides.empty()) {
        if (index >= lc.volume_overrides.size()) return false;
        *out = lc.volume_overrides[index];
        return true;
    }
    // 推导：0 = near；1 = far（仅 ddgi_enable_far_volume）；其余越界。
    if (index == 0) {
        lumen::LumenVolumeConfig near_v{};
        near_v.probe_count_x = lc.ddgi_probe_count_x;
        near_v.probe_count_y = lc.ddgi_probe_count_y;
        near_v.probe_count_z = lc.ddgi_probe_count_z;
        near_v.rays_per_probe = lc.ddgi_rays_per_probe;
        near_v.probe_spacing = lc.ddgi_probe_spacing;
        near_v.ray_max_distance = lc.ddgi_ray_max_distance;
        ApplyFitToNearVolume(pipeline, lc, near_v);   // P1-A：fit 时回读实际体积
        *out = near_v;
        return true;
    }
    if (index == 1 && lc.ddgi_enable_far_volume) {
        lumen::LumenVolumeConfig far_v{};
        far_v.probe_count_x = lc.ddgi_far_probe_count_x;
        far_v.probe_count_y = lc.ddgi_far_probe_count_y;
        far_v.probe_count_z = lc.ddgi_far_probe_count_z;
        far_v.rays_per_probe = lc.ddgi_rays_per_probe;
        far_v.probe_spacing = lc.ddgi_far_probe_spacing;
        far_v.ray_max_distance = lc.ddgi_far_ray_max_distance;
        far_v.update_budget_divisor = 4;
        *out = far_v;
        return true;
    }
    return false;
}

// ============================================================================
// 五轴热重载
// ============================================================================

bool ApplyAxesConfig(StandardRenderPipeline* pipeline,
                     const lumen::GIFiveAxisConfig& axes) {
    if (!pipeline) return false;
    lumen::LumenConfig lc = pipeline->GetSettings().lumen;
    lc.axes = axes;
    // SetLumenConfig：settings_.lumen = lc；quality 重置为 FromPreset 档位
    // 默认；ApplyGIAxis1_SpaceCaches 用 axes 覆写 pass 开关（轴视图单一
    // 真源）。已初始化管线不会在此重建 pass —— 下一步补 dirty。
    pipeline->SetLumenConfig(lc);
    // 触发已初始化管线的 Lumen pass 重建（轴1 开关组合变化可能需要创建/
    // 销毁 SurfaceCache / ScreenProbes 实例）。渲染循环首 ApplyConfigChanges
    // 消费此标记（Render/RenderWithCommandBuffer 均在帧首调用）。
    pipeline->UpdateSettings(pipeline->GetSettings());
    return true;
}

// ============================================================================
// Bake（grid 取当前生效 volume 0 配置）
// ============================================================================

bool BakeStaticProbes(StandardRenderPipeline* pipeline,
                      const lumen::ProbeBakingScene& scene,
                      const lumen::ProbeBakingParams& params,
                      char* status, u32 status_cap) {
    if (!pipeline) {
        ReportStatus(status, status_cap, "Bake failed: no pipeline");
        return false;
    }
    if (!scene.vertices || !scene.indices || scene.vertex_count == 0 ||
        scene.index_count == 0) {
        ReportStatus(status, status_cap,
                     "Bake failed: empty scene (CPU vertices/indices required)");
        return false;
    }

    // grid 参数取当前 volume 0 配置（overrides 优先，否则 ddgi_* 推导）
    lumen::LumenVolumeConfig v0{};
    if (!GetEffectiveVolumeConfig(pipeline, 0, &v0)) {
        ReportStatus(status, status_cap, "Bake failed: no volume 0 config");
        return false;
    }

    auto* vol = pipeline->GetStaticProbeVolume();
    if (!vol) {
        ReportStatus(status, status_cap, "Bake failed: static probe volume not created");
        return false;
    }

    // grid 不匹配则重建（Shutdown+Initialize 释放/重分配 CPU 侧数据与 GPU
    // buffer —— 必须在渲染帧之外调用，见头文件线程性说明）。
    // origin 比较用逐分量（simd::float3 无 bool 语义的 operator==）。
    const auto& cur = vol->GetParams();
    const math::v3 want_origin = v0.use_origin_override ? v0.origin_override
                                                        : math::v3{0.0f, 0.0f, 0.0f};
    const bool grid_mismatch =
        cur.grid_dim_x != v0.probe_count_x ||
        cur.grid_dim_y != v0.probe_count_y ||
        cur.grid_dim_z != v0.probe_count_z ||
        cur.spacing    != v0.probe_spacing ||
        cur.origin.x != want_origin.x || cur.origin.y != want_origin.y ||
        cur.origin.z != want_origin.z;
    if (grid_mismatch) {
        vol->Shutdown();
        lumen::StaticProbeParams sp{};
        sp.grid_dim_x = v0.probe_count_x;
        sp.grid_dim_y = v0.probe_count_y;
        sp.grid_dim_z = v0.probe_count_z;
        sp.spacing    = v0.probe_spacing;
        sp.origin     = want_origin;
        if (!vol->Initialize(pipeline->GetDevice(), sp)) {
            ReportStatus(status, status_cap, "Bake failed: volume re-init failed");
            return false;
        }
    }

    if (!lumen::StaticProbeBaker::Bake(*vol, scene, params)) {
        ReportStatus(status, status_cap, "Bake failed: StaticProbeBaker::Bake returned false");
        return false;
    }
    if (!vol->UploadToGPU()) {
        ReportStatus(status, status_cap, "Baked %u probes but GPU upload failed",
                     vol->ProbeCount());
        return false;
    }
    ReportStatus(status, status_cap, "Baked %u probes (%ux%ux%u @ %.2f)",
                 vol->ProbeCount(), v0.probe_count_x, v0.probe_count_y,
                 v0.probe_count_z, v0.probe_spacing);
    return true;
}

// ============================================================================
// 面板动作注册表（进程级；被动容器，默认空 = 零开销）
// ============================================================================

namespace {
std::vector<PanelAction>& ActionRegistry() {
    static std::vector<PanelAction> registry;
    return registry;
}
} // anonymous namespace

void RegisterAction(const PanelAction& action) {
    if (!action.id || !action.fn) return;
    auto& reg = ActionRegistry();
    for (auto& a : reg) {
        if (std::strcmp(a.id, action.id) == 0) {   // 同 id 覆盖
            a = action;
            return;
        }
    }
    reg.push_back(action);
}

void UnregisterAction(const char* id) {
    if (!id) return;
    auto& reg = ActionRegistry();
    for (size_t i = 0; i < reg.size(); ++i) {
        if (std::strcmp(reg[i].id, id) == 0) {
            reg.erase(reg.begin() + static_cast<long>(i));
            return;
        }
    }
}

void ClearActions() {
    ActionRegistry().clear();
}

u32 GetActionCount() {
    return static_cast<u32>(ActionRegistry().size());
}

const PanelAction* GetActionByIndex(u32 index) {
    const auto& reg = ActionRegistry();
    if (index >= reg.size()) return nullptr;
    return &reg[index];
}

const PanelAction* GetActionById(const char* id) {
    if (!id) return nullptr;
    for (const auto& a : ActionRegistry()) {
        if (std::strcmp(a.id, id) == 0) return &a;
    }
    return nullptr;
}

bool InvokeAction(const char* id) {
    const PanelAction* a = GetActionById(id);
    if (!a || !a->fn) return false;
    return a->fn(a->user_data);
}

} // namespace primal::graphics::gi_panel
} // namespace primal::graphics
