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

    bool measured_environment_visibility = false;

    // sun-audit P2 第二步: DDGI 探针深度可见性（PRIMAL_PROBE_OCC 变体）。
    // probe_depth_buf = 128 floats/probe 的 SSBO（64 mean + 64 variance，
    // 8×8 octahedral）；origin/spacing/counts 来自 DDGIVolumeData。句柄
    // 无效时模块绑定 1-float dummy 且 params.counts=0（shader 早退）。
    rhi::ResourceHandle probe_depth_buf{rhi::handles::INVALID_RESOURCE};
    math::v4 probe_origin_spacing{0.0f, 0.0f, 0.0f, 3.0f};  // xyz=origin, w=spacing
    math::v4 probe_counts{0.0f, 0.0f, 0.0f, 0.0f};          // xyz=counts, w=total

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v3 camera_position{};
    math::v4 light_pos{};
    math::v4 light_color{20.0f, 20.0f, 20.0f, 1.0f};
    math::v4 cascade_splits{600.0f, 2000.0f, 0.0f, 0.0f};
    math::m4x4 shadow_matrix0{};
    math::m4x4 shadow_matrix1{};
    math::m4x4 shadow_matrix2{};   // P4.2: 级联 2
    math::m4x4 shadow_matrix3{};   // P4.2: 级联 3

    u32 current_buffer_index = 0;

    // From ShadowMapModule outputs
    rendergraph::RGResourceHandle shadow_visibility_rg;
    rhi::ResourceHandle shadow_visibility_tex;
    rendergraph::RGResourceHandle shadow_translucent_rg;   // P4.1
    rhi::ResourceHandle shadow_translucent_tex;            // P4.1

    // VSM path (ShadowMapModule vsm_enabled): blurred RG32 moments per
    // cascade — shader samples + Chebyshev upper bound instead of the
    // pre-filtered R8 visibility. shadow_moments_tex[] must be valid when
    // vsm_enabled is set, otherwise binding 6 (R8) is used.
    bool vsm_enabled = false;
    rendergraph::RGResourceHandle shadow_moments_rg[2];
    rhi::ResourceHandle shadow_moments_tex[4]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};

    // GBuffer RG handles for render graph dependency tracking
    rendergraph::RGResourceHandle gbuffer_albedo_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;
    rendergraph::RGResourceHandle gbuffer_orm_rg;
    rendergraph::RGResourceHandle gbuffer_depth_rg;
};

struct DeferredLightingOutputs {
    rendergraph::RGResourceHandle deferred_output_rg;
    rhi::ResourceHandle deferred_output_tex{rhi::handles::INVALID_RESOURCE};
    // Display-only environment reflection. Never use as screen-space GI radiance.
    rendergraph::RGResourceHandle environment_specular_rg{};
    rhi::ResourceHandle environment_specular_tex{rhi::handles::INVALID_RESOURCE};
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
    rhi::ResourceHandle environment_textures_[3]{
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
    // sun-audit P2 第二步: 探针参数 UBO（三缓冲）+ dummy 探针深度 SSBO
    rhi::ResourceHandle probe_param_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle probe_depth_dummy_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle sampler_{rhi::handles::INVALID_SAMPLER};

    // T4.6.5 part 24.4 (B2 fix): 1x1 fallback texture for invalid bindings
    // (SSAO/shadow_visibility when those features aren't enabled). Vulkan
    // validation rejects VK_NULL_HANDLE imageView without nullDescriptor
    // feature — use a real texture as a safe fallback.
    rhi::ResourceHandle fallback_tex_{rhi::handles::INVALID_RESOURCE};

    // R7-WIN(07752): IBL 不可用时 bindings 10/11(cube 槽)的占位 cube
    // (1x1x6 RGBA8,TextureType::TextureCube)。旧实现把 2D fallback_tex_
    // 绑进 cube 槽,shader OpTypeImage Dim=Cube 与 TYPE_2D 视图不符触发
    // VUID-vkCmdDraw-viewType-07752。IBL 有效时(Mac 正常路径)不参与绑定。
    rhi::ResourceHandle fallback_cube_tex_{rhi::handles::INVALID_RESOURCE};

    // T4.6.5 part 37: IBL resources (Tier 5 visual fidelity).
    // Set via SetIBLResources before first AddPasses call.
    rhi::ResourceHandle ibl_irradiance_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_prefilter_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ibl_brdf_lut_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
