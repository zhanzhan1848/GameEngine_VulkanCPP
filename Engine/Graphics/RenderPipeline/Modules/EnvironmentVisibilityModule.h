#pragma once

#include <functional>

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Utilities/GeometryFirstHit.h"
#include "Graphics/Utilities/EnvironmentHybridCertificate.h"
#include "ExperimentalSAHState.h"
#include "Graphics/Utilities/ComputeWorkgroupExperiment.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct EnvironmentVisibilityInputs {
    rhi::ResourceHandle depth{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle normal{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle orm{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle depth_rg{}, normal_rg{}, orm_rg{};
    // SC TLAS, BLAS, triangles, instances; caller certifies complete current
    // scene epoch and supported opaque occluders before setting ready.
    rhi::ResourceHandle geometry[4]{rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle geometry_rg[4]{};
    bool geometry_ready = false;
    environment_hybrid::Binding hybrid_binding{};
    std::span<const u8> hybrid_field{};
    std::function<bool()> hybrid_still_current;
    // Optional live provenance check, evaluated separately at each dispatch.
    // Failure forces unsupported output even when a private slot was uploaded.
    std::function<bool()> geometry_still_current;
    bool legacy_unoccluded = false; // explicit A/B only, never implicit fallback
    // Inverse of the UNJITTERED projection*view. Raster applies this original
    // pre-Vulkan-Y-flip clip/NDC offset; reconstruct NDC minus that offset.
    math::m4x4 inverse_view_projection{};
    // Unjittered previous-frame proj*view, used only by the opt-in temporal
    // single-ray mode to reproject accumulated visibility.
    math::m4x4 prev_view_projection{};
    bool temporal_accumulation = false;
    math::v2 raster_jitter_ndc{0.0f,0.0f};
    bool raster_jitter_valid = true; // explicit zero-jitter is valid
    // Evaluated after the canonical GBuffer producer, never at graph build.
    // False means missing/mismatched actual raster frame and fails closed.
    std::function<bool(math::v2&)> raster_jitter_provider;
    math::v3 camera_position{};
    u32 current_buffer_index = 0;
    u32 frame_index = 0;
};

struct EnvironmentVisibilityOutputs {
    rendergraph::RGResourceHandle visibility_rg{}, half_visibility_rg{};
    rhi::ResourceHandle visibility_tex{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle half_visibility_tex{rhi::handles::INVALID_RESOURCE};
    // RGBA16F: R visibility (unknown contributes zero), G known lobe fraction,
    // B quality (0 half-lobe,1 unsupported,2 mirror fallback,3 unknown fallback),
    // A valid receiver. Valid occluded black stays black.
};

// Vulkan-only BRDF-lobe geometry visibility. Four VNDF samples at half res,
// one reflection ray for near mirrors; qualified full-res nearest resolve.
// This scalar multiplied by prefiltered IBL is a separability approximation,
// not an exact directional integration of the environment radiance.
class EnvironmentVisibilityModule {
public:
    EnvironmentVisibilityModule();
    bool SetGeometryScene(std::span<const ray_geometry::InstanceInput> instances);
    // Explicit memory-only SAH experiment, scoped to a certified SC epoch.
    bool SetExperimentalGeometryScene(std::span<const ray_geometry::InstanceInput> instances,
                                      u64 source_digest, u64 scene_epoch);
    bool HasExperimentalGeometryScene(u64 source_digest, u64 scene_epoch) const;
    // source_width/height are the GBuffer texel size (render target); the
    // target_* pair is the resolve output size, which is derived from the
    // logical viewport so a raster-only render_scale does not multiply the GI
    // ray budget. Trace runs at ceil(target/2) and is upsampled by the resolve.
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle trace_shader,
                    rhi::ShaderHandle resolve_shader, u32 source_width, u32 source_height,
                    u32 target_width, u32 target_height);
    void Shutdown();
    EnvironmentVisibilityOutputs AddPasses(rendergraph::RenderGraph& graph,
                                           const EnvironmentVisibilityInputs& inputs);
    struct HybridReceipt {bool requested=false,qualified=false,actual=false,dispatched=false;u32 slot=0;u64 shader=0,field_hash=0,field_buffer=0,frame_completion=0;environment_hybrid::Binding binding{};};
    const HybridReceipt& GetHybridReceipt() const {return hybrid_receipt_;}
    const workgroup_experiment::Receipt& GetPairReceipt() const {return pair_receipt_;}
    void ResetPairReceipt() {pair_receipt_={};hybrid_receipt_={};pair_policy0_=pair_policy1_=0;}
    void SetFrameAuditTraceHash(u64 hash) {frame_audit_trace_hash_=hash;}
    u32 GetPairPolicy0() const {return pair_policy0_;}
    u32 GetPairPolicy1() const {return pair_policy1_;}
    const workgroup_experiment::Receipt& GetWorkgroupReceipt() const { return workgroup_receipt_; }
    bool IsInitialized() const { return pipelines_[0] != rhi::handles::INVALID_PIPELINE &&
        pipelines_[1] != rhi::handles::INVALID_PIPELINE; }
private:
    HybridReceipt hybrid_receipt_{};bool hybrid_requested_=false,hybrid_env_requested_=false;u64 hybrid_shader_hash_=0;
    rhi::ShaderHandle hybrid_shader_{rhi::handles::INVALID_SHADER};rhi::PipelineHandle hybrid_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::DescriptorSetLayoutHandle hybrid_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};rhi::PipelineLayoutHandle hybrid_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetHandle hybrid_sets_[3]{rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle hybrid_buffers_[3]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};u64 hybrid_serial_[3]{};
    workgroup_experiment::Receipt pair_receipt_{};
    bool frame_audit_enabled_=false;u64 frame_audit_trace_hash_=0,frame_audit_workgroup_hash_=0;u32 pair_policy0_=0,pair_policy1_=0;
    bool pair_requested_=false,pair_env_requested_=false;u64 pair_shader_hash_=0;
    rhi::ShaderHandle pair_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle pair_pipeline_{rhi::handles::INVALID_PIPELINE};
    workgroup_experiment::Receipt workgroup_receipt_{};
    u32 workgroup_requested_=0;
    rhi::ShaderHandle workgroup_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle workgroup_pipeline_{rhi::handles::INVALID_PIPELINE};
    bool UploadOwnGeometry(u32 slot);
    struct GeometrySlot {
        rhi::ResourceHandle buffers[4]{};
        u64 sizes[4]{};
        u64 epoch = ~u64(0);
        bool ready = false;
    };
    ray_geometry::SceneBVH geometry_scene_{};
    u64 geometry_epoch_ = 0;
    u64 experimental_source_digest_ = 0, experimental_scene_epoch_ = 0;
    experimental_sah::FailureMemo experimental_failure_{};
    u64 experimental_reported_epochs_[2][3]{};
    int experimental_reported_states_[2][3]{{-1,-1,-1},{-1,-1,-1}};
    u64 experimental_consumed_serials_[3]{};
    GeometrySlot own_geometry_[3]{};
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 width_ = 0, height_ = 0;             // GBuffer source size
    u32 target_width_ = 0, target_height_ = 0; // resolve output size
    rhi::PipelineHandle pipelines_[2]{rhi::handles::INVALID_PIPELINE, rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle sets_[2][3]{};
    rhi::ResourceHandle cb_[2][3]{};
    rhi::ResourceHandle half_[3]{}, full_[3]{};
    rhi::ResourceHandle unsupported_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_geometry_{rhi::handles::INVALID_RESOURCE};
};
} // namespace primal::graphics
