#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Passes/BlurPass.h"

#include <memory>

namespace primal::graphics {

namespace nanite { class GPUDrivenDrawPipeline; }
class RenderSceneSnapshot;
class BlurPass;

namespace rendergraph { class RenderGraph; }

struct ShadowMapInputs {
    nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline = nullptr;
    RenderSceneSnapshot* scene_snapshot = nullptr;
    math::v3 camera_position{};
    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v3 light_direction{ -0.9f, 1.5f, -0.8f };
    // sun-audit §5.3: PCSS 光源角直径（度）——见 SceneLightingParams 同名字段。
    float sun_angular_diameter_deg = 0.5357f;
    u32 current_buffer_index = 0;
    lumen::ShadowQuality shadow_quality = lumen::ShadowQuality::PCF_16;
};

struct ShadowMapOutputs {
    // 2026-09-22 修复：全部句柄补 INVALID 默认值。此前默认零初始化——
    // pass 被跳过/禁用时 `ShadowMapOutputs{}` 携带 0 句柄上流，而全仓
    // 的有效性守卫都只认 INVALID 哨兵，0 直接写进描述符触发 Metal
    // validation "Null texture access"（fragmentLighting_gpuDriven
    // 采样 texture6 shadowVisibility 处中止，帧 2-3 稳定复现）。
    rendergraph::RGResourceHandle shadow_visibility_rg{};
    rhi::ResourceHandle shadow_visibility_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle shadow_translucent_rg{};   // P4.1
    rhi::ResourceHandle shadow_translucent_tex{rhi::handles::INVALID_RESOURCE};  // P4.1
    // P4.2 (2026-09-15): CSM 2→4 级联——各 cascade 的 light VP
    math::m4x4 shadow_matrix[4];
    // VSM path: blurred RG32 moments per cascade (INVALID when VSM is off —
    // DeferredLighting then samples shadow_visibility_tex instead).
    rendergraph::RGResourceHandle shadow_moments_rg[4]{};
    // P1（sponza-gap-reaudit §4.4）: PCSS 链的 light-space 深度图（D32→R32
    // blit）RG 句柄，与 shadow_matrix 同源同帧。消费方：FroxelFogPass 在
    // 无 VSM moments 时做逐 froxel 深度比较（体积阴影 mode 2）。
    rendergraph::RGResourceHandle shadow_depth_rg[4]{};
    rhi::ResourceHandle shadow_moments_tex[4]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    bool vsm_enabled = false;
    // P0（sun-audit §7）: pass 尺寸 provenance——[SunContract] 导出用真值。
    u32 shadow_map_resolution = 0;      ///< 每级联 shadow map 边长（如 2048）
    u32 shadow_cascade_count = 0;       ///< 级联数（P4.2 起 = 4）
    float shadow_cascade_extents[4]{};  ///< 各级联正交半宽（12/30/90/270）
};

class ShadowMapModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline,
                    u32 render_width, u32 render_height, u32 num_instances, u32 max_clusters);
    void Shutdown();

    // Must call after Initialize(). Accepts a pre-compiled compute shader for shadow filtering.
    bool InitializeShadowFilter(rhi::ShaderHandle shadow_filter_shader);

    ShadowMapOutputs AddPasses(rendergraph::RenderGraph& graph, const ShadowMapInputs& inputs);

    rhi::ResourceHandle GetShadowVisibilityTexture(u32 /*buffer_index*/) const { return shadow_visibility_tex_; }
    const math::m4x4* GetCachedShadowVP(u32 buffer_index) const { return cached_shadow_vp_[buffer_index % 3]; }

    // VSM (variance shadow maps): moments raster + Gaussian blur + Chebyshev
    // sampling in DeferredLighting. When off, the module falls back to the
    // PCSS chain (D32 → R32 blit → ShadowFilter half-res visibility).
    void SetVSMEnabled(bool enabled) { vsm_enabled_ = enabled; }
    bool IsVSMEnabled() const { return vsm_enabled_; }

private:
    bool InitializeShadowFilterPipeline();
    math::m4x4 ComputeCascadeVP(math::v3 lightDir, math::v3 cameraPos,
                                float orthoExtent, u32 shadowMapSize,
                                const ShadowMapInputs& inputs, u32 cascade,
                                float* outNearPlane = nullptr, float* outFarPlane = nullptr);

    rhi::RHIDeviceBase* device_ = nullptr;
    nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    // Shadow VP caching（P4.2: 4 级联）
    math::m4x4 cached_shadow_vp_[3][4]{};       // [buffer][cascade]
    bool shadow_cache_valid_[3][4]{false};
    bool shadow_cache_globally_valid_{false};

    // VSM moments blur (BlurPass ping-pong H+V, radius 3 / sigma 1.0 — same
    // parameters as Metal's ForwardRenderer VSM loop)
    bool vsm_enabled_{true};
    std::unique_ptr<BlurPass> moments_blur_;
    rhi::ResourceHandle moments_temp_{
        rhi::handles::INVALID_RESOURCE};        // shared blur ping-pong target

    // Shadow filter (half-res compute)
    rhi::PipelineHandle shadow_filter_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle shadow_filter_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle shadow_filter_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle shadow_filter_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle shadow_visibility_tex_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadow_translucent_tex_{rhi::handles::INVALID_RESOURCE};  // P4.1
    rhi::ResourceHandle shadow_filter_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    // T4.6.5 part 40: linear sampler shared across all ShadowFilter texture
    // samples (shadowMap0/1, normalTex, depthTex). Sampler binding lives at
    // descriptor slot 6 (see InitializeShadowFilter layout).
    rhi::SamplerHandle shadow_filter_sampler_{rhi::handles::INVALID_SAMPLER};
};

} // namespace primal::graphics
