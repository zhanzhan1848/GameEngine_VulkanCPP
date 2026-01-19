#pragma once
#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/RenderMesh.h"

/**
 * @brief CSM Integration Test using RenderGraph
 * @details Re-implementation of TestCSMIntegration using RenderGraph for pass management
 */
class CSMIntegrationRenderGraphTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

protected:
    // Helper to read shader file
    std::string ReadShaderFile(const std::string& filepath);
    
    // Helper to draw scene
    void DrawScene(primal::graphics::rhi::RHICommandBuffer* cmdBuffer, 
                   primal::graphics::rhi::RenderPassHandle renderPass,
                   uint32_t permutationId,
                   primal::graphics::PipelineFlags flags = primal::graphics::PipelineFlags::None,
                   uint32_t instanceCount = 1);

    // Rendering Resources
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device = nullptr;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem; // Still needed for some basics like SwapChain/Window interaction
    
    // Render Graph
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph;

    // Scene Objects
    primal::graphics::RenderScene scene;
    primal::graphics::RenderView view;
    
    // Assets
    primal::graphics::RenderMesh* cubeMesh = nullptr;
    primal::graphics::Material* material = nullptr;
    primal::graphics::MaterialInstance* materialInstance = nullptr; // Cube
    primal::graphics::MaterialInstance* floorMaterialInstance = nullptr; // Floor
    
    primal::graphics::rhi::DescriptorSetLayoutHandle materialSetLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::PipelineLayoutHandle pipelineLayout = primal::graphics::rhi::handles::INVALID_RESOURCE; // Global (Main)
    primal::graphics::rhi::PipelineLayoutHandle shadowPipelineLayout = primal::graphics::rhi::handles::INVALID_RESOURCE; // Shadow Only
    primal::graphics::rhi::PipelineHandle shadowPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE; // Shadow Pipeline Handle
    primal::graphics::rhi::ShaderHandle shadowVS = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle shadowPS = primal::graphics::rhi::handles::INVALID_SHADER;
    
    // Custom Descriptor Sets
    primal::graphics::rhi::DescriptorSetLayoutHandle globalSetLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetLayoutHandle perObjectSetLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle globalSet = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle shadowGlobalSet = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle perObjectSet = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    primal::graphics::rhi::ResourceHandle globalBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle shadowGlobalBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle lightBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle perObjectBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::SamplerHandle shadowSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    void* perObjectBufferMapped = nullptr;
    void* globalBufferMapped = nullptr;
    void* shadowGlobalBufferMapped = nullptr;
    void* lightBufferMapped = nullptr;
    uint32_t perObjectSize = 0;
    
    // CSM Data

    // Animation
    float rotationAngle = 0.0f;
};

/**
 * @brief Test Entry Point
 */
class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
