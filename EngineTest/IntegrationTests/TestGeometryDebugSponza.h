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
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/Debug/GeometryDebugPass.h"
#include "Engine/Utilities/Hash.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include <chrono>

class TestGeometryDebugSponza;

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};

struct StringHash {
    size_t operator()(const std::string& key) const {
        uint32_t hash;
        primal::utl::MurmurHash3_x86_32(key.c_str(), (int)key.length(), 0, &hash);
        return hash;
    }
};

class TestGeometryDebugSponza : public primal::test::RenderTestCase {
public:
    virtual ~TestGeometryDebugSponza();
    bool Initialize() override;
    void Resize(uint32_t width, uint32_t height) override;
    void Run() override;
    void Shutdown() override;

private:
    bool isShutdown = false;
    // Core RHI & System
    primal::graphics::rhi::RHIDeviceBase* device = nullptr;
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_ownership;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph;
    
    // Camera
    RHICamera m_camera;

    // Scene Assets
    primal::graphics::RenderScene scene;
    primal::graphics::RenderView view;
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes; // stale-test port: LoadRenderItemData returns utl::vector
    
    // Shader Management
    std::unordered_map<std::string, primal::graphics::rhi::ShaderHandle, StringHash> shaderVariantMap;
    
    // Pass Resources
    primal::graphics::rhi::ResourceHandle skyboxTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    std::unique_ptr<primal::graphics::rhi::IBLPrecomputer> iblPrecomputer;
    primal::graphics::rhi::ResourceHandle irradianceMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle prefilteredMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle brdfLUT = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // Persistent Resources
    primal::graphics::rhi::ResourceHandle depthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle whiteTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle normalTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle shadowMap0 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle shadowMap1 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle envCubemap = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // Samplers
    primal::graphics::rhi::SamplerHandle defaultSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    primal::graphics::rhi::SamplerHandle brdfSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    primal::graphics::rhi::SamplerHandle debugSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;

    // Test State
    uint32_t frameCount = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    
    // Geometry Debug Settings
    primal::graphics::GeometryDebugSettings debugSettings;

    // Helper Functions
    bool CompileAllShaders();
    bool SetupPipelines();
    bool LoadScene();
    bool SetupIBL();
    void BuildRenderGraph(primal::graphics::rendergraph::RenderGraph& graph, primal::graphics::rhi::ResourceHandle backBuffer);
    void UpdateScene();
    bool CreateUniformBuffers();
    bool CreateDescriptorSets();
    bool CreatePersistentResources();
    
    primal::graphics::rhi::ResourceHandle defaultMaterialSet = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // Pipelines (reused from Sponza test for basic rendering)
    primal::graphics::rhi::PipelineHandle gbufferPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle lightingPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle skyboxPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle shadowPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineHandle blitPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    
    // Pipeline Layouts
    primal::graphics::rhi::PipelineLayoutHandle gbufferLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle lightingLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle skyboxLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle shadowLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineLayoutHandle blitLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;

    // Descriptor Set Layouts
    primal::graphics::rhi::DescriptorSetLayoutHandle globalSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle materialSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle lightingSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle skyboxSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle blitSetLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;

    // Descriptor Sets
    primal::graphics::rhi::DescriptorSetHandle globalDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle lightingDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle skyboxDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle blitDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;

    // Buffers
    primal::graphics::rhi::ResourceHandle viewDataBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle sceneDataBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;

    // Shadow Matrices
    primal::math::m4x4 lightVP0;
    primal::math::m4x4 lightVP1;

    // Static singleton for main entry
    static TestGeometryDebugSponza* instance;
    
    // Resource Management
    std::vector<primal::graphics::rhi::ResourceHandle> createdResources;
    
    // ============================================
    // Async Texture Loading Support
    // ============================================
    void StartAsyncTextureLoading();
    void UpdateAsyncTextures();
    
    // Async loading state
    std::unordered_map<std::string, primal::graphics::rhi::ResourceHandle> _asyncTextureMap;
    primal::utl::vector<std::string> _pendingTexturePaths; // stale-test port: LoadTexturesAsync takes utl::vector
    std::atomic<bool> _asyncTexturesLoaded{false};
    std::atomic<u32> _asyncTexturesLoadedCount{0};
    std::atomic<u32> _asyncTexturesTotalCount{0};
    primal::jobsystem::JobHandle _asyncLoadHandle;
    bool _asyncLoadStarted{false};
};
