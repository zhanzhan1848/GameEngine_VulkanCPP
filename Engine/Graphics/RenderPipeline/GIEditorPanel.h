#pragma once

// ============================================================================
// GIEditorPanel — M6/T6.1 GI 编辑器面板（可编程面板 API + 数据驱动描述符）
//
// 调研结论（2026-08-24）：Engine 构建内无 ImGui（Particles/CurveEditor.cpp
// 为 stub），PrimalEditor(C#)/PrimalEditor_Avalonia 经 EngineDLL 的
// EDITOR_INTERFACE C ABI 通信且 ParamDescriptor 表暂无 C# 消费者，唯一活
// 调试 UI 是 WASM shell（手写 HTML 滑条）。因此本文件交付【数据面】：
//   1) 可编程面板 API —— 动作注册（按钮，如 Bake）+ 进程级注册表；
//   2) 数据驱动参数描述符 —— GIDebugParams 运行时旋钮表（进程单例）、
//      LumenVolumeConfig 字段表（volume 列表编辑）、RenderPipelineSettings
//      的 GI Axes 五轴分组（在 RenderPipelineConfig.cpp 的 kParams 表内，
//      经 GetParamDescriptors / EngineDLL GetPipelineParamDescriptors 暴露）。
// UI 宿主（PrimalEditor / WASM shell / 未来 ImGui shell / 测试）遍历描述符
// 表自动生成控件，按钮经动作注册表触发。
//
// 零开销红线：全部 API 为被动数据 + 按需调用的函数，无每帧轮询、无渲染
// 路径改动；面板不打开 = 无人调用 = 零成本。默认行为零变化（GIDebugParams
// 新增 probe_vis 字段默认 Off 且无 pass 读取）。
//
// 生命周期分面（见 GIDebugParams.h 头注释）：
//   - axes / volume 列表 = init 时配置面（LumenConfig 内嵌，本面板写回后
//     触发热重载：axes → SetLumenConfig；volume → UpdateSettings 置
//     settings_dirty_，下一渲染帧 ApplyConfigChanges 自动重建 Lumen passes）；
//   - GIDebugParams = 运行时热调面（进程单例，写后即时生效）。
// ============================================================================

#include "Graphics/RenderPipeline/PipelineQualityConfig.h"  // ParamDescriptor / RenderPipelineSettings
#include "Graphics/Lumen/LumenTypes.h"                       // GIFiveAxisConfig / LumenVolumeConfig
#include "Graphics/Lumen/GIDebugParams.h"                    // GIDebugParams

namespace primal::graphics {
class StandardRenderPipeline;   // 头内仅指针引用（实现在 .cpp include 完整定义）

namespace lumen {
// 完整定义在 Graphics/Lumen/StaticProbe/StaticProbeBaker.h —— 该头经 BVH.hpp
// 打开 primal::graphics::utl 命名空间（遮蔽 primal::utl 的 vector 查找），故
// 此处只前置声明、由 .cpp 在安全位置 include（Bake 调用点同理）。
struct ProbeBakingScene;
struct ProbeBakingParams;
} // namespace lumen

namespace gi_panel {

// ============================================================================
// 1) 数据驱动描述符表
//
// ParamDescriptor.offset 的 base struct 由各表自行定义（kParams 表是
// RenderPipelineSettings；本文件两张表分别是 GIDebugParams 与
// LumenVolumeConfig）。宿主按 group 分组渲染控件。
// ============================================================================

/// GIDebug() 进程单例的运行时旋钮描述符表（pass 开关/权重/freeze/probe vis）。
/// 写路径：宿主按 offset 直接写 GIDebug() 单例，即时生效（无重初始化）。
const ParamDescriptor* GetDebugParamDescriptors(u32& out_count);

/// LumenVolumeConfig 字段描述符表（volume 列表每条目的编辑控件）。
/// 写路径：Get/SetVolumeOverride（copy-modify-commit，commit 时热重载）。
const ParamDescriptor* GetVolumeParamDescriptors(u32& out_count);

// ============================================================================
// 2) volume 列表编辑（cap 4；counts/spacing/ray 距离）
//
// 显式传 pipeline（无全局 attach）；修改走 copy → UpdateSettings →
// settings_dirty_ → 下一渲染帧 ApplyConfigChanges 自动重建 Lumen passes
// （= "改后自动触发重初始化"；宿主亦可只读描述符展示、由
// NeedsVolumeReinit 查询待重建状态做提示）。
// ============================================================================

constexpr u32 kMaxVolumes = 4;

/// overrides 列表当前条数（空 = 从 ddgi_* 字段推导 near+far，见
/// StandardRenderPipeline::InitializeLumenPasses；空列表时本 API 返回 0，
/// 宿主可用 AppendVolumeOverride 物化当前推导结果为显式条目再编辑）。
u32  GetVolumeOverrideCount(StandardRenderPipeline* pipeline);
bool GetVolumeOverride(StandardRenderPipeline* pipeline, u32 index,
                       lumen::LumenVolumeConfig* out);
/// 写入第 index 条 override（clamp: counts 4..64 / spacing 0.5..20 /
/// rays 8..256 / maxDist 5..200）。提交后置 settings_dirty_（下帧重建）。
bool SetVolumeOverride(StandardRenderPipeline* pipeline, u32 index,
                       const lumen::LumenVolumeConfig& v);
/// 追加一条（拷贝默认 near 配置；达上限 4 返回 false）。
bool AppendVolumeOverride(StandardRenderPipeline* pipeline);
bool RemoveVolumeOverride(StandardRenderPipeline* pipeline, u32 index);

/// 物化当前推导 volume（overrides 为空时把 near+far 推导结果写入
/// volume_overrides，使编辑可见）。已有 overrides 时为 no-op 返回 true。
bool MaterializeDerivedVolumes(StandardRenderPipeline* pipeline);

/// 读取生效的第 index 条 volume 配置（overrides 优先；否则按 ddgi_*
/// 字段推导 near+far）。越界返回 false。
bool GetEffectiveVolumeConfig(StandardRenderPipeline* pipeline, u32 index,
                              lumen::LumenVolumeConfig* out);

// ============================================================================
// 3) 五轴（GIFiveAxisConfig）热重载
// ============================================================================

/// 写 LumenConfig.axes 并触发 SetLumenConfig 热重载：ApplyGIAxis1 用 axes
/// 覆写 quality.enable_*（轴视图单一真源），随后补 UpdateSettings 置
/// settings_dirty_ 使已初始化管线在下一渲染帧重建 Lumen passes（如新开
/// SurfaceCache/ScreenProbes 需要创建 pass 实例）。
/// 注意：SetLumenConfig 会把 quality 重置为 FromPreset(lumen.quality) 档位
/// 默认（现有语义，与 EngineDLL PipelineSetLumenConfig 一致），再被轴1 覆写。
bool ApplyAxesConfig(StandardRenderPipeline* pipeline,
                     const lumen::GIFiveAxisConfig& axes);

// ============================================================================
// 4) Bake（StaticProbeBaker 入口；grid 参数取当前 volume 0 配置）
// ============================================================================

/// CPU 烘焙 static probe volume：
///   - grid（counts/spacing/origin）取当前生效 volume 0 配置（overrides[0]
///     优先，否则 ddgi_* 推导；origin 用 origin_override）；
///   - 若 pipeline 内 static_probe_volume_ 的 grid 与 volume 0 不一致，先用
///     pipeline->GetDevice() Shutdown+Initialize 重建；
///   - 调 lumen::StaticProbeBaker::Bake 并 UploadToGPU。
/// status/status_cap 可选：回写人类可读结果（供面板显示）。
/// 线程性：会释放/重建 GPU buffer，必须在渲染帧之外调用（编辑器暂停渲染/
/// 帧间隙），渲染循环进行中调用属使用方错误。
/// 注：ProbeBakingScene/Params 完整类型在调用点 include StaticProbeBaker.h。
bool BakeStaticProbes(StandardRenderPipeline* pipeline,
                      const lumen::ProbeBakingScene& scene,
                      const lumen::ProbeBakingParams& params,
                      char* status, u32 status_cap);

// ============================================================================
// 5) 可编程面板动作注册（按钮）
//
// 进程级注册表。内置动作（静态表）：无；宿主（EngineDLL 导出层 / 引擎内
// 测试 / 未来 ImGui shell）用 RegisterAction 注入带上下文的回调。红线：
// 注册表为被动容器，无后台线程/每帧钩子——不 Register = 零开销。
// ============================================================================

using PanelActionFn = bool (*)(void* user_data);

struct PanelAction {
    const char*  id;         // 稳定标识（如 "bake_static_probes"）
    const char*  label;      // 按钮文案
    const char*  tooltip;    // 悬浮说明
    PanelActionFn fn;        // 触发回调（user_data 原样透传）
    void*        user_data;  // 调用方 owned（典型：Bake 上下文结构）
};

void RegisterAction(const PanelAction& action);        // 同 id 覆盖
void UnregisterAction(const char* id);
void ClearActions();
u32  GetActionCount();
const PanelAction* GetActionByIndex(u32 index);        // 遍历渲染按钮
const PanelAction* GetActionById(const char* id);
bool InvokeAction(const char* id);                     // 未知 id → false

} // namespace primal::graphics::gi_panel
} // namespace primal::graphics
