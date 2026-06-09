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
                rhi::ResourceHandle depthStencil,
                const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                u32 frameIndex,
                u32 width,
                u32 height);

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

public:
    void SetTime(float deltaTime, float totalTime, u32 frameNumber) {
        deltaTime_ = deltaTime;
        totalTime_ = totalTime;
        frameNumber_ = frameNumber;
    }

    void SetDawnShadowResources(rhi::ResourceHandle depthTex, rhi::SamplerHandle sampler);
    void SetDawnShadowLightVP(const primal::math::m4x4& vp) { dawnShadowLightVP_ = vp; }

    rhi::DescriptorSetLayoutHandle GetGlobalDescriptorSetLayout() const { return globalDescriptorSetLayout_; }
    rhi::DescriptorSetLayoutHandle GetPerObjectDescriptorSetLayout() const { return perObjectDescriptorSetLayout_; }
};

} // namespace primal::graphics
