#pragma once

// ============================================================================
// GIVolumeGizmo — M6/T6.2 world volume 的 viewport gizmo（拾取/平移/缩放）
//
// Docs/2026-08-24-gi-optimization-plan.md 最后一件：world probe volume 需要
// UI 层手动编辑位置/规模。本文件是引擎侧【纯数学/数据层】：
//   1) 屏幕点 → 世界射线 → 各 volume AABB 求交（最近优先）＝拾取；
//   2) gizmo 模式 + selected volume（进程级状态，与 GIDebug 单例同面的
//      被动数据，无每帧轮询）；
//   3) Translate/Scale 编辑 —— copy-modify-commit 写 settings.lumen.
//      volume_overrides（UpdateSettings 置 settings_dirty_ → 下一渲染帧
//      ApplyConfigChanges 自动重建 Lumen passes，与 GIEditorPanel 同路径）；
//   4) DragDelta：两条拖拽射线 + 相机 right/up → 世界位移（供 Translate）；
//   5) AABB 线框端点 + 选中高亮色常量（GeometryDebugPass 接线点见
//      GetVolumeWireframe 注释，本任务不强制接线）。
//
// 编辑对象 = LumenVolumeConfig（Engine/Graphics/Lumen/LumenTypes.h）：
//   - Translate 写 origin_override + use_origin_override（origin_override
//     的消费点是 BakeStaticProbes 的 grid origin；运行时 DDGI 动态 volume
//     仍按 world-centered 跟随相机，见 StandardRenderPipeline::
//     InitializeLumenPasses）；
//   - Scale 写 probe_count_*（gizmo clamp 4..128 —— 比面板描述符表的
//     4..64 宽：拖拽粗调允许到 128，后续经 SetVolumeOverride 精调会再收到 64）。
//
// AABB 语义（本 gizmo 定义，与配置空间一致）：min = use_origin_override ?
// origin_override : (0,0,0)（world-centered），max = min + counts*spacing。
//
// 零开销红线：全部 API 被动调用，无渲染路径改动、无 shader 改动；模式 Off
// （默认）且无人调用 = 零成本。
//
// include 顺序契约（同 GIEditorPanel.h 头注释的先例）：本头只用
// primal::math / primal::graphics::lumen 的 POD，不碰 primal::utl，可任意
// 顺序 include。但消费它的 .cpp 若同时 include StaticProbeBaker.h（经
// BVH.hpp 打开 primal::graphics::utl，遮蔽 primal::utl 名字查找），必须把
// StaticProbeBaker.h 放在最后 —— 见 EngineDLL/RenderPipelineAPI.cpp 顶部注释。
// ============================================================================

#include "Utilities/MathTypes.h"      // math::v3 / v4 / m4x4
#include "Graphics/Lumen/LumenTypes.h"   // lumen::LumenVolumeConfig

namespace primal::graphics {
class StandardRenderPipeline;   // 头内仅指针引用（实现在 .cpp include 完整定义）

namespace gi_gizmo {

// ============================================================================
// 1) 拾取：屏幕 → NDC → 世界射线 → AABB slab 求交
// ============================================================================

/// 一次拾取的结果。tMin = 射线参数（世界单位），origin 在盒内时为 0。
struct GIVolumeHit {
    bool  hit = false;
    u32   volumeIndex = 0;
    float tMin = 0.0f;
};

/// 拖拽/拾取用世界射线（dir 已归一化约定；ScreenRay 保证）。
struct GizmoRay {
    math::v3 origin{};
    math::v3 dir{0.0f, 0.0f, 1.0f};
};

/// 屏幕（像素，Y 向下）→ NDC → invViewProj 反变换 → 世界射线。
/// screen 坐标可为小数（亚像素）；origin 取 viewOrigin（视点）。
/// 退化输入（零视口 / w≈0 / 零长度 dir）回退 dir=(0,0,1)，不产生 NaN。
GizmoRay ScreenRay(f32 screenX, f32 screenY, u32 viewportW, u32 viewportH,
                   const math::m4x4& invViewProj, const math::v3& viewOrigin);

/// volume 的 gizmo AABB（min = origin 或 world 原点；max = min + counts*spacing）。
void GetVolumeAABB(const lumen::LumenVolumeConfig& v,
                   math::v3& outMin, math::v3& outMax);

/// 对 volumes[] 逐个 slab 求交，返回最近命中（tMin 最小者优先）。纯函数，
/// 不写任何状态。null volumes / count 0 → 未命中。
GIVolumeHit PickVolume(f32 screenX, f32 screenY, u32 viewportW, u32 viewportH,
                       const math::m4x4& invViewProj, const math::v3& viewOrigin,
                       const lumen::LumenVolumeConfig* volumes, u32 volumeCount);

/// PickVolume 的管线便捷版：volumes 取 settings.lumen.volume_overrides
/// （可编辑数据面）。overrides 为空（推导模式）时未命中 —— 宿主应先调
/// gi_panel::MaterializeDerivedVolumes 物化再拾取（索引才能对上编辑面）。
GIVolumeHit PickPipelineVolumes(StandardRenderPipeline* pipeline,
                                f32 screenX, f32 screenY,
                                u32 viewportW, u32 viewportH,
                                const math::m4x4& invViewProj,
                                const math::v3& viewOrigin);

// ============================================================================
// 2) gizmo 模式 + selected volume（进程级被动状态，默认 Off/未选中）
// ============================================================================

enum class GizmoMode : u32 {
    Off       = 0,   // gizmo 关闭（默认）
    Select    = 1,   // 点击拾取/切换 selected volume
    Translate = 2,   // 拖拽平移（DragDelta → TranslateVolume）
    Scale     = 3,   // 拖拽缩放（ScaleVolume）
};

void       SetMode(GizmoMode mode);
GizmoMode  GetMode();

constexpr u32 kNoVolumeSelected = 0xFFFFFFFFu;

/// selected volume（高亮/编辑对象）。越界值会被 Translate/Scale 拒绝，
/// 这里不做存在性校验（volume 列表随 settings 变化）。
void SetSelectedVolume(u32 index);
u32  GetSelectedVolume();

/// 拖拽参考距离：Select 拾取命中时记录的 tMin，供 DragDelta 把屏幕拖拽
/// 映射到该深度的世界平面。默认 10。
void SetDragReferenceDistance(f32 distance);
f32  GetDragReferenceDistance();

// ============================================================================
// 3) 编辑：Translate / Scale（copy-modify-commit，提交自动置
//    settings_dirty_ → 下一渲染帧重建 Lumen passes）
// ============================================================================

/// 平移：origin_override += worldDelta，并置 use_origin_override = true。
/// 首次平移即把 volume 从 world-centered 变为显式 origin。
bool TranslateVolume(StandardRenderPipeline* pipeline, u32 index,
                     const math::v3& worldDelta);

/// gizmo 缩放的 counts clamp（比面板描述符表 4..64 宽，见文件头）。
constexpr u32 kMinProbeCount = 4;
constexpr u32 kMaxProbeCount = 128;
constexpr u32 kScaleAllAxes  = 3;   // axis 传此值 = 三轴同缩

/// 缩放：probe_count[axis] = clamp(lround(count * factor), 4, 128)。
/// axis：0=x / 1=y / 2=z / kScaleAllAxes=三轴同缩；其余值拒绝。
/// spacing 不变（counts 语义 = 分辨率而非包围盒尺寸）。
bool ScaleVolume(StandardRenderPipeline* pipeline, u32 index,
                 f32 factor, u32 axis);

// ============================================================================
// 4) 拖挽数学：两条射线 → 世界位移（供 Translate 消费）
// ============================================================================

/// 把一次拖拽（prevRay → curRay，均来自 ScreenRay）映射为参考平面上的
/// 世界位移。参考平面 = 过 prevRay.origin + prevRay.dir * refDistance、
/// 法线 cross(camRight, camUp)（即视线方向）的平面；refDistance 典型取拾取
/// 命中的 tMin（GetDragReferenceDistance）。curRay 平行于平面（或参考距离
/// 非有限）时返回零位移，不产生 NaN。
math::v3 DragDelta(const GizmoRay& prevRay, const GizmoRay& curRay,
                   const math::v3& camRight, const math::v3& camUp,
                   f32 refDistance);

// ============================================================================
// 5) AABB 线框 + 高亮色（GeometryDebugPass 接线点，本任务不强制接线）
// ============================================================================

/// 12 条边 × 2 端点 = 24 端点，端点序 (e0a,e0b, e1a,e1b, ...)。
constexpr u32 kWireframeEndpointCount = 24;

bool GetVolumeWireframe(const lumen::LumenVolumeConfig& v,
                        math::v3 outEndpoints[kWireframeEndpointCount]);

/// 管线便捷版：读 overrides[index]，越界/无管线 → false。
bool GetPipelineVolumeWireframe(StandardRenderPipeline* pipeline, u32 index,
                                math::v3 outEndpoints[kWireframeEndpointCount]);

// RGBA 线框颜色。接线点：GeometryDebugPass（RenderPasses/Debug/
// GeometryDebugPass.h）后续可按 GizmoMode != Off 遍历 overrides 调
// GetPipelineVolumeWireframe 画盒线框，选中条目用 kColorSelected。
constexpr f32 kColorSelected[4]   = {1.00f, 0.55f, 0.10f, 1.00f};  // 橙
constexpr f32 kColorUnselected[4] = {0.20f, 0.75f, 1.00f, 0.55f};  // 青半透明

// ============================================================================
// 6) GIEditorPanel 动作集成（gizmo.mode.*）
//
// 面板按钮 → RegisterAction（见 GIEditorPanel.h §5）：本函数向进程级注册
// 表注入四个内置动作（幂等，同 id 覆盖）：
//   gizmo.mode.select / translate / scale / off
// off 同时清 selected volume。红线保持：不调用 = 不注册 = 零开销。
// ============================================================================

bool RegisterDefaultActions();

} // namespace primal::graphics::gi_gizmo
} // namespace primal::graphics
