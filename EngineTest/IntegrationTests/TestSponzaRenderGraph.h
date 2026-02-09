#pragma once
#include "RenderTestFramework.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/Material.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Utils/IBLPrecomputer.h"
#include "Graphics/SceneDataAdapter.h"
#include "Engine/Platform/Platform.h"
#include "ShaderCompilation.h"

#include <unordered_map>
#include <string>
#include <memory>

class TestSponzaRenderGraph : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Resize(uint32_t width, uint32_t height) override;
    void Run() override;
    void Shutdown() override;

private:
    // Core RHI & System
    primal::graphics::rhi::RHIDeviceBase* device = nullptr;
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_ownership;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph;
    
    // Scene Assets
    primal::graphics::RenderScene scene;
    primal::graphics::RenderView view;
    std::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes;
    
    // Shader Management
    std::unordered_map<std::string, primal::graphics::rhi::ShaderHandle> shaderVariantMap;
    
    // Pass Resources
    primal::graphics::rhi::ResourceHandle skyboxTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    std::unique_ptr<primal::graphics::rhi::IBLPrecomputer> iblPrecomputer;
    primal::graphics::rhi::ResourceHandle irradianceMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle prefilteredMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle brdfLUT = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // Persistent Resources for Static Descriptor Sets
    primal::graphics::rhi::ResourceHandle depthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle shadowMap0 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle shadowMap1 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle envCubemap = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // TAA Resources
    primal::graphics::rhi::ResourceHandle historyTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle motionVectorTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // Test State
    uint32_t frameCount = 0;
    bool debugPassEnabled = false;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;

    // Helper Functions
    bool CompileAllShaders();
    bool SetupPipelines();
    bool LoadScene();
    bool SetupIBL();
    void BuildRenderGraph(primal::graphics::rendergraph::RenderGraph& graph, primal::graphics::rhi::ResourceHandle backBuffer);
    void UpdateScene();
    void ValidateFrame();
    void WriteTexture(primal::graphics::rhi::ResourceHandle texture, const void* data, uint64_t size, uint32_t width, uint32_t height, uint32_t layer = 0);
    
    primal::graphics::rhi::ResourceHandle defaultMaterialSet = primal::graphics::rhi::handles::INVALID_RESOURCE;
    bool CreatePersistentResources();
    
    // Pipelines
    primal::graphics::rhi::PipelineHandle gbufferPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle lightingPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle skyboxPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle shadowPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    
    // Pipeline Layouts
    primal::graphics::rhi::PipelineLayoutHandle gbufferLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle lightingLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle skyboxLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle shadowLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;

    // Descriptor Set Layouts
    primal::graphics::rhi::DescriptorSetLayoutHandle globalSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle materialSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle lightingSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle skyboxSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;

    // Descriptor Sets
    primal::graphics::rhi::DescriptorSetHandle globalDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle lightingDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle skyboxDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    std::unordered_map<primal::graphics::MaterialInstance*, primal::graphics::rhi::DescriptorSetHandle> materialDescriptorSets;

    // Uniform Buffers
    primal::graphics::rhi::ResourceHandle sceneDataBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle viewDataBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle readbackBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // Shadow Matrices
    primal::math::m4x4 lightVP0;
    primal::math::m4x4 lightVP1;

    // TAA State
    primal::math::v2 previousJitter = {0.0f, 0.0f};
    primal::math::m4x4 previousViewProjection = primal::graphics::rhi::math::MatrixIdentity();

    // Debug/Blit Resources
    primal::graphics::rhi::PipelineHandle debugPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle debugLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle debugSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetHandle debugDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::SamplerHandle debugSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;

    // PostProcess Pipeline Resources
    primal::graphics::rhi::PipelineHandle postProcessPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle postProcessLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle postProcessSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetHandle postProcessDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;

    // TAA Pipeline Resources
    primal::graphics::rhi::PipelineHandle taaPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle taaLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle taaSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetHandle taaDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::SamplerHandle taaSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    primal::graphics::rhi::ResourceHandle taaUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // Helper Functions
    bool CreateUniformBuffers();
    bool CreateDescriptorSets();

public:
    static void OnF1Pressed();
private:
    static TestSponzaRenderGraph* instance;
};

#ifdef TEST_SPONZA_RENDERGRAPH
class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
#endif
