#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Passes/BlurPass.h"

#include <memory>

namespace primal::graphics {

namespace nanite { class GPUDrivenDrawPipeline; }
struct RenderSceneSnapshot;
class BlurPass;

namespace rendergraph { class RenderGraph; }

struct ShadowMapInputs {
    nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline = nullptr;
    RenderSceneSnapshot* scene_snapshot = nullptr;
    math::v3 camera_position{};
    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v3 light_direction{ -0.9f, 1.5f, -0.8f };
    u32 current_buffer_index = 0;
    lumen::ShadowQuality shadow_quality = lumen::ShadowQuality::PCF_16;
};

struct ShadowMapOutputs {
    rendergraph::RGResourceHandle shadow_visibility_rg;
    rhi::ResourceHandle shadow_visibility_tex;
    math::m4x4 shadow_matrix0;   // Cascade 0 light VP
    math::m4x4 shadow_matrix1;   // Cascade 1 light VP
    // VSM path: blurred RG32 moments per cascade (INVALID when VSM is off —
    // DeferredLighting then samples shadow_visibility_tex instead).
    rendergraph::RGResourceHandle shadow_moments_rg[2];
    rhi::ResourceHandle shadow_moments_tex[2]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    bool vsm_enabled = false;
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
                                const ShadowMapInputs& inputs, u32 cascade);

    rhi::RHIDeviceBase* device_ = nullptr;
    nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    // Shadow VP caching
    math::m4x4 cached_shadow_vp_[3][2]{};       // [buffer][cascade]
    bool shadow_cache_valid_[3][2]{false};
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
