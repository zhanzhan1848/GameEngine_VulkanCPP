#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/SceneDataAdapter.h"
#include "Graphics/Passes/ParticlePass.h"
#ifndef DISABLE_PARTICLE_FX
#include "Graphics/Passes/ParticleFXLightPass.h"
#include "Graphics/Passes/ParticleFXMeshPass.h"
#endif
#include "Graphics/RenderPipeline/Modules/LineBatchRenderer.h"
#include "Graphics/DebugDraw/DebugDrawQueue.h"
#include "Graphics/Material/ShaderTechnique.h"
#include "Utilities/Math.h"
#include <unordered_map>
#include <memory>
#include <string>

namespace primal::graphics {

class RenderScene;
namespace rhi { class IBLPrecomputer; }

class ForwardSceneRenderer {
public:
    ForwardSceneRenderer();
    ~ForwardSceneRenderer();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    rhi::DataFormat backbuffer_format = rhi::DataFormat::BGRA8_UNorm);
    void Shutdown();

    // Scene loading — uses SceneDataAdapter internally
    bool LoadScene(const std::string& model_path);
    void UnloadScene();

    // Async texture loading (called each frame until all textures loaded)
    void UpdateAsyncTextures(u32 frame_index);

    // IBL environment setup
    void SetIBLMaps(rhi::ResourceHandle irradiance, rhi::ResourceHandle prefiltered,
                    rhi::ResourceHandle brdf_lut);

    // HDR environment map (equirect .hdr): loads, converts to a cube and
    // bakes the IBL triple (irradiance / prefilter / BRDF LUT). The cube is
    // also used by the skybox pass. Returns false on load/bake failure.
    bool SetEnvironmentMap(const std::string& hdr_path);
    void ClearEnvironmentMap();
    void SetAmbientLight(math::v3 color, float intensity);
    void SetSkyboxEnabled(bool enabled) { skybox_enabled_ = enabled; }
    const std::string& GetEnvironmentMapPath() const { return env_map_path_; }

    // Main render entry point
    void Render(rhi::RHICommandBuffer* cmd,
                const math::m4x4& view_matrix,
                const math::m4x4& proj_matrix,
                const math::v3& camera_position,
                rhi::ResourceHandle backbuffer,
                u32 frame_index,
                rhi::DataFormat backbuffer_format = rhi::DataFormat::BGRA8_UNorm);

    void SetLightDirection(math::v3 dir);
    void SetLightColor(math::v4 color);

    /// VT M2 mesh 路径:启用 VT 材质模式。vtexAssetsDir 为烘焙好的 .vtex
    /// 资产目录(空串 = 关闭)。材质三槽(albedo/normal/orm)的 .vtex 齐备才
    /// 走 GBufferVT 管线,否则回退常规材质。feedback 回读由
    /// AttachVTFeedback 在帧末挂接(调用方 submit 前),系统 update 由
    /// 帧驱动方调用 virtual_texture_system::instance().update(frame)。
    void EnableVirtualTextures(const std::string& vtexAssetsDir);
    /// 帧末调用:把 VT 材质 set 的 feedback 绑定切到本帧槽并记录回读拷贝,
    /// 同时把 ring fence 挂到 si(调用方随后 submit si)。
    void AttachVTFeedback(rhi::RHICommandBuffer* cb, u32 frame_index, rhi::QueueSubmitInfo& si);
    bool IsVirtualTextureEnabled() const { return vt_mode_; }
    /// VT 化的材质数(LoadScene 后统计)
    u32 VTMaterialCount() const {
        u32 n = 0;
        for (bool b : material_is_vt_) if (b) ++n;
        return n;
    }
    ParticlePass* GetParticlePass();

    // ECS bridge: sync entities via SyncEntitiesToRenderScene + RenderDynamicInstances

    // Accessors for ECS bridge (Phase 3c)
    const SceneDataMeshInfo* GetMeshInfo(u32 index) const;
    u32 GetMeshInfoCount() const;

    // RenderScene-based rendering (Phase 3d)
    void SetRenderScene(const RenderScene* scene);
    void SetEditorMode(bool enabled) { editor_mode_ = enabled; }
    void RenderDynamicInstances(rhi::RHICommandBuffer* cmd,
                                const math::m4x4& vp_matrix,
                                u32 frame_index,
                                bool shadow_pass,
                                bool forward_pass = false);

    // Static mesh ECS entity creation (Phase 3e)
    struct StaticEntityResult {
        std::vector<id::id_type> entity_ids;
        std::vector<u32> mesh_slot_indices;
    };
    StaticEntityResult CreateStaticEntities();
    void DestroyStaticEntities();

    // Content system mesh registration (Phase 4)
    // Register a mesh that was imported via content system. Creates RenderMesh + material DS.
    // Returns mesh slot index for mesh_id_to_index_ lookup.
    //
    // entity_id (optional): if non-invalid, the RenderMesh is tagged with
    // this id instead of geometry_content_id. The pipeline uses this to
    // disambiguate per-entity instances of the same geometry asset —
    // mesh_id_to_index_ is keyed by mesh entity id, so 4 entities sharing
    // geometry_content_id="sphereGeom" must each get a unique entityId.
    // Legacy callers that don't care can pass id::invalid_id and the
    // geometry_content_id is used (1 mesh per geometry, like the legacy
    // AddRenderItem path).
    u32 RegisterMeshResource(id::id_type geometry_content_id,
                             id::id_type albedo_texture_id,
                             id::id_type normal_texture_id,
                             id::id_type orm_texture_id,
                             id::id_type entity_id = id::invalid_id);
    void UnregisterMeshResource(u32 slot_index);

    // === Material-graph compiled variants (editor world-material path) ===
    //
    // Compile a material graph (editor-serialized JSON with absolute texture
    // paths) into a GBuffer fragment via ShaderIR, pair it with
    // MaterialGraphVertex.metal, and remember the pipeline/textures as a
    // shared variant keyed by a content hash. Entities reference variants by
    // hash through RenderProxy::graphVariant. Compilation failure returns 0
    // and leaves the entity's previous binding untouched — the caller keeps
    // rendering through the fixed 3-slot material path.
    //
    // v1 limitations (documented in MetalEmitter.h / the generated source):
    // opaque technique only, ≤8 textures per graph, Time nodes evaluate to 0.
    static constexpr u64 kInvalidGraphVariant = 0;
    u64 SetEntityGraphMaterial(id::id_type entity_id, const char* utf8_json);
    void ClearEntityGraphMaterial(id::id_type entity_id);
    u64 GetEntityGraphVariant(id::id_type entity_id) const {
        auto it = entity_graph_variant_.find(entity_id);
        return (it != entity_graph_variant_.end()) ? it->second : kInvalidGraphVariant;
    }

    // Push constant with instance buffer flag
    struct PCGPushConsts {
        math::m4x4 transform;
        u32 use_instances{0};
        u32 _pad[3]{0, 0, 0};
    };

    // Geometry entity line rendering
    void SetGeometryEntities(const std::vector<id::id_type>& entity_ids);
    void ClearGeometryEntities();

private:
    // Initialization sub-methods
    void CreateShaders();
    void CreatePipelines();
    void CreateDescriptorLayouts();
    void CreatePersistentResources();
    void CreateSamplers();

    // Per-mesh material descriptor sets
    void CreateMaterialDescriptorSets();

    // Shadow VP computation
    math::m4x4 ComputeCascadeVP(math::v3 light_dir, math::v3 camera_pos,
                                float ortho_extent, u32 shadow_map_size, u32 cascade,
                                float near_plane = 0.1f, float far_plane = 1000.0f);

    // Render sub-passes
    void RenderShadowPass(rhi::RHICommandBuffer* cmd, u32 cascade, u32 frame_index);
    void RenderGBufferPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderSkyboxPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderLightingPass(rhi::RHICommandBuffer* cmd, u32 frame_index);
    void RenderParticlePass(rhi::RHICommandBuffer* cmd, u32 frame_index,
                            const math::m4x4& view_matrix, const math::m4x4& proj_matrix);
    void RenderBlit(rhi::RHICommandBuffer* cmd, rhi::ResourceHandle backbuffer, u32 frame_index);

    // Streaming mesh pass (Phase 9.3b Task 12) — draws GPU-resident StreamingMesh
    // records (e.g. GlobalSDFMeshNode output) into the GBuffer attachments.
    // Inserts between Pass 3 (GBuffer) and Pass 4 (Deferred Lighting) so that
    // the deferred lighting pass illuminates the streaming surface. No CPU
    // readback; positions/elements live in separate SoA buffers bound to slots
    // 20/21 (see StreamingGBuffer.metal).
    void RenderStreamingMeshes(rhi::RHICommandBuffer* cmd, u32 frame_index);

    // Shader loading helper
    rhi::ShaderHandle LoadShader(const char* filename, const char* entry_point, rhi::ShaderStage stage);

    // Technique → Pipeline mapping
    rhi::PipelineHandle GetTechniquePipeline(ShaderTechnique technique) const;
    rhi::PipelineHandle GetForwardTechniquePipeline(ShaderTechnique technique) const;

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    bool initialized_ = false;

    // Scene data (loaded via SceneDataAdapter)
    SceneDataAdapter scene_adapter_;
    utl::vector<SceneDataMeshInfo> mesh_infos_;
    utl::vector<rhi::ResourceHandle> material_textures_; // albedo, normal, ORM per mesh
    bool scene_loaded_ = false;
    bool editor_mode_ = false;  // when true, render all techniques in forward pass

    // IBL
    rhi::ResourceHandle irradiance_map_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle prefiltered_map_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle brdf_lut_{rhi::handles::INVALID_RESOURCE};
    bool ibl_ready_ = false;
    math::v4 ambient_light_{0.05f, 0.05f, 0.05f, 1.0f};

    // HDR environment (skybox + IBL source)
    rhi::ResourceHandle env_cube_{rhi::handles::INVALID_RESOURCE};
    std::string env_map_path_;
    bool skybox_enabled_ = false;
    std::unique_ptr<rhi::IBLPrecomputer> ibl_precomputer_;

    // Shaders
    rhi::ShaderHandle gbuffer_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle gbuffer_ps_{rhi::handles::INVALID_SHADER};
    // VT M2.14: GBuffer 的 VT 采样变体(vertex 复用 gbuffer_vs_)
    rhi::ShaderHandle gbuffer_vt_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle shadow_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle lighting_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle lighting_ps_{rhi::handles::INVALID_SHADER};
    // T4.6.5 part 4 Path B: Vulkan-only compute shader replacing lighting_vs_+lighting_ps_.
    rhi::ShaderHandle lighting_cs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle skybox_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle skybox_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_ps_{rhi::handles::INVALID_SHADER};

    // AlphaClip technique shaders
    rhi::ShaderHandle alphaclip_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle alphaclip_ps_{rhi::handles::INVALID_SHADER};

    // Unlit technique shaders
    rhi::ShaderHandle unlit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle unlit_ps_{rhi::handles::INVALID_SHADER};

    // Foliage technique shaders
    rhi::ShaderHandle foliage_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle foliage_ps_{rhi::handles::INVALID_SHADER};

    // Water technique shaders
    rhi::ShaderHandle water_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle water_ps_{rhi::handles::INVALID_SHADER};

    // Transparent technique shaders (GBuffer pass — unused for rendering, kept for shadow)
    rhi::ShaderHandle transparent_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle transparent_ps_{rhi::handles::INVALID_SHADER};

    // Forward pass shaders (Water/Transparent rendered after deferred lighting)
    rhi::ShaderHandle forward_water_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle forward_water_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle forward_transparent_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle forward_transparent_ps_{rhi::handles::INVALID_SHADER};

    // Streaming mesh shaders (Phase 9.3b Task 12) — SoA vertex pulling
    rhi::ShaderHandle streaming_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle streaming_ps_{rhi::handles::INVALID_SHADER};

    // Pipelines
    rhi::PipelineHandle gbuffer_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle gbuffer_vt_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle shadow_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle lighting_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle skybox_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle blit_pipeline_{rhi::handles::INVALID_PIPELINE};

    // Technique-specific pipelines
    rhi::PipelineHandle alphaclip_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle unlit_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle foliage_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle water_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle transparent_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle forward_water_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle forward_transparent_pipeline_{rhi::handles::INVALID_PIPELINE};

    // Streaming mesh pipeline — shares gbuffer_layout_, draws into GBuffer RTs
    rhi::PipelineHandle streaming_pipeline_{rhi::handles::INVALID_PIPELINE};
    // Default material DS for streaming meshes (white albedo, flat normal)
    rhi::DescriptorSetHandle streaming_material_ds_{rhi::handles::INVALID_DESCRIPTOR_SET};

    // === Material-graph compiled variants ===
    // One compiled pixel shader + PSO + texture descriptor set per distinct
    // material graph content hash; entities share variants via refcount.
    struct GraphVariant {
        rhi::ShaderHandle pixelShader{rhi::handles::INVALID_SHADER};
        rhi::PipelineHandle pipeline{rhi::handles::INVALID_PIPELINE};
        rhi::DescriptorSetHandle materialSet{rhi::handles::INVALID_DESCRIPTOR_SET};
        utl::vector<id::id_type> texture_ids;   // imported cooked textures, size ≤ 8
        u32 refcount{0};
    };
    rhi::ShaderHandle material_graph_vs_{rhi::handles::INVALID_SHADER};
    rhi::PipelineLayoutHandle graph_material_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle graph_material_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    std::unordered_map<u64, GraphVariant> graph_variants_;
    std::unordered_map<id::id_type, u64> entity_graph_variant_;
    static constexpr u32 kMaxGraphTextures = 8;

    void ReleaseGraphVariant(u64 variant_hash);
    void PurgeGraphMaterials();

    // Pipeline layouts
    rhi::PipelineLayoutHandle gbuffer_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle gbuffer_vt_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle shadow_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle lighting_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    // T4.6.5 part 4 Path B: Vulkan-only compute pipeline layout (12 bindings).
    rhi::PipelineLayoutHandle lighting_compute_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle skybox_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle blit_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle global_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle material_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    // VT M2.14: VT 材质 set(页表 + 3 atlas + 采样器 + 参数 UBO + feedback)
    rhi::DescriptorSetLayoutHandle material_set_vt_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    // VT M2 mesh 路径状态
    bool vt_mode_{false};
    std::string vt_assets_dir_;
    rhi::ResourceHandle vt_material_params_buf_{rhi::handles::INVALID_RESOURCE};
    std::vector<bool> material_is_vt_;
    std::vector<rhi::DescriptorSetHandle> vt_material_ds_;
    std::unordered_map<std::string, u32> vt_registered_;   // vtex 路径 → vtId
    // 尝试为 mesh 创建 VT 材质(三槽 .vtex 齐备才成功;失败走常规)
    bool TryCreateVTMaterial(const SceneDataMeshInfo& meshInfo, u32 meshIdx,
                             const std::string& assetBaseDir);
    rhi::DescriptorSetLayoutHandle lighting_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    // T4.6.5 part 4 Path B: Vulkan-only compute descriptor set layout matching
    // Engine/Graphics/Vulkan/shaders/DeferredLighting.spv (12 bindings).
    rhi::DescriptorSetLayoutHandle lighting_compute_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle skybox_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle blit_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};

    // Descriptor sets (triple-buffered)
    rhi::DescriptorSetHandle global_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    // T4.6.5 part 15.3: Vulkan-only shadow-pass descriptor sets. Replaces Metal's
    // BindVertexBuffers(0, 1, &shadow_view_cb_[c], ...) per-vertex slot override.
    // One per cascade: binding 0 = shadow_view_cb_[c], bindings 1+2 mirror global_ds_.
    rhi::DescriptorSetHandle shadow_global_ds_[2]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle lighting_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    // T4.6.5 part 4 Path B: Vulkan-only compute descriptor sets (12 bindings).
    rhi::DescriptorSetHandle lighting_compute_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    // T4.6.5 part 6 Path B: Vulkan-only UBOs for compute lighting pass.
    // GlobalShaderData (480B) and ForwardLightBuffer (25808B) match the engine
    // UBO layout that existing DeferredLighting.spv expects at bindings 9 and 10.
    rhi::ResourceHandle lighting_global_ubos_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle lighting_light_ubos_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    void* lighting_global_mapped_[3]{nullptr, nullptr, nullptr};
    void* lighting_light_mapped_[3]{nullptr, nullptr, nullptr};
    rhi::DescriptorSetHandle skybox_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::DescriptorSetHandle blit_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    // Per-mesh material descriptor sets
    utl::vector<rhi::DescriptorSetHandle> material_ds_;

    // Persistent GPU resources
    rhi::ResourceHandle shadow_map_[2]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_albedo_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                            rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_normal_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                            rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_orm_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                         rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_velocity_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                              rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_depth_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                           rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle lighting_output_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                             rhi::handles::INVALID_RESOURCE};

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle view_cb_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                     rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle scene_cb_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                      rhi::handles::INVALID_RESOURCE};

    // GPU light array (all scene lights) — consumed by fragmentLighting_v3
    // via global set binding 2. Layout mirrors rhi::ForwardLightBuffer.
    rhi::ResourceHandle light_cb_[3]{rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
                                      rhi::handles::INVALID_RESOURCE};

    // Backbuffer format that drives the blit pipeline's renderTargetFormats[0].
    // If the user target changes format between frames (e.g., RGBA16F in headless
    // tests vs BGRA8 for Avalonia MTKView), the blit pipeline is recreated lazily.
    rhi::DataFormat blit_target_format_{rhi::DataFormat::BGRA8_UNorm};

    // Shadow VP constant buffers (one per cascade, avoids shared-memory timing issue)
    rhi::ResourceHandle shadow_view_cb_[2]{rhi::handles::INVALID_RESOURCE,
                                            rhi::handles::INVALID_RESOURCE};

    // Samplers
    rhi::SamplerHandle default_sampler_{rhi::handles::INVALID_SAMPLER};
    rhi::SamplerHandle brdf_sampler_{rhi::handles::INVALID_SAMPLER};

    // Particle pass (owned)
    ParticlePass particle_pass_;
#ifndef DISABLE_PARTICLE_FX
    ParticleFXLightPass particle_fx_light_pass_;
    ParticleFXMeshPass particle_fx_mesh_pass_;
    // 渲染 instance 几何槽缓存：emitter 的 instance_mesh_path → ParticleFXMeshPass 槽号
    //（0 = 内置立方体回退；加载失败也缓存，避免每帧重试刷盘）。
    std::unordered_map<std::string, u32> pfx_instance_geometry_slots_;
#endif

    // Lighting state
    math::v3 light_dir_{-0.9f, 1.5f, -0.8f};
    math::v4 light_color_{20.0f, 20.0f, 20.0f, 1.0f};

    // Shadow VP caching
    math::m4x4 cached_shadow_vp_[2]{};

    // Fallback textures
    rhi::ResourceHandle white_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle flat_normal_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle black_cube_texture_{rhi::handles::INVALID_RESOURCE};

    // PCG instances (CPU-side) — removed, now uses Entity→RenderScene path

    // Instance buffer for instanced rendering (used by RenderDynamicInstances)
    rhi::ResourceHandle pcg_instance_buffer_{rhi::handles::INVALID_RESOURCE};
    u32 pcg_instance_buffer_capacity_{0};

    // mesh_id_to_index_: maps RenderMesh::GetEntityId() → mesh_infos_ array index
    std::unordered_map<id::id_type, u32> mesh_id_to_index_;

    // External RenderScene (for dynamic instances via ECS bridge)
    const RenderScene* render_scene_{nullptr};

    // Geometry entity line rendering
    LineBatchRenderer line_renderer_;
    std::vector<id::id_type> geometry_entity_ids_;

    // Frame-local staging for debug draw lines drained from debug_draw::queue().
    // Reused across frames to avoid reallocations.
    std::vector<debug_draw::DebugLine> frame_local_debug_lines_;
};

} // namespace primal::graphics
