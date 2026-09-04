#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

/// Inputs for the real-mesh DDGI probe sphere overlay.
struct DebugProbeSphereInputs {
    rendergraph::RGResourceHandle backbuffer_rg;   // fresh alias of the swapchain image (WRITE target)
    rendergraph::RGResourceHandle blit_output_rg;  // FinalBlit's output node — declared READ to serialize after it
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v4   camera_position{ 0, 0, 0, 0 };
    math::v4   sun_dir{ 0, 1, 0, 0 };              // toward light, normalized

    // Probe grid definition (instance transforms are built from this — the
    // same definition the trace chain uses; no per-frame derivation).
    math::v3   probe_origin{ 0, 0, 0 };
    u32 probe_counts[3] = {0, 0, 0};
    u32 probe_count = 0;
    f32  probe_spacing = 3.0f;
    f32  sphere_radius = 0.0f;   // 0 → spacing * 0.4

    // B1 L3.2: DDGI SH9 irradiance buffer (probeIdx×9 float4, triple-buffered
    // triple index = current_buffer_index). When valid, spheres sample real
    // probe irradiance by direction instead of the hue fallback.
    rhi::ResourceHandle irradiance_buffer{ rhi::handles::INVALID_RESOURCE };
};

/// Renders one real mesh sphere per DDGI probe on top of the tonemapped
/// scene: shared icosphere VB/IB + per-instance transform matrices, standard
/// vertex/fragment path, NO GPU culling (all probes draw), no descriptors
/// (view_proj/sun via push constants) — the exact shape of the engine's
/// proven instanced-mesh paths.
class DebugProbeSphereModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle vertex_shader,
                    rhi::ShaderHandle pixel_shader);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const DebugProbeSphereInputs& inputs);

    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }
    /// True when the SH9 irradiance-sampling pixel pipeline compiled (Metal).
    bool HasIrradiancePipeline() const { return irradiance_pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    bool BuildMeshAndInstances(const math::v3& origin, const u32 counts[3],
                               u32 count, f32 spacing, f32 radius);

    rhi::RHIDeviceBase* device_ = nullptr;

    rhi::PipelineHandle pipeline_{ rhi::handles::INVALID_PIPELINE };
    // B1 L3.2: same VS, irradiance-sampling FS. INVALID on Vulkan (no spv yet)
    // and when the MSL entry fails — then the hue pipeline is used.
    rhi::PipelineHandle irradiance_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle ubo_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };

    // Expanded flat vertex stream: all probes' triangles baked out
    // (pos+normal+probeIdx per vertex). No instancing anywhere.
    rhi::ResourceHandle sphere_vb_{ rhi::handles::INVALID_RESOURCE };
    u32 instance_count_ = 0;   // total expanded vertex count
    math::v3 instances_origin_{};   // grid params the buffer was built for
    f32 instances_spacing_ = 0.0f;
    f32 instances_radius_ = 0.0f;
};

} // namespace primal::graphics
