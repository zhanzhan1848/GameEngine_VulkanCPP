#pragma once
#include "RenderGraph.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::rendergraph {

class RenderGraphDebug {
public:
    RenderGraphDebug(rhi::RHIDeviceBase& device, const std::string& shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/DebugGraph.metal");
    ~RenderGraphDebug();

    void Update(float deltaTime);
    void Draw(rhi::RHICommandBuffer* cmdBuffer, const RenderGraph& graph, u32 width, u32 height);

    void ToggleEnabled() { enabled_ = !enabled_; }
    bool IsEnabled() const { return enabled_; }

    void SetDebugResources(const utl::vector<std::pair<std::string, RenderGraphResource*>>& resources) {
        debugResources_ = resources;
        graphDirty_ = true;
    }

private:
    void CreatePipeline();
    void BuildGraphMesh(const RenderGraph& graph);
    void BuildTimelineMesh(const RenderGraph& graph, float width, float height);

    rhi::RHIDeviceBase& device_;
    std::string shaderPath_;
    rhi::PipelineHandle pipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::ResourceHandle vertexBuffer_ = rhi::handles::INVALID_RESOURCE;
    size_t vertexBufferSize_ = 0;
    rhi::ResourceHandle uniformBuffer_ = rhi::handles::INVALID_RESOURCE;
    
    // Texture Rendering
    rhi::PipelineHandle texturePipeline_ = rhi::handles::INVALID_PIPELINE;
    rhi::ResourceHandle textureVertexBuffer_ = rhi::handles::INVALID_RESOURCE;
    size_t textureVertexBufferSize_ = 0;
    rhi::ResourceHandle textureUniformBuffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::SamplerHandle textureSampler_ = rhi::handles::INVALID_SAMPLER;
    rhi::DescriptorSetLayoutHandle textureSetLayout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    rhi::PipelineLayoutHandle texturePipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    
    // Cache for descriptor sets (one per debug resource)
    // We rebuild these every frame or when resources change
    utl::vector<rhi::DescriptorSetHandle> textureSets_;

    bool enabled_ = false;
    bool graphDirty_ = true;
    
    // Debug Resources
    utl::vector<std::pair<std::string, RenderGraphResource*>> debugResources_;

    // View state
    math::v2 offset_ = {50.0f, 50.0f}; // Move down to see vertical stack
    float scale_ = 0.8f; // Zoom out slightly
    bool f1_pressed_ = false;
    
    struct Vertex {
        math::v2 pos;
        math::v4 color;
    };
    utl::vector<Vertex> vertices_;

    struct TextureVertex {
        math::v2 pos;
        math::v2 uv;
        float type; // 0: Color, 1: Depth, 2: Moments (RG32)
    };
    
    // Layout cache
    struct NodeLayout {
        math::v2 pos;
        math::v2 size;
        math::v4 color;
    };
    std::unordered_map<std::string, NodeLayout> layoutCache_;

    // Timing
    // double lastLogTime_ = 0.0; // Removed unused warning
};

}
