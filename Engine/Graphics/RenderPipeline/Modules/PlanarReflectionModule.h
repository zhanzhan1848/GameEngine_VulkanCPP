#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

#include <memory>

namespace primal::graphics {

class RenderSceneSnapshot;
namespace nanite { class GPUDrivenDrawPipeline; }
namespace rendergraph { class RenderGraph; }

// One planar reflection plane. The reflection is rendered from the mirrored
// camera (Householder reflection of the view matrix — same math as
// ForwardRenderer::RenderReflections) and composited over the HDR frame
// before TAA with a view-angle Fresnel blend.
struct PlanarReflectionPlane {
    math::v3 position{0.0f, 0.02f, 0.0f};
    math::v3 normal{0.0f, 1.0f, 0.0f};
    math::v2 half_extents{5.0f, 5.0f};
    float reflectivity = 0.85f;
};

struct PlanarReflectionInputs {
    RenderSceneSnapshot* scene_snapshot = nullptr;
    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v3 camera_position{};
    math::v4 light_dir{0.0f, 1.0f, 0.0f, 0.0f};   // TO light
    math::v4 light_color{20.0f, 20.0f, 20.0f, 1.0f};
    math::v4 ambient{0.03f, 0.03f, 0.035f, 1.0f};
    u32 current_buffer_index = 0;
};

class PlanarReflectionModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, nanite::GPUDrivenDrawPipeline* gpu_draw,
                    u32 max_clusters);
    void Shutdown();

    void SetPlane(const PlanarReflectionPlane& plane) { plane_ = plane; }
    const PlanarReflectionPlane& GetPlane() const { return plane_; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_; }

    // Adds two passes:
    //   1. ReflectionRender — renders the scene from the mirrored camera
    //      into this module's 1024² RGBA16F + D32 (SideEffect pass; the RTs
    //      are module-internal).
    //   2. MirrorComposite  — draws the mirror quad over hdrColor (Load)
    //      sampling the reflection RT (SrcAlpha blend).
    // Call AFTER the HDR composite exists and BEFORE TAA.
    void AddPasses(rendergraph::RenderGraph& graph,
                   rendergraph::RGResourceHandle hdrColorRG,
                   const PlanarReflectionInputs& inputs);

    rhi::ResourceHandle GetReflectionTexture() const { return reflection_rt_; }
    bool IsInitialized() const { return initialized_; }

private:
    bool LoadCompositeShaders();

    rhi::RHIDeviceBase* device_ = nullptr;
    nanite::GPUDrivenDrawPipeline* gpu_draw_ = nullptr;
    bool initialized_ = false;
    // Off by default — reflection is a demo feature (F11). Long-term the
    // reflection texture should be sampled by the mirror surface's MATERIAL
    // (Metal's ForwardRenderer binds it at material texture slot 2) rather
    // than a scene-level composite quad.
    bool enabled_ = false;
    PlanarReflectionPlane plane_{};

    // Reflection RTs (1024²)
    rhi::ResourceHandle reflection_rt_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle reflection_depth_{rhi::handles::INVALID_RESOURCE};

    // Composite pipeline (mirror quad → HDR)
    rhi::PipelineHandle composite_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle composite_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle composite_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle composite_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle composite_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle composite_sampler_{rhi::handles::INVALID_SAMPLER};
};

} // namespace primal::graphics
