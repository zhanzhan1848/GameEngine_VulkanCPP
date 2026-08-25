#include "GIVolumeGizmo.h"
// include 顺序契约（先例：GIEditorPanel.cpp 顶部注释）：StaticProbeBaker.h 经
// BVH.hpp 打开 primal::graphics::utl 会遮蔽 primal::utl 的名字查找 —— 本 TU
// 不 include StaticProbeBaker.h；若未来需要，必须放在下面所有 Engine 头之后。
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/GIEditorPanel.h"   // gi_panel::RegisterAction

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace primal::graphics {
namespace gi_gizmo {

// ============================================================================
// 内部数学（跨平台：Apple simd 与便携结构体共用 .x/.y/.z + columns 访问）
// ============================================================================

namespace {

inline math::v4 TransformVec4(const math::m4x4& m, const math::v4& v) {
    // m.columns[c] = 第 c 列；result = sum_c m.columns[c] * v[c]
    return m.columns[0] * v.x + m.columns[1] * v.y +
           m.columns[2] * v.z + m.columns[3] * v.w;
}

inline f32 Dot3(const math::v3& a, const math::v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline math::v3 Cross3(const math::v3& a, const math::v3& b) {
    return math::v3{a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x};
}

inline math::v3 Normalize3(const math::v3& v) {
    const f32 len = std::sqrt(Dot3(v, v));
    if (len < 1e-8f) return math::v3{0.0f, 0.0f, 0.0f};
    return math::v3{v.x / len, v.y / len, v.z / len};
}

// 进程级 gizmo 状态（被动数据，无锁：编辑器主线程消费）
struct GizmoState {
    GizmoMode mode            = GizmoMode::Off;
    u32       selectedVolume  = kNoVolumeSelected;
    f32       refDistance     = 10.0f;
};

GizmoState& State() {
    static GizmoState s;
    return s;
}

} // anonymous namespace

// ============================================================================
// 1) 拾取
// ============================================================================

GizmoRay ScreenRay(f32 screenX, f32 screenY, u32 viewportW, u32 viewportH,
                   const math::m4x4& invViewProj, const math::v3& viewOrigin) {
    GizmoRay ray;
    ray.origin = viewOrigin;
    ray.dir    = math::v3{0.0f, 0.0f, 1.0f};   // 退化回退
    if (viewportW == 0 || viewportH == 0) return ray;

    // 屏幕（Y 向下）→ NDC（Y 向上）
    const f32 ndcX = (2.0f * screenX) / static_cast<f32>(viewportW) - 1.0f;
    const f32 ndcY = 1.0f - (2.0f * screenY) / static_cast<f32>(viewportH);

    // NDC 近/远点 → clip → 世界（透视除法）
    const math::v4 nearH = TransformVec4(invViewProj,
        math::v4{ndcX, ndcY, -1.0f, 1.0f});
    const math::v4 farH  = TransformVec4(invViewProj,
        math::v4{ndcX, ndcY,  1.0f, 1.0f});
    if (std::abs(nearH.w) < 1e-8f || std::abs(farH.w) < 1e-8f) return ray;

    const math::v3 nearP{nearH.x / nearH.w, nearH.y / nearH.w, nearH.z / nearH.w};
    const math::v3 farP {farH.x  / farH.w, farH.y  / farH.w, farH.z  / farH.w};
    const math::v3 dir   = Normalize3(farP - nearP);
    if (dir.x == 0.0f && dir.y == 0.0f && dir.z == 0.0f) return ray;
    ray.dir = dir;
    return ray;
}

void GetVolumeAABB(const lumen::LumenVolumeConfig& v,
                   math::v3& outMin, math::v3& outMax) {
    // (0,0,0) = world-centered（与 LumenVolumeConfig::origin_override 注释
    // 及 BakeStaticProbes 的 grid origin 推导一致）。
    outMin = v.use_origin_override ? v.origin_override
                                   : math::v3{0.0f, 0.0f, 0.0f};
    outMax = math::v3{
        outMin.x + static_cast<f32>(v.probe_count_x) * v.probe_spacing,
        outMin.y + static_cast<f32>(v.probe_count_y) * v.probe_spacing,
        outMin.z + static_cast<f32>(v.probe_count_z) * v.probe_spacing};
}

GIVolumeHit PickVolume(f32 screenX, f32 screenY, u32 viewportW, u32 viewportH,
                       const math::m4x4& invViewProj, const math::v3& viewOrigin,
                       const lumen::LumenVolumeConfig* volumes, u32 volumeCount) {
    GIVolumeHit result;
    if (!volumes || volumeCount == 0) return result;

    const GizmoRay ray = ScreenRay(screenX, screenY, viewportW, viewportH,
                                   invViewProj, viewOrigin);

    f32 bestT = 3.402823466e+38f;   // FLT_MAX
    for (u32 i = 0; i < volumeCount; ++i) {
        math::v3 bmin, bmax;
        GetVolumeAABB(volumes[i], bmin, bmax);

        // slab 法：逐轴压缩 [t0, t1]，退化轴（dir≈0）退化为包含性检查
        f32 t0 = 0.0f;
        f32 t1 = bestT;             // 只关心比当前最近更近的命中
        bool miss = false;
        const f32 o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
        const f32 d[3] = {ray.dir.x,    ray.dir.y,    ray.dir.z};
        const f32 mn[3] = {bmin.x, bmin.y, bmin.z};
        const f32 mx[3] = {bmax.x, bmax.y, bmax.z};
        for (int a = 0; a < 3 && !miss; ++a) {
            if (std::abs(d[a]) < 1e-8f) {
                if (o[a] < mn[a] || o[a] > mx[a]) miss = true;
                continue;
            }
            f32 ta = (mn[a] - o[a]) / d[a];
            f32 tb = (mx[a] - o[a]) / d[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) miss = true;
        }
        if (miss || t1 < 0.0f) continue;        // 盒在射线后方
        const f32 tEnter = std::max(t0, 0.0f);  // origin 在盒内 → 0
        if (tEnter < bestT) {
            bestT = tEnter;
            result.hit = true;
            result.volumeIndex = i;
            result.tMin = tEnter;
        }
    }
    return result;
}

GIVolumeHit PickPipelineVolumes(StandardRenderPipeline* pipeline,
                                f32 screenX, f32 screenY,
                                u32 viewportW, u32 viewportH,
                                const math::m4x4& invViewProj,
                                const math::v3& viewOrigin) {
    if (!pipeline) return GIVolumeHit{};
    const auto& vols = pipeline->GetSettings().lumen.volume_overrides;
    if (vols.empty()) return GIVolumeHit{};
    return PickVolume(screenX, screenY, viewportW, viewportH,
                      invViewProj, viewOrigin,
                      vols.data(), static_cast<u32>(vols.size()));
}

// ============================================================================
// 2) 模式 + selected volume
// ============================================================================

void      SetMode(GizmoMode mode) { State().mode = mode; }
GizmoMode GetMode()               { return State().mode; }

void SetSelectedVolume(u32 index) { State().selectedVolume = index; }
u32  GetSelectedVolume()          { return State().selectedVolume; }

void SetDragReferenceDistance(f32 distance) {
    if (std::isfinite(distance) && distance > 0.0f)
        State().refDistance = distance;
}
f32  GetDragReferenceDistance()   { return State().refDistance; }

// ============================================================================
// 3) 编辑（copy-modify-commit：UpdateSettings 置 settings_dirty_ → 下一渲染
//    帧 ApplyConfigChanges 重建 Lumen passes —— 与 GIEditorPanel 同一路径）
// ============================================================================

bool TranslateVolume(StandardRenderPipeline* pipeline, u32 index,
                     const math::v3& worldDelta) {
    if (!pipeline) return false;
    if (!std::isfinite(worldDelta.x) || !std::isfinite(worldDelta.y) ||
        !std::isfinite(worldDelta.z))
        return false;
    RenderPipelineSettings s = pipeline->GetSettings();   // copy
    if (index >= s.lumen.volume_overrides.size()) return false;
    auto& v = s.lumen.volume_overrides[index];
    const math::v3 base = v.use_origin_override ? v.origin_override
                                                : math::v3{0.0f, 0.0f, 0.0f};
    v.origin_override = math::v3{base.x + worldDelta.x,
                                 base.y + worldDelta.y,
                                 base.z + worldDelta.z};
    v.use_origin_override = true;
    pipeline->UpdateSettings(s);
    return true;
}

namespace {

u32 ScaleCount(u32 count, f32 factor) {
    const f64 scaled = static_cast<f64>(count) * static_cast<f64>(factor);
    const f64 rounded = std::round(scaled);
    const f64 lo = static_cast<f64>(kMinProbeCount);
    const f64 hi = static_cast<f64>(kMaxProbeCount);
    const f64 clamped = std::clamp(rounded, lo, hi);
    return static_cast<u32>(clamped);
}

} // anonymous namespace

bool ScaleVolume(StandardRenderPipeline* pipeline, u32 index,
                 f32 factor, u32 axis) {
    if (!pipeline || axis > kScaleAllAxes) return false;
    if (!std::isfinite(factor) || factor <= 0.0f) return false;
    RenderPipelineSettings s = pipeline->GetSettings();   // copy
    if (index >= s.lumen.volume_overrides.size()) return false;
    auto& v = s.lumen.volume_overrides[index];
    const bool all = (axis == kScaleAllAxes);
    if (all || axis == 0) v.probe_count_x = ScaleCount(v.probe_count_x, factor);
    if (all || axis == 1) v.probe_count_y = ScaleCount(v.probe_count_y, factor);
    if (all || axis == 2) v.probe_count_z = ScaleCount(v.probe_count_z, factor);
    pipeline->UpdateSettings(s);
    return true;
}

// ============================================================================
// 4) 拖挽数学
// ============================================================================

math::v3 DragDelta(const GizmoRay& prevRay, const GizmoRay& curRay,
                   const math::v3& camRight, const math::v3& camUp,
                   f32 refDistance) {
    const math::v3 zero{0.0f, 0.0f, 0.0f};
    if (!std::isfinite(refDistance) || refDistance <= 0.0f) return zero;

    // 参考点：上一帧射线上的拾取点；参考平面法线 = 视线方向
    const math::v3 p0{prevRay.origin.x + prevRay.dir.x * refDistance,
                      prevRay.origin.y + prevRay.dir.y * refDistance,
                      prevRay.origin.z + prevRay.dir.z * refDistance};
    const math::v3 n = Normalize3(Cross3(camRight, camUp));
    if (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f) return zero;

    // 当前射线与参考平面求交（curRay.origin 与 prevRay.origin 通常同为视点）
    const f32 denom = Dot3(curRay.dir, n);
    if (std::abs(denom) < 1e-6f) return zero;   // 平行：无位移定义
    const math::v3 toP0{p0.x - curRay.origin.x, p0.y - curRay.origin.y,
                        p0.z - curRay.origin.z};
    const f32 t = Dot3(toP0, n) / denom;
    if (!std::isfinite(t) || t < 0.0f) return zero;
    const math::v3 p1{curRay.origin.x + curRay.dir.x * t,
                      curRay.origin.y + curRay.dir.y * t,
                      curRay.origin.z + curRay.dir.z * t};
    return math::v3{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
}

// ============================================================================
// 5) AABB 线框
// ============================================================================

bool GetVolumeWireframe(const lumen::LumenVolumeConfig& v,
                        math::v3 outEndpoints[kWireframeEndpointCount]) {
    if (!outEndpoints) return false;
    math::v3 bmin, bmax;
    GetVolumeAABB(v, bmin, bmax);

    // 8 角点：bit0 = +x, bit1 = +y, bit2 = +z
    math::v3 c[8];
    for (u32 i = 0; i < 8; ++i) {
        c[i] = math::v3{(i & 1u) ? bmax.x : bmin.x,
                        (i & 2u) ? bmax.y : bmin.y,
                        (i & 4u) ? bmax.z : bmin.z};
    }
    // 12 条边：底面 4 + 顶面 4 + 竖边 4（顶点对仅在单轴上不同）
    static const u32 kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},   // z = bmin 面
        {4, 5}, {5, 7}, {7, 6}, {6, 4},   // z = bmax 面
        {0, 4}, {1, 5}, {3, 7}, {2, 6},   // 竖边
    };
    for (u32 e = 0; e < 12; ++e) {
        outEndpoints[e * 2 + 0] = c[kEdges[e][0]];
        outEndpoints[e * 2 + 1] = c[kEdges[e][1]];
    }
    return true;
}

bool GetPipelineVolumeWireframe(StandardRenderPipeline* pipeline, u32 index,
                                math::v3 outEndpoints[kWireframeEndpointCount]) {
    if (!pipeline || !outEndpoints) return false;
    const auto& vols = pipeline->GetSettings().lumen.volume_overrides;
    if (index >= vols.size()) return false;
    return GetVolumeWireframe(vols[index], outEndpoints);
}

// ============================================================================
// 6) GIEditorPanel 动作集成：gizmo.mode.select / translate / scale / off
// ============================================================================

namespace {
bool ActionSelect(void*)    { SetMode(GizmoMode::Select);    return true; }
bool ActionTranslate(void*) { SetMode(GizmoMode::Translate); return true; }
bool ActionScale(void*)     { SetMode(GizmoMode::Scale);     return true; }
bool ActionOff(void*) {
    SetMode(GizmoMode::Off);
    SetSelectedVolume(kNoVolumeSelected);   // 关 gizmo 即取消选中
    return true;
}
} // anonymous namespace

bool RegisterDefaultActions() {
    static const gi_panel::PanelAction kActions[] = {
        {"gizmo.mode.select",    "Gizmo: Select",
         "Pick GI volumes in the viewport (click to select)",
         &ActionSelect,    nullptr},
        {"gizmo.mode.translate", "Gizmo: Translate",
         "Drag the selected GI volume (writes origin_override)",
         &ActionTranslate, nullptr},
        {"gizmo.mode.scale",     "Gizmo: Scale",
         "Drag to scale the selected GI volume probe counts (4..128)",
         &ActionScale,     nullptr},
        {"gizmo.mode.off",       "Gizmo: Off",
         "Disable the GI volume gizmo and deselect",
         &ActionOff,       nullptr},
    };
    for (const auto& a : kActions) gi_panel::RegisterAction(a);  // 同 id 覆盖
    return true;
}

} // namespace primal::graphics::gi_gizmo
} // namespace primal::graphics
