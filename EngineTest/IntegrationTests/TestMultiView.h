#pragma once
#include "RenderTestFramework.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/Material.h"
#include "Graphics/Passes/SSRPass.h"
#include "Graphics/Passes/TAAPass.h"
#include "Platform/Window.h"
#include <map>
#include <string>

#include "Graphics/RHI/Utils/IBLPrecomputer.h"
#include "Graphics/MaterialInstance.h"

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
    primal::graphics::rhi::ResourceHandle mainColorTexture = primal::graphics::rhi::handles::INVALID_RESOURCE; // Main Scene Color
    primal::graphics::rhi::ResourceHandle mainDepthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE; // Depth for Main Window
    primal::graphics::rhi::DescriptorSetHandle mainDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET; // Set for Main View
    
    // Reflection Resources
    primal::graphics::rhi::ResourceHandle reflectionTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionDepthTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Reflection Camera
    primal::graphics::rhi::ResourceHandle reflectionPlaneBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Plane Eq
    primal::graphics::rhi::ResourceHandle mirrorUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE; // Mirror Instance Data (Dynamic)
    primal::graphics::rhi::DescriptorSetHandle reflectionDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle mirrorDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    // Second Reflection Plane Resources
    primal::graphics::rhi::ResourceHandle reflectionTexture2 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionDepthTexture2 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionUniformBuffer2 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionPlaneBuffer2 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle reflectionDescriptorSet2 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    // Third Reflection Plane Resources (Top Face)
    primal::graphics::rhi::ResourceHandle reflectionTexture3 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionDepthTexture3 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionUniformBuffer3 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle reflectionPlaneBuffer3 = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle reflectionDescriptorSet3 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    primal::graphics::rhi::PipelineHandle reflectionPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle reflectionPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::PipelineHandle mirrorPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle mirrorPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    
    primal::graphics::rhi::ShaderHandle reflectionVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle mirrorVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle mirrorPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;


    // Skybox Resources
    primal::graphics::rhi::ResourceHandle skyboxTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::PipelineHandle skyboxPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle skyboxPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle skyboxDSLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::DescriptorSetHandle skyboxDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle skyboxReflDescriptorSet1 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle skyboxReflDescriptorSet2 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle skyboxReflDescriptorSet3 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;

    // ShortBox Descriptor Sets
    primal::graphics::rhi::DescriptorSetHandle shortBoxMainDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle shortBoxReflDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle shortBoxReflDescriptorSet2 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle shortBoxReflDescriptorSet3 = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::ResourceHandle skyboxUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ShaderHandle skyboxVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle skyboxPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;

    // ShortBox Resources
    primal::graphics::rhi::ResourceHandle shortBoxUniformBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::DescriptorSetHandle shortBoxDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    
    // TAA Resources
    primal::graphics::TAAPass taaPass;
    primal::graphics::rhi::ResourceHandle taaHistoryTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle taaResultTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle mainVelocityTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // TAA State
    uint64_t frameCount = 0;
    primal::math::m4x4 previousViewProjection;
    std::vector<primal::graphics::rhi::math::v2> jitterSamples;

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

    // SH Coefficients
    primal::math::v4 computedSH[9];
    void ProjectCubemapToSH(unsigned char* pixels[6], int width, int height);

    // Debug Overlay Pipeline Resources
    primal::graphics::rhi::PipelineHandle debugPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::ShaderHandle debugVertexShader = primal::graphics::rhi::handles::INVALID_SHADER;
    primal::graphics::rhi::ShaderHandle debugPixelShader = primal::graphics::rhi::handles::INVALID_SHADER;

    // Debug Overlay State
    bool showDebugOverlay = true;

    // SSR Resources
    primal::graphics::SSRPass ssrPass;
    primal::graphics::RenderScene::RenderReflectionPlane reflectionPlane;
    primal::graphics::rhi::ResourceHandle sceneColorTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle ssrOutputTexture = primal::graphics::rhi::handles::INVALID_RESOURCE;

    primal::graphics::rhi::PipelineHandle ssrCompositePipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::DescriptorSetHandle mainCompositeDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;
    primal::graphics::rhi::DescriptorSetHandle ssrCompositeDescriptorSet = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET;

    // Command Buffer
    std::vector<primal::graphics::rhi::CommandBufferHandle> commandBuffers;

    // IBL Resources
    std::unique_ptr<primal::graphics::rhi::IBLPrecomputer> iblPrecomputer;
    primal::graphics::rhi::ResourceHandle envMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle irradianceMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle prefilteredMap = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle brdfLUT = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // PBR Material
    std::unique_ptr<primal::graphics::Material> pbrMaterial;
    std::unique_ptr<primal::graphics::MaterialInstance> pbrMaterialInstance;
    
    // PBR Resources (Buffers)
    primal::graphics::rhi::PipelineHandle pbrPipeline = primal::graphics::rhi::handles::INVALID_PIPELINE;
    primal::graphics::rhi::PipelineLayoutHandle pbrPipelineLayout = primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT;
    primal::graphics::rhi::DescriptorSetLayoutHandle pbrDSLayout = primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    primal::graphics::rhi::ResourceHandle pbrSpherePerObjectBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle pbrLightBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::SamplerHandle pbrDefaultSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    primal::graphics::rhi::SamplerHandle pbrBRDFSampler = primal::graphics::rhi::handles::INVALID_SAMPLER;
    
    // Sphere Mesh for PBR
    primal::graphics::rhi::ResourceHandle sphereVertexBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::ResourceHandle sphereIndexBuffer = primal::graphics::rhi::handles::INVALID_RESOURCE;
    uint32_t sphereIndexCount = 0;

private:
    bool CreateReflectionResources();

    // Helper to load Cubemap
    primal::graphics::rhi::ResourceHandle LoadCubemap(const std::vector<std::string>& filenames);

    void CreateCubeMesh();
    void CreateSphereMesh();
    bool SetupIBL();
    bool CreatePBRResources();
    std::string ReadShaderFile(const std::string& filepath);
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
