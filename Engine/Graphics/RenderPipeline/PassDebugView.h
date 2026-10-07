#pragma once

#include "CommonHeaders.h"

namespace primal::graphics {

// ============================================================================
// PassDebugView — 视口实时 pass 可视化视图（编辑器 PASS 下拉的引擎侧单一事实源）
// ============================================================================
// 选中非 Off 视图时，BuildRenderGraph 的 Step 11 走 PassDebugViewModule 把对应
// 中间资源整屏画进 BackBuffer（替代 FinalBlit）；Off 恢复正常输出。
//
// 语义约定：
//   - 该功能要求完整渲染图路径（deferred/GI）。PipelineSetPassDebugView 在
//     mode != Off 时会强制 SetEditorMode(false)；SetEditorMode(true) 在视图
//     激活期间被忽略（退出 Play 不会把视口打回轻量前向路径）。
//   - 目标 pass 本帧未产出（如 SSAO 关闭、GI 内容未就绪）→ 整帧回退正常
//     FinalBlit 输出并打一次 Info 日志，绝不黑屏。
//   - ID 类视图（Visibility/MeshletID/ClusterID/MeshID）从可见性缓冲
//     R32_UINT（打包 (meshlet_id<<24)|primitive_id）+ cluster map 反查解码，
//     不污染 GBuffer 写入路径；哈希函数与 Dawn GPUDrivenDraw.wgsl 的
//     hash_id_to_color 保持一致（三端同色）。
enum class PassDebugView : u32 {
    Off = 0,
    Albedo,            // GBuffer albedo（材质色直读）
    Normal,            // GBuffer world normal（n*0.5+0.5）
    ORM,               // GBuffer occlusion/roughness/metallic 通道
    Depth,             // 线性化深度（near..far → 0..1）
    Velocity,          // 运动矢量（[-0.5,0.5] → [0,1]，绿/红通道）
    Visibility,        // 可见性缓冲 triangle_id 哈希伪彩色
    MeshletID,         // vis>>24 的 meshlet_id 哈希伪彩色
    ClusterID,         // cluster map 反查 cluster 序号哈希伪彩色
    MeshID,            // cluster map 反查 instance 序号哈希伪彩色
    SSAO,              // Lumen SSAO filter 输出（灰度）
    DeferredLighting,  // 延迟光照 HDR 输出（tonemap 显示）
    GIIndirect,        // Fusion 间接光 HDR 输出（tonemap 显示）
    SPGI,              // Screen Probe GI 降噪后输出（tonemap 显示）
    SCLightingAtlas,   // Surface Cache 光照 atlas（直读）
    // [F3-V1/V2 2026-09-20] SC 三 atlas 视图 + SPGI 原始输出（能量不对称
    // 修复计划 F3 可视化基建；[PDV] 缺口清单收口）。
    SCAlbedoAtlas,     // SC albedo atlas 直读（rgba8，gamma 显示）
    SCNormalAtlas,     // SC normal atlas（rg 八面体解码 → rgb）
    SCDepthAtlas,      // SC capture 深度（线性标尺 0..16m 灰度）
    SPGIRaw,           // SPGI 降噪前原始 gather 输出（tonemap 显示）
    // [F3 扩展 2026-09-20] SC 剩余存储全可视化（"所有纹理可看"）。
    SCEmissive,        // SC emissive atlas（HDR → 显示曝光 tonemap）
    SCShadowVis,       // SC 逐 texel 阴影可见性（buffer，白=lit 黑=shadowed）
    SCIndirect,        // SC radiosity 间接光（albedo×tile 入射，HDR tonemap）
    SCTileRadiance,    // SC tile 入射辐射（tile 域 rgb + validity alpha）
    SCTileAux,         // SC tile 兼容证据（R/G=oct 编码 B=中心深度 0..16m）
    Count
};

// 视图名（小写，供 CLI/UI 按名解析；"off"/"albedo"/...）。越界返回 nullptr。
const char* GetPassDebugViewName(u32 view);

// 按名解析（大小写不敏感）。未知名返回 -1。
s32 ParsePassDebugViewName(const char* name);

} // namespace primal::graphics
