#pragma once
#include "DDGITraversalAudit.h"
#include "Graphics/Utilities/ComputeWorkgroupExperiment.h"
#include "InstanceNormalCache.h"
#include "InstanceNormalCachePolicy.h"

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Lumen/GICacheView.h"
#include <functional>

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class StaticProbeVolume; }

struct GIGatherInputs {
    // T6.3 sub-B: 全部 DDGI volume 视图（上限 4，与 ddgi_volumes_ 容量一致）。
    // valid 数 ≥ 2 且设备支持 descriptor indexing 时，GIGatherModule 走 N-volume
    // gather 路径（binding 17 SSBO 数组 + binding 18 参数 CB）；否则回退
    // near+far 两槽路径（行为与 T4.4/T5.5 一致）。volume_views[0] 缺省时模块
    // 用 ddgi_pass 视图补齐，[1] 用 far_volume_view/ddgi_far_pass——现有调用
    // 点不填本字段也完全兼容（零行为变化）。
    static constexpr u32 kMaxVolumes = 4;
    lumen::DDGIVolumeView volume_views[kMaxVolumes]{};
    u32 volume_count = 0;

    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::LumenDDGIPass* ddgi_far_pass = nullptr;  // T4.4 far-field fallback
    lumen::StaticProbeVolume* static_probe_volume = nullptr;

    // X2/A3: 远场 volume 数据视图（可选）。有效时远场 CB 上传与 binding 16
    // 的 irradiance 句柄优先从 view 取数；无效时回退 ddgi_far_pass 指针路径
    // （两条路径字段来源相同，上传逐字节等价）。ddgi_far_pass 仍负责 RG 侧
    // "DDGIFarIrrRead" import 声明，保持 RG 声明零变化。
    lumen::DDGIVolumeView far_volume_view{};

    rhi::ResourceHandle gbuffer_depth;
    rhi::ResourceHandle gbuffer_normal;
    rhi::ResourceHandle gbuffer_orm;      // T4.3: roughness for lobe widening
    rendergraph::RGResourceHandle gbuffer_depth_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;
    rendergraph::RGResourceHandle gbuffer_orm_rg;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    u32 current_buffer_index = 0;
    // Irradiance triple-buffer slot source. DDGI/SC-DDGI writers rotate on
    // the frame counter (slot = frame_index % 3); current_buffer_index is a
    // swapchain index that can drift out of phase with it. Callers that leave
    // this at the sentinel keep the legacy (cbIdx + 2) % 3 read slot.
    static constexpr u32 kFrameIndexUnset = 0xFFFFFFFFu;
    u32 frame_index = kFrameIndexUnset;
    rhi::ResourceHandle geometry[4]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};
    u64 geometry_sizes[4]{};
    bool geometry_ready=false;
    u64 geometry_epoch=0,geometry_source_digest=0;
    std::shared_ptr<const ray_geometry::InstanceNormalOwnerCertificate> geometry_normal_owners;
    std::function<bool()> geometry_still_current;
    std::function<bool()> geometry_compact_still_current;
    rhi::ResourceHandle equal_box_buffer{rhi::handles::INVALID_RESOURCE};
    u64 equal_box_size=0,equal_box_flags_hash=0,equal_box_payload_hash=0,equal_box_serial=0;
    std::function<bool()> equal_box_still_current;
    rhi::ResourceHandle geometry_upper{rhi::handles::INVALID_RESOURCE};
    u64 geometry_upper_size=0, geometry_upper_original_hash=0, geometry_upper_candidate_hash=0;
    std::function<bool()> geometry_upper_still_current;
    std::function<bool(math::v2&)> raster_jitter_provider;
    rhi::ResourceHandle fallback_texture{rhi::handles::INVALID_RESOURCE};  // when ORM missing
};

struct GIGatherOutputs {
    // UNITS: Irradiance（GIGather 已 ×π——PROBE_SH_RAD_TO_IRRAD；消费端
    // FusionIndirect 自行 ×albedo/π 得 Lo，见审计 §5 四量表）。
    rendergraph::RGResourceHandle gi_output_rg;
    rhi::ResourceHandle gi_output_tex;
    // T4.3: half-res glossy specular (RGB = SH(R) lobe radiance, A = NdotV).
    rendergraph::RGResourceHandle gi_specular_rg;
    rhi::ResourceHandle gi_specular_tex;
};

class GIGatherModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle shader,
                    u32 render_width, u32 render_height,
                    rhi::ShaderHandle glossy_source_audit_shader = rhi::handles::INVALID_SHADER);
    void Shutdown();

    GIGatherOutputs AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs);

    rhi::ResourceHandle GetOutputTexture() const { return gi_halfres_texture_; }
    // gather 实际输出分辨率（Metal 全分辨率，Vulkan 维持半分辨率基线）。
    u32 GetGatherWidth() const { return gather_width_; }
    u32 GetGatherHeight() const { return gather_height_; }
    bool RequiresGeometryConnections() const { return geometry_connections_enabled_; }
    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }
    void SetGeometryCompactTraversalEnabled(bool enabled) { compact_requested_=enabled && compact_pipeline_!=rhi::handles::INVALID_PIPELINE; compact_used_=false; }
    bool WasCompactGeometryUsed() const { return compact_used_; }
    void ResetGeometryCompactTraversalUsed() { compact_used_=false; }
    void SetGeometryUpperBVHEnabled(bool enabled) { upper_requested_=enabled && upper_allowed_; upper_used_=false; }
    bool WasUpperBVHUsed() const { return upper_used_; }
    void ResetGeometryUpperBVHUsed() { upper_used_=false; }
    void SetGeometryQueryKernelEnabled(bool enabled) {
        query_requested_=enabled && query_pipeline_!=rhi::handles::INVALID_PIPELINE; query_used_=false;
        if(!query_requested_)SetInstanceNormalCacheEnabled(false);
    }
    void SetInstanceNormalCacheEnabled(bool enabled){
        normal_cache_requested_=enabled&&normal_cache_enabled_&&InstanceNormalCacheQueryReady(
            query_requested_,query_pipeline_!=rhi::handles::INVALID_PIPELINE);
        normal_cache_used_=false;
    }
    bool IsInstanceNormalCacheRequested()const{return normal_cache_requested_;}
    bool WasInstanceNormalCacheUsed()const{return normal_cache_used_;}
    void SetGeometryBoundsFirstEnabled(bool enabled) { bounds_requested_=enabled && bounds_pipeline_!=rhi::handles::INVALID_PIPELINE; bounds_used_=false; }
    bool IsGeometryBoundsFirstRequested() const { return bounds_requested_; }
    bool WasGeometryBoundsFirstUsed() const { return bounds_used_; }
    void ResetGeometryBoundsFirstUsed() { bounds_used_=false; bounds_dispatch_=false; }
    u64 GeometryBoundsFirstActiveShaderHash() const { return bounds_dispatch_ ? (pair_receipt_.actual ? pair_shader_hash_ : (bounds_used_ ? bounds_shader_hash_ : (payload_receipt_.actual ? payload_shader_hash_ : (equal_box_receipt_.actual ? equal_box_shader_hash_ : (workgroup_receipt_.actual ? workgroup_shader_hash_ : query_shader_hash_))))) : 0; }
    // True when the dispatched shader includes query-kernel optimization, including bounds-first.
    bool WasGeometryQueryKernelUsed() const { return query_used_; }
    void ResetGeometryQueryKernelUsed() { query_used_=false; query_dispatch_=false; }
    bool IsGeometryQueryKernelRequested() const { return query_requested_; }
    u64 GeometryQueryKernelActiveShaderHash() const { return query_dispatch_ ? (pair_receipt_.actual ? pair_shader_hash_ : (bounds_used_ ? bounds_shader_hash_ : (payload_receipt_.actual ? payload_shader_hash_ : (equal_box_receipt_.actual ? equal_box_shader_hash_ : (workgroup_receipt_.actual ? workgroup_shader_hash_ : (query_used_ ? query_shader_hash_ : query_main_shader_hash_)))))) : 0; }

    struct PayloadReceipt {bool requested=false,actual=false;u32 slot=0;u64 epoch=0,digest=0,shader_hash=0,sources[4]{};};
    using PairReceipt=PayloadReceipt;
    const PairReceipt& GetCrossRayPairReceipt() const {return pair_receipt_;}
    void ResetCrossRayPairReceipt() {pair_receipt_={};}
    const PayloadReceipt& GetInstancePayloadReceipt() const {return payload_receipt_;}
    void ResetInstancePayloadReceipt() {payload_receipt_={};}
    struct EqualBoxReceipt {bool requested=false,actual=false;u32 slot=0;u64 epoch=0,digest=0,payload_hash=0,flags_hash=0,buffer=0,bytes=0,serial=0,sources[4]{};};
    const EqualBoxReceipt& GetEqualBoxReceipt() const {return equal_box_receipt_;}
    void ResetEqualBoxReceipt() {equal_box_receipt_={};}
    const workgroup_experiment::Receipt& GetWorkgroupReceipt() const { return workgroup_receipt_; }

private:
    PairReceipt pair_receipt_{};
    bool pair_requested_=false,pair_env_requested_=false;u64 pair_shader_hash_=0;
    rhi::ShaderHandle pair_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle pair_pipeline_{rhi::handles::INVALID_PIPELINE};
    PayloadReceipt payload_receipt_{};
    bool payload_requested_=false,payload_env_requested_=false;
    u64 payload_shader_hash_=0;
    rhi::ShaderHandle payload_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle payload_pipeline_{rhi::handles::INVALID_PIPELINE};
    EqualBoxReceipt equal_box_receipt_{};
    bool equal_box_requested_=false,equal_box_env_requested_=false;
    u64 equal_box_shader_hash_=0;
    rhi::ShaderHandle equal_box_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle equal_box_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle equal_box_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle equal_box_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle equal_box_sets_[3]{rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET,rhi::handles::INVALID_DESCRIPTOR_SET};
    workgroup_experiment::Receipt workgroup_receipt_{};
    u32 workgroup_requested_=0;
    u64 workgroup_shader_hash_=0;
    rhi::ShaderHandle workgroup_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle workgroup_pipeline_{rhi::handles::INVALID_PIPELINE};
    InstanceNormalCache normal_cache_;
    bool normal_cache_enabled_=false,normal_cache_requested_=false,normal_cache_used_=false;
    bool PrepareTraversalAudit(u32 nativeFrame,u32 slot);
    bool traversal_audit_requested_=false, traversal_audit_attempted_=false;
    u32 traversal_audit_frame_=119;
    u64 traversal_shader_hash_=0, traversal_main_shader_hash_=0;
    rhi::ShaderHandle traversal_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle traversal_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle traversal_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle traversal_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle traversal_set_{rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle traversal_cb_{rhi::handles::INVALID_RESOURCE},traversal_records_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle traversal_diffuse_{rhi::handles::INVALID_RESOURCE},traversal_specular_{rhi::handles::INVALID_RESOURCE};
    traversal_audit::Selection traversal_selection_{};
    std::vector<traversal_audit::Pixel> traversal_pixels_;
    bool geometry_connections_enabled_=false;
    bool compact_requested_=false, compact_used_=false;
    bool upper_allowed_=false, upper_requested_=false, upper_used_=false;
    bool bounds_requested_=false, bounds_used_=false, bounds_dispatch_=false;
    u64 bounds_shader_hash_=0;
    rhi::ShaderHandle bounds_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle bounds_pipeline_{rhi::handles::INVALID_PIPELINE};
    bool query_requested_=false, query_used_=false, query_dispatch_=false;
    u64 query_shader_hash_=0, query_main_shader_hash_=0;
    rhi::ShaderHandle query_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle query_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::ShaderHandle compact_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle compact_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::ShaderHandle connection_shader_{rhi::handles::INVALID_SHADER};
    rhi::ResourceHandle connection_cb_[3]{rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE,rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle connection_unknown_cb_{rhi::handles::INVALID_RESOURCE};
    struct GatherAuditSource {
        rhi::ResourceHandle staging{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle resource{rhi::handles::INVALID_RESOURCE};
        const char* name{nullptr};
        const char* format{"f32"};
        const char* layout{"unknown"};
        const char* source_identity{""};
        u32 binding{0xFFFFFFFFu};
        u32 runtime_format{0};
        u64 bytes{0};
        u32 width{0}, height{0};
    };
    GatherAuditSource gather_audit_sources_[15]{};
    u32 gather_audit_source_count_{10};
    u32 gather_audit_ddgi_frame_{0},gather_audit_ray_mode_{0},gather_audit_rays_{0};
    u32 gather_audit_frame_{0}, gather_audit_slot_{0};
    bool gather_audit_queued_{false}, gather_audit_dumped_{false};
    // The material audit is correlated with the imageView actually written to
    // descriptor binding 13 (including the existing fallback chain).  Keep
    // these values from the captured dispatch for the deferred metadata write.
    rhi::ResourceHandle gather_audit_orm_resource_{rhi::handles::INVALID_RESOURCE};
    rhi::DataFormat gather_audit_orm_runtime_format_{rhi::DataFormat::Unknown};
    u32 gather_audit_orm_width_{0}, gather_audit_orm_height_{0};
    bool gather_audit_orm_metadata_valid_{false};
    bool gather_audit_orm_from_scene_{false};

    // DDGI_GLOSSY_SOURCE_AUDIT=1 is a Vulkan-only diagnostic path.  The
    // regular gather pipeline/layout and its 19 production bindings stay
    // untouched; the source shader is dispatched into shadow textures with an
    // auxiliary binding for the per-pixel source records.
    rhi::PipelineHandle glossy_source_audit_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle glossy_source_audit_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle glossy_source_audit_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle glossy_source_audit_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle glossy_source_audit_buffer_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle glossy_source_audit_output_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle glossy_source_audit_specular_{rhi::handles::INVALID_RESOURCE};
    u64 glossy_source_audit_bytes_{0};
    rhi::ResourceHandle glossy_material_audit_buffer_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 glossy_material_audit_bytes_{0};
    bool glossy_source_audit_enabled_{false};
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    u32 gather_width_ = 0;
    u32 gather_height_ = 0;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    // Rotate CPU-updated bindings with the in-flight command buffers.  This is
    // ordinary resource-lifetime isolation; DDGI probe-state consistency is
    // handled separately by the update/integration passes.
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle gi_halfres_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_halfres_history_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_specular_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle probe_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle far_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};      // T4.4 far volume params
    rhi::ResourceHandle static_probe_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
    // T6.3 sub-B → T3.6/D1: N-volume 参数 CB（144B std140）。原 binding 18
    // NVolumeCB 原位改型为 FlatVolumeCB（probe offset 表）。
    rhi::ResourceHandle nvolumes_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    // T3.6/D1: flat irradiance SSBO（triple-buffered，按需增长）。所有 volume
    // 的 9-vec4/probe 探针数据连续打包：[header 256B][vol0][vol1]...；描述符
    // 绑 offset 256 跳过头部，shader 侧即纯探针数组。取代 Vulkan 的
    // descriptor-indexing SSBO 数组（原 binding 17 array[4]）与双端 legacy
    // near/far 双槽绑定——Metal 与 Vulkan 走同一条 flat 路径。
    rhi::ResourceHandle flat_irr_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 flat_irr_capacity_[3]{0, 0, 0};
    // GI 根治 Phase 3.2: flat openSky 缓冲（256B 头 + probes×4B，同槽位轮转）。
    rhi::ResourceHandle flat_opensky_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 flat_opensky_capacity_[3]{0, 0, 0};   // 已分配字节数（16 头部 + probes×144）

    // Debug (SC_DUMP_CARDS=1): staging copy of the gather output + dump frame
    // (deferred print >=3 frames later — correlates probe health with what
    // Fusion actually receives).
    rhi::ResourceHandle out_staging_{rhi::handles::INVALID_RESOURCE};
    u64  out_staging_size_ = 0;
    u32  out_dump_frame_ = 0xFFFFFFFFu;
};

} // namespace primal::graphics
