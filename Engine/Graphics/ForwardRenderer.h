#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHIShaderCommon.h"
#include "Graphics/RHI/Utils/ShadowUtils.h"
#include <unordered_map>

namespace primal::graphics {

class RenderScene;
class RenderView;
struct RenderProxy;
class MaterialInstance;

// Definition of Binding Slots
constexpr uint32_t PER_OBJECT_BINDING = 10;
constexpr uint32_t FRAME_DATA_BINDING = 11;
constexpr uint32_t LIGHT_DATA_BINDING = 12;
constexpr uint32_t SHADOW_MAP_BINDING = 13;
constexpr uint32_t SHADOW_CUBE_MAP_BINDING = 14;

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
                const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                uint32_t frameIndex,
                uint32_t width,
                uint32_t height);

private:
    void DepthPrePass(rhi::RHICommandBuffer* cmdBuffer, 
                     const RenderView& view, 
                     rhi::ResourceHandle depthStencil,
                     const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                     const utl::vector<const RenderProxy*>& proxies,
                     uint32_t frameIndex,
                     uint32_t width,
                     uint32_t height);

    void ShadowPass(rhi::RHICommandBuffer* cmdBuffer, 
                   const RenderView& view, 
                   rhi::ResourceHandle shadowMap,
                   const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                   const utl::vector<const RenderProxy*>& proxies,
                   uint32_t frameIndex,
                   uint32_t arrayLayer);

    void SetupLights(const RenderScene& scene,  
                    uint32_t frameIndex, 
                    rhi::GlobalShaderData* globalData,
                    const utl::vector<RenderView>& shadowViews,
                    const utl::vector<float>& splits,
                    const std::unordered_map<uint32_t, int>& lightShadowIndices,
                    const std::unordered_map<uint32_t, rhi::math::m4x4>& lightViewProjs);

    void OpaquePass(rhi::RHICommandBuffer* cmdBuffer, 
                   const RenderView& view, 
                   const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                   const utl::vector<const RenderProxy*>& proxies,
                   uint32_t frameIndex,
                   bool useDepthEqual);

    void TransparentPass(rhi::RHICommandBuffer* cmdBuffer, 
                        const RenderView& view, 
                        const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                        const utl::vector<const RenderProxy*>& proxies,
                        uint32_t frameIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    
    // Global Descriptor Set (Set 0)
    rhi::DescriptorSetLayoutHandle globalDescriptorSetLayout_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetHandle globalDescriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Shadow Pipeline
    rhi::PipelineLayoutHandle shadowPipelineLayout_{rhi::handles::INVALID_RESOURCE};
    rhi::PipelineHandle shadowPipeline_{rhi::handles::INVALID_PIPELINE};

    // Shadow Resources
    rhi::ResourceHandle shadowMapArray_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadowMapSampler_{rhi::handles::INVALID_RESOURCE};
    // Shared depth buffer for shadow rendering
    rhi::ResourceHandle shadowDepthBuffer_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadowCubeMapArray_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle shadowCubeMapSampler_{rhi::handles::INVALID_RESOURCE};

    // Multi-frame buffers to avoid CPU-GPU sync stalls
    rhi::ResourceHandle lightBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* lightBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    rhi::ResourceHandle frameBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* frameBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    // Per-Object Buffer (Dynamic Uniform Buffer)
    rhi::ResourceHandle perObjectBuffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    void* perObjectBuffersMapped_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    uint32_t perObjectBufferOffset_{0};
    static constexpr uint32_t MAX_PER_OBJECT_SIZE = 10 * 1024 * 1024; // 10MB per frame
    
    // Per-Object Descriptor Set (Set 1)
    rhi::DescriptorSetLayoutHandle perObjectDescriptorSetLayout_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetHandle perObjectDescriptorSets_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    float deltaTime_{0.0f};
    float totalTime_{0.0f};
    uint32_t frameNumber_{0};

public:
    void SetTime(float deltaTime, float totalTime, uint32_t frameNumber) {
        deltaTime_ = deltaTime;
        totalTime_ = totalTime;
        frameNumber_ = frameNumber;
    }

    rhi::DescriptorSetLayoutHandle GetGlobalDescriptorSetLayout() const { return globalDescriptorSetLayout_; }
    rhi::DescriptorSetLayoutHandle GetPerObjectDescriptorSetLayout() const { return perObjectDescriptorSetLayout_; }
};

} // namespace primal::graphics
