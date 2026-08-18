#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace nanite { class GPUDrivenDrawPipeline; }
namespace lumen { class LumenSSAOPass; }

struct DeferredLightingInputs {
    nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline = nullptr;
    lumen::LumenSSAOPass* ssao_pass = nullptr;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v3 camera_position{};
    math::v4 light_pos{};
    math::v4 light_color{20.0f, 20.0f, 20.0f, 1.0f};
    math::v4 cascade_splits{600.0f, 2000.0f, 0.0f, 0.0f};
    math::m4x4 shadow_matrix0{};
    math::m4x4 shadow_matrix1{};

    u32 current_buffer_index = 0;

    // From ShadowMapModule outputs
    rendergraph::RGResourceHandle shadow_visibility_rg;
    rhi::ResourceHandle shadow_visibility_tex;

    // VSM path (ShadowMapModule vsm_enabled): blurred RG32 moments per
    // cascade — shader samples + Chebyshev upper bound instead of the
    // pre-filtered R8 visibility. shadow_moments_tex[] must be valid when
    // vsm_enabled is set, otherwise binding 6 (R8) is used.
    bool vsm_enabled = false;
    rendergraph::RGResourceHandle shadow_moments_rg[2];
    rhi::ResourceHandle shadow_moments_tex[2]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};

    // GBuffer RG handles for render graph dependency tracking
    rendergraph::RGResourceHandle gbuffer_albedo_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;
    rendergraph::RGResourceHandle gbuffer_orm_rg;
    rendergraph::RGResourceHandle gbuffer_depth_rg;
};

struct DeferredLightingOutputs {
    rendergraph::RGResourceHandle deferred_output_rg;
    rhi::ResourceHandle deferred_output_tex;
};

class DeferredLightingModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle vertex_shader, rhi::ShaderHandle pixel_shader,
                    u32 render_width, u32 render_height);
    void Shutdown();

    DeferredLightingOutputs AddPasses(rendergraph::RenderGraph& graph, const DeferredLightingInputs& inputs);

    rhi::ResourceHandle GetOutputTexture(u32 buffer_index) const;

    // T4.6.5 part 37: IBL resources (Tier 5 visual fidelity). Call after
    // Initialize. Pass INVALID_RESOURCE to disable IBL (shader falls back to
    // flat ambient). irradiance/prefilter are cube maps; brdfLUT is 2D.
    void SetIBLResources(rhi::ResourceHandle irradiance,
                         rhi::ResourceHandle prefilter,
                         rhi::ResourceHandle brdfLUT);

    rhi::ResourceHandle GetViewCB(u32 idx) const { return idx < 3 ? view_cb_[idx] : rhi::handles::INVALID_RESOURCE; }
    rhi::ResourceHandle GetSceneCB(u32 idx) const { return idx < 3 ? scene_cb_[idx] : rhi::handles::INVALID_RESOURCE; }
    rhi::DescriptorSetHandle GetDescriptorSet(u32 idx) const { return idx < 3 ? descriptor_sets_[idx] : rhi::handles::INVALID_DESCRIPTOR_SET; }
    rhi::PipelineHandle GetPipeline() const { return pipeline_; }
    rhi::PipelineLayoutHandle GetPipelineLayout() const { return layout_; }
    rhi::SamplerHandle GetSampler() const { return sampler_; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };

    rhi::ResourceHandle output_textures_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle view_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle scene_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    rhi::SamplerHandle sampler_{rhi::handles::INVALID_SAMPLER};

    // T4.6.5 part 24.4 (B2 fix): 1x1 fallback texture for invalid bindings
    // (SSAO/shadow_visibility when those features aren't enabled). Vulkan
    // validation rejects VK_NULL_HANDLE imageView without nullDescriptor
    // feature — use a real texture as a safe fallback.
    rhi::ResourceHandle fallback_tex_{rhi::handles::INVALID_RESOURCE};

    // T4.6.5 part 37: IBL resources (Tier 5 visual fidelity).
    // Set via SetIBLResources before first AddPasses call.
    rhi::ResourceHandle ibl_irradiance_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_prefilter_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_brdf_lut_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
