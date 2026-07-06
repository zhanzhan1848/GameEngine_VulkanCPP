#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/RHI/Utils/ShadowUtils.h"
#include "Graphics/Passes/BlurPass.h"
#include "Graphics/Passes/SSRPass.h"
#include "Graphics/Material.h"
#ifndef DISABLE_PARTICLE_SYSTEM
#include "Graphics/Passes/ParticlePass.h"
#endif
#include "Graphics/Scene/SceneExtractionSystem.h"
#include "RenderPipeline/RenderPasses/Debug/GeometryDebugPass.h"
#include <unordered_map>

namespace primal::graphics {

class RenderScene;
class RenderView;
struct RenderProxy;
class MaterialInstance;

// Definition of Binding Slots
constexpr u32 PER_OBJECT_BINDING = 10;
constexpr u32 FRAME_DATA_BINDING = 11;
constexpr u32 LIGHT_DATA_BINDING = 12;
constexpr u32 SHADOW_MAP_BINDING = 13;
constexpr u32 SHADOW_CUBE_MAP_BINDING = 14;
constexpr u32 IBL_IRRADIANCE_BINDING = 15;
constexpr u32 IBL_PREFILTER_BINDING = 16;
constexpr u32 IBL_BRDF_LUT_BINDING = 17;
constexpr u32 IBL_SAMPLER_BINDING = 18;

class ForwardRenderer {
public:
    ForwardRenderer();
    ~ForwardRenderer();

    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();

    void Render(rhi::RHICommandBuffer* cmdBuffer,
                const RenderScene& scene,
                const RenderView& view,
                rhi::ResourceHandle renderTarget,
                rhi::ResourceHandle velocityTarget,
                rhi::ResourceHandle depthStencil,
                const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                u32 frameIndex,
                u32 width,
                u32 height);

    // Non-jittered depth prepass — writes camera-view depth to the supplied texture
    // without applying TAA jitter. Call BEFORE Render so downstream RG passes
    // (HZB/SSR/SSAO) that sample this depth see stable, non-jittered geometry.
    void RenderDawnDepthPrepass(rhi::RHICommandBuffer* cmdBuffer,
                                const RenderView& view,
                                rhi::ResourceHandle depthTexture,
                                u32 frameIndex,
                                u32 width,
                                u32 height);

    // Phase 3b — G-Buffer pass for Deferred mode. Writes 5 MRT G-Buffer textures
    // (RT0=WorldPos RGBA16F, RT1=Normal+linearDepth RGBA16F, RT2=Albedo+Metallic RGBA8_sRGB,
    // RT3=ORM RGBA8, RT4=Velocity RG16F). Depth is loaded from the supplied depth texture
    // (assumed populated by RenderDawnDepthPrepass).
    // Current implementation (3b-1): clear-only stub that verifies the 5-attachment MRT
    // pipeline works in Dawn. Geometry rendering comes in 3b-2.
    static constexpr u32 DAWN_GBUFFER_RT_COUNT = 5;
    void RenderDawnGBuffer(rhi::RHICommandBuffer* cmdBuffer,
                           const RenderView& view,
                           const rhi::ResourceHandle gbufferTextures[DAWN_GBUFFER_RT_COUNT],
                           rhi::ResourceHandle depthTexture,
                           const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                           u32 frameIndex,
                           u32 width,
                           u32 height);

    // Phase 3c — Deferred lighting compute pass. Reads the 5 MRT G-Buffer targets +
    // shadow depth + IBL, evaluates PBR (mirrors ForwardPBR.wgsl), and writes HDR
    // linear color to hdrTexture. Reuses the renderer's frameBuffers_/lightBuffers_
    // (populated internally) so the deferred shader samples the same uniforms as
    // the forward path.
    void RenderDawnDeferredLighting(rhi::RHICommandBuffer* cmdBuffer,
                                    const RenderView& view,
                                    const rhi::ResourceHandle gbufferTextures[DAWN_GBUFFER_RT_COUNT],
                                    rhi::ResourceHandle hdrTexture,
                                    const RenderScene& scene,
                                    u32 frameIndex,
                                    u32 width,
                                    u32 height);

    // Phase N2 — Meshlet deferred lighting. Same PBR as RenderDawnDeferredLighting
    // but consumes the 4-RT meshlet GBuffer (albedo/normal/orm/velocity) written
    // by Nanite/GPUDrivenDrawPipeline + the meshlet depth texture for worldPos
    // reconstruction. gbufferTextures[0..3] map to the meshlet RTs; depthTexture
    // is the sampleable D32 produced by GPUDrivenDrawPipeline::Execute.
    static constexpr u32 DAWN_MESHLET_GBUFFER_RT_COUNT = 4;
    void RenderDawnMeshletDeferredLighting(rhi::RHICommandBuffer* cmdBuffer,
                                           const RenderView& view,
                                           const rhi::ResourceHandle gbufferTextures[DAWN_MESHLET_GBUFFER_RT_COUNT],
                                           rhi::ResourceHandle depthTexture,
                                           rhi::ResourceHandle hdrTexture,
                                           const RenderScene& scene,
                                           u32 frameIndex,
                                           u32 width,
                                           u32 height,
                                           rhi::ResourceHandle gi_indirect_texture = rhi::handles::INVALID_RESOURCE);

    GeometryDebugSettings& GetDebugSettings() { return debugSettings_; }
    
    const SceneExtractionStats& GetExtractionStats() const { return sceneExtractionSystem_.GetStats(); }
    SceneExtractionStats& GetExtractionStats() { return sceneExtractionSystem_.GetStats(); }

private:
    GeometryDebugSettings debugSettings_;

    void DepthPrePass(rhi::RHICommandBuffer* cmdBuffer, 
                     const RenderView& view, 
                     rhi::ResourceHandle depthStencil,
                     const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                     const utl::vector<const RenderProxy*>& proxies,
                     u32 frameIndex,
                     u32 width,
                     u32 height);

    void RenderReflections(rhi::RHICommandBuffer* cmdBuffer,
                          const RenderScene& scene,
                          const RenderView& mainView,
                          const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                          u32 frameIndex);

    void ShadowPass(rhi::RHICommandBuffer* cmdBuffer, 
                   const RenderView& view, 
                   rhi::ResourceHandle shadowMap,
                   const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                   const utl::vector<const RenderProxy*>& proxies,
                   u32 frameIndex,
                   u32 arrayLayer);

    void SetupLights(const RenderScene& scene,  
                    u32 frameIndex, 
                    rhi::GlobalShaderData* globalData,
                    const utl::vector<RenderView>& shadowViews,
                    const utl::vector<float>& splits,
                    const ::std::unordered_map<u32, int>& lightShadowIndices,
                    const ::std::unordered_map<u32, rhi::math::m4x4>& lightViewProjs);

    void OpaquePass(rhi::RHICommandBuffer* cmdBuffer, 
                   const RenderView& view, 
                   const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                   const utl::vector<const RenderProxy*>& proxies,
                   u32 frameIndex,
                   bool useDepthEqual,
                   rhi::DescriptorSetHandle overrideGlobalSet = rhi::handles::INVALID_DESCRIPTOR_SET,
                   PipelineFlags extraFlags = PipelineFlags::None);

    void TransparentPass(rhi::RHICommandBuffer* cmdBuffer, 
                        const RenderView& view, 
                        const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                        const utl::vector<const RenderProxy*>& proxies,
                        u32 frameIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    
    // Global Descriptor Set (Set 0)
    rhi::DescriptorSetLayoutHandle globalDescriptorSetLayout_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetLayoutHandle globalDescriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Shadow Resources
    rhi::ResourceHandle shadowMapArray_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadowMapSampler_{rhi::handles::INVALID_RESOURCE};
    // Shared depth buffer for shadow rendering
    rhi::ResourceHandle shadowDepthBuffer_{rhi::handles::INVALID_RESOURCE};
    // Temporary Shadow Map Array for Blur Pass (VSM)
    rhi::ResourceHandle shadowMapTempArray_{rhi::handles::INVALID_RESOURCE};

    rhi::ResourceHandle shadowCubeMapArray_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadowCubeMapSampler_{rhi::handles::INVALID_RESOURCE};

    // SSR Resources
    rhi::ResourceHandle ssrOutput_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle compositePipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle compositePipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::DescriptorSetLayoutHandle compositeDescriptorSetLayout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle compositeDescriptorSet_{rhi::handles::INVALID_RESOURCE};

    // Planar Reflection Resources
    struct ReflectionResource {
        rhi::ResourceHandle texture{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle depth{rhi::handles::INVALID_RESOURCE};
        rhi::ResourceHandle frameBuffer{rhi::handles::INVALID_RESOURCE};
        void* frameBufferMapped{nullptr};
        rhi::DescriptorSetHandle descriptorSet{rhi::handles::INVALID_DESCRIPTOR_SET};
    };
    ::std::unordered_map<id::id_type, ReflectionResource> reflectionResources_;
    rhi::SamplerHandle reflectionSampler_{rhi::handles::INVALID_RESOURCE};

    // Passes
    BlurPass blurPass_;
    SSRPass ssrPass_;
#ifndef DISABLE_PARTICLE_SYSTEM
    ParticlePass particlePass_;
#endif

    // Scene Extraction System (Nanite v7.1)
    SceneExtractionSystem sceneExtractionSystem_;
    bool sceneExtractionEnabled_{true};

    // Multi-frame buffers to avoid CPU-GPU sync stalls
    rhi::ResourceHandle lightBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* lightBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    rhi::ResourceHandle frameBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* frameBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Per-Object Buffer (Dynamic Uniform Buffer)
    rhi::ResourceHandle perObjectBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* perObjectBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    u32 perObjectBufferOffset_{0};
    static constexpr u32 MAX_PER_OBJECT_SIZE = 1 * 1024 * 1024; // 1MB (enough for ~2000 objects at 512 bytes each)
    
    // Per-Object Descriptor Set (Set 1)
    rhi::DescriptorSetLayoutHandle perObjectDescriptorSetLayout_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetHandle perObjectDescriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    float deltaTime_{0.0f};
    float totalTime_{0.0f};
    u32 frameNumber_{0};

    // Dawn shadow resources — shadow bindings go in Group 0 (bindings 13, 14)
    rhi::ResourceHandle dawnShadowDepthTex_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle dawnShadowSampler_{rhi::handles::INVALID_SAMPLER};
    primal::math::m4x4 dawnShadowLightVP_{};
    primal::math::m4x4 dawnCascadeVPs_[4]{};
    float dawnCascadeSplits_[4]{};

    // Dawn IBL resources — bindings 15-18 in Group 0
    rhi::ResourceHandle dawnIBLIrradiance_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dawnIBLPrefilter_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dawnIBLBRDFLUT_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle dawnIBLSampler_{rhi::handles::INVALID_SAMPLER};

    u32 dawnRenderMode_{2}; // default ShadowAndIBL

    // Debug visualization + IBL toggle for meshlet modes (7/8).
    // debugMode is forwarded to GPUDrivenDrawPipeline::SetDebugMode by the test;
    // enableIBL is written into GlobalShaderData to gate the deferred IBL block.
    u32 dawnMeshletDebugMode_{0};   // 0=off, 1=meshlet_id, 2=triangle_id, 3=mesh_id
    u32 dawnEnableIBL_{1};
    u32 dawnEnableDDGI_{0};  // 0 = skip DDGI indirect (default), 1 = apply (Mode 10)

    // 1x1 fallback texture for binding 13 when DDGI is off. Avoids WebGPU
    // validation errors from binding INVALID_RESOURCE to a declared slot.
    rhi::ResourceHandle dawnDummy1x1Tex_{rhi::handles::INVALID_RESOURCE};

    // Phase 2: previous-frame state for velocity MRT
    primal::math::m4x4 prevViewProjection_{};
    std::unordered_map<id::id_type, primal::math::m4x4> prevWorldMap_;

    // Dawn shadow pipeline (owned by ForwardRenderer)
    rhi::DescriptorSetLayoutHandle dawnShadowDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle dawnShadowPipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle dawnShadowPipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::ResourceHandle dawnShadowPerObjectBuf_{rhi::handles::INVALID_RESOURCE};
    void* dawnShadowPerObjectMapped_{nullptr};
    rhi::DescriptorSetHandle dawnShadowPerObjectSet_{rhi::handles::INVALID_DESCRIPTOR_SET};

    void RenderDawnShadowPass(rhi::RHICommandBuffer* cmdBuffer,
                              const RenderScene& scene,
                              const RenderView& view,
                              const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials);

    // Phase 3c-2 — lazily creates the G-Buffer graphics pipeline on first use.
    // Deferred because the pipeline layout needs the test-supplied material
    // DSL (group 1), which isn't available at ForwardRenderer::Initialize() time.
    // Returns true if the pipeline is ready (or already was).
    bool EnsureDawnGBufferPipeline();

    // Dawn camera depth prepass — non-jittered depth for HZB/SSR/SSAO.
    // Uses its own pipeline so it never touches GlobalShaderData.jitterOffset,
    // producing a stable depth buffer that downstream passes consume.
    static constexpr u32 DAWN_PREPASS_PER_OBJECT_ALIGN = 256;
    static constexpr u32 DAWN_PREPASS_MAX_OBJECTS = 4096; // 1MB / 256B
    rhi::DescriptorSetLayoutHandle dawnPrepassDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle dawnPrepassPipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle dawnPrepassPipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::ResourceHandle dawnPrepassPerObjectBuf_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* dawnPrepassPerObjectMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::DescriptorSetHandle dawnPrepassPerObjectSet_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Dawn G-Buffer pass (Phase 3b) — writes 5 MRT G-Buffer targets for Deferred mode.
    // Same per-object dynamic-offset pattern as the prepass, but with a 3-matrix
    // uniform (world + WVP + prevWVP) and an actual fragment shader.
    // Phase 3c-2: pipeline layout = [perObject, material] — material DSL is
    // supplied by the test (matches its MaterialInstance descriptor sets).
    static constexpr u32 DAWN_GBUFFER_PER_OBJECT_ALIGN = 256;
    static constexpr u32 DAWN_GBUFFER_MAX_OBJECTS = 4096; // 1MB / 256B
    rhi::DescriptorSetLayoutHandle dawnGBufferDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetLayoutHandle dawnGBufferMaterialDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle dawnGBufferPipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle dawnGBufferPipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::ResourceHandle dawnGBufferPerObjectBuf_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* dawnGBufferPerObjectMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::DescriptorSetHandle dawnGBufferPerObjectSet_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Dawn Deferred lighting pass (Phase 3c) — compute shader reading G-Buffer +
    // shadow depth + IBL, writing HDR color. 12 bindings in set 0:
    //   0..3 gbuffer, 4 shadowDepthTex, 5..7 IBL, 8 iblSampler,
    //   9 globalData UB, 10 lightBuffer UB, 11 outputTex storage.
    // Frame/light UBs reuse the renderer's existing frameBuffers_/lightBuffers_.
    // Per-frame descriptor sets bind the G-Buffer + IBL textures (caller-supplied)
    // and the global/light UB handles (renderer-owned).
    rhi::DescriptorSetLayoutHandle dawnDeferredDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle dawnDeferredPipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle dawnDeferredPipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::DescriptorSetHandle dawnDeferredSet_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Dawn Meshlet Deferred lighting pass (Phase N2) — compute shader reading the
    // 4-RT meshlet GBuffer + sampleable depth + shadow depth + IBL, writing HDR
    // color. 14 bindings in set 0:
    //   0..3 meshlet gbuffer (albedo/normal/orm/velocity),
    //   4 depthTex (SampledDepthImage), 5 shadowDepthTex (depth_2d_array),
    //   6..8 IBL (cube, cube, 2d), 9 iblSampler,
    //   10 globalData UB, 11 lightBuffer UB, 12 outputTex storage,
    //   13 gi_indirect_tex (DDGI indirect — Mode 10 only, fallback 1x1 otherwise).
    rhi::DescriptorSetLayoutHandle dawnMeshletDeferredDSL_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineLayoutHandle dawnMeshletDeferredPipelineLayout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineHandle dawnMeshletDeferredPipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::DescriptorSetHandle dawnMeshletDeferredSet_[rhi::MAX_FRAMES_IN_FLIGHT]{};

public:
    void SetTime(float deltaTime, float totalTime, u32 frameNumber) {
        deltaTime_ = deltaTime;
        totalTime_ = totalTime;
        frameNumber_ = frameNumber;
    }

    void SetDawnShadowResources(rhi::ResourceHandle depthTex, rhi::SamplerHandle sampler);
    void SetDawnShadowLightVP(const primal::math::m4x4& vp) { dawnShadowLightVP_ = vp; }
    void SetDawnCascadeVPs(const primal::math::m4x4 vps[4], const float splits[4]) {
        memcpy(dawnCascadeVPs_, vps, sizeof(dawnCascadeVPs_));
        memcpy(dawnCascadeSplits_, splits, sizeof(dawnCascadeSplits_));
    }
    void SetDawnIBLResources(rhi::ResourceHandle irradiance, rhi::ResourceHandle prefilter,
                             rhi::ResourceHandle brdfLUT, rhi::SamplerHandle sampler);
    void SetDawnRenderMode(u32 mode) { dawnRenderMode_ = mode; }
    void SetDawnMeshletDebugMode(u32 mode) { dawnMeshletDebugMode_ = mode; }
    void SetDawnEnableIBL(u32 enable) { dawnEnableIBL_ = enable; }
    void SetDawnEnableDDGI(u32 enable) { dawnEnableDDGI_ = enable; }

    // Phase 3c-2: caller supplies the material DSL (created by the test, shared
    // with MaterialInstance descriptor sets). The G-Buffer pipeline binds this
    // as group 1 so per-draw material textures can be sampled in the geometry
    // pass. Must be called BEFORE Initialize() so the pipeline layout includes
    // the material group at creation time.
    void SetDawnGBufferMaterialDSL(rhi::DescriptorSetLayoutHandle layout) {
        dawnGBufferMaterialDSL_ = layout;
    }

    rhi::DescriptorSetLayoutHandle GetGlobalDescriptorSetLayout() const { return globalDescriptorSetLayout_; }
    rhi::DescriptorSetLayoutHandle GetPerObjectDescriptorSetLayout() const { return perObjectDescriptorSetLayout_; }
};

} // namespace primal::graphics
