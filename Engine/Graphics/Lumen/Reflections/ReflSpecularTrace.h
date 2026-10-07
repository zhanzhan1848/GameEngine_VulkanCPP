#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Lumen/WRC/WRCCacheView.h"
#include "Graphics/Lumen/GICacheView.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"   // ReflProbeEnabled

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

// ============================================================================
// ReflSpecularTracePass — [REFL] 屏幕外 specular 兜底（SC 正确性 ×
// Reflections 整合计划 R4/R5）。
//
// SSR 未命中段（ssr.a < 门限）沿反射射线走 GlobalSDF；命中点做与
// WRCTraceFinalize 同构的 SC 卡片查询（R5，PRIMAL_REFL=2 默认路径）；
// PRIMAL_REFL_PROBE=1 叠加 WRC 近场探针 cos-lobe 采样（R4 原型档）。
// 排在 R1/R2 之后的理由：卡片覆盖缺口会直接成像为反射黑洞——产出率门禁
// 通过后才默认化（PRIMAL_REFL=2）。
//
// 输出全分辨率 RGBA16F（RGB = Fresnel 加权辐射，A = 权重），由
// FusionComposite binding 8 消费；缺席 → black（v1 形态）。
// Metal：Vulkan 先行（WRC 无 .metal 同源约束），Initialize 平台守卫。
// ============================================================================
class ReflSpecularTracePass {
public:
    ReflSpecularTracePass() = default;
    ~ReflSpecularTracePass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 width, u32 height);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    /// 每帧物理资源快照（RG 句柄声明读边；物理句柄绑描述符）。
    struct FrameData {
        rendergraph::RGResourceHandle depth_rg{};
        rendergraph::RGResourceHandle albedo_rg{};
        rendergraph::RGResourceHandle orm_rg{};
        rendergraph::RGResourceHandle ssr_rg{};
        rhi::ResourceHandle depth{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle albedo{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle orm{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle ssr{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle normal{rhi::handles::INVALID_RESOURCE};
        math::m4x4 inv_view_proj{};
        math::v3 camera_pos{0.0f, 0.0f, 0.0f};
        u32 width{0};
        u32 height{0};
        // SC 公共真值层（卡片查询源）。
        SurfaceCacheView sc{};
        // WRC 近场探针（R4 探针档源）。
        WRCCacheView wrc{};
        // 离线 SDF 优先，GlobalSDF 运行时级联回退（AddPass 内解析）。
        rhi::ResourceHandle offline_sdf{rhi::handles::INVALID_RESOURCE};
        math::v3 offline_sdf_origin{0.0f};
        math::v3 offline_sdf_extent{0.0f};
        u32 offline_sdf_resolution{0};
        bool signed_sdf{false};
        math::v3 sky_color{0.15f};
        f32 sky_tier{0.15f};
        f32 ray_max_distance{50.0f};
        bool probe_mode{false};   ///< PRIMAL_REFL_PROBE=1（R4 档）
    };

    /// 返回本帧输出的 RG 句柄（fusion binding 8 的读边声明源）。
    rendergraph::RGResourceHandle AddPass(rendergraph::RenderGraph& graph,
                                          const FrameData& data, u32 frame_index);

    /// 本帧输出物理纹理（fusion binding 8；三缓冲槽位 = frame%3）。
    rhi::ResourceHandle GetOutputTex(u32 frame) const {
        return output_tex_[frame % 3u];
    }

private:
    bool initialized_{false};
    rhi::RHIDeviceBase* device_{nullptr};
    u32 render_width_{0};
    u32 render_height_{0};

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle output_tex_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_2d_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_3d_{rhi::handles::INVALID_RESOURCE};
    bool dummy_transitioned_{false};
};

} // namespace primal::graphics::lumen
