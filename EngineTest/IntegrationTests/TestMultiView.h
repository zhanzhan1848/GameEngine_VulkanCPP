#pragma once
#include "RenderTestFramework.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/Material.h"
#include "Platform/Window.h"
#include <map>
#include <string>

class MultiViewTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

protected:
    primal::graphics::rhi::RHIDeviceBase* device = nullptr;
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_ownership;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph;
    
    // Scene Assets
    primal::graphics::RenderScene scene;
    primal::graphics::RenderView view;
    primal::graphics::RenderMesh* cubeMesh = nullptr;
    
    // Pipeline Resources
    primal::graphics::rhi::PipelineLayoutHandle pipelineLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::PipelineHandle pipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::ShaderHandle vertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle pixelShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::DescriptorSetLayoutHandle dsLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetHandle descriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    // Mesh Resources
    primal::graphics::rhi::ResourceHandle vertexBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle indexBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    uint32_t indexCount = 0;
    
    // Uniform Buffers
    primal::graphics::rhi::ResourceHandle viewUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle instanceUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle multiViewDepthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE; // Depth for CubeMap

    // Main View Resources
    primal::graphics::rhi::ResourceHandle mainViewUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Main Camera
    primal::graphics::rhi::ResourceHandle mainDepthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE; // Depth for Main Window
    primal::graphics::rhi::DescriptorSetHandle mainDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET; // Set for Main View
    
    // Reflection Resources
    primal::graphics::rhi::ResourceHandle reflectionTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionDepthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Reflection Camera
    primal::graphics::rhi::ResourceHandle reflectionPlaneBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Plane Eq
    primal::graphics::rhi::DescriptorSetHandle reflectionDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle mirrorDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    primal::graphics::rhi::PipelineHandle reflectionPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle reflectionPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineHandle mirrorPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle mirrorPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    
    primal::graphics::rhi::ShaderHandle reflectionVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle mirrorVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle mirrorPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;

    // Draw Ranges
    struct DrawRange {
        uint32_t start;
        uint32_t count;
    };
    std::map<std::string, DrawRange> drawRanges;

    // Simple Pipeline Resources (Direct Render)
    primal::graphics::rhi::PipelineHandle simplePipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle mainPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE; // New Pipeline for BackBuffer
    primal::graphics::rhi::ShaderHandle simpleVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle simplePixelShader = primal::graphics::rhi::handles::INVALID_SHADER;

    // Blit / Debug Pass Resources
    primal::graphics::rhi::PipelineHandle blitPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle blitPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::ShaderHandle blitVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle blitPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::DescriptorSetLayoutHandle blitDSLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    std::vector<primal::graphics::rhi::DescriptorSetHandle> blitDescriptorSets;
    primal::graphics::rhi::ResourceHandle blitUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // Debug Overlay Pipeline Resources
    primal::graphics::rhi::PipelineHandle debugPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::ShaderHandle debugVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle debugPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;

    // Debug Overlay State
    bool showDebugOverlay = true;

    // Command Buffer
    std::vector<primal::graphics::rhi::CommandBufferHandle> commandBuffers;

private:
    bool CreateReflectionResources();
    void CreateCubeMesh();
    std::string ReadShaderFile(const std::string& filepath);
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
