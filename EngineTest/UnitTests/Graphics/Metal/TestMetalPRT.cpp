#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalShader.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#include "Utilities/SphericalHarmonics.h"
#include "Graphics/Utilities/PRTBaker.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>

#if defined(__APPLE__)
#include <simd/simd.h>
#endif

using namespace primal::graphics::rhi;
using namespace primal::graphics::utl;
using namespace primal::math::sh;
using namespace Engine::Test;

namespace
{
    std::string ReadShaderFile(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            std::cerr << "Failed to open shader file: " << path << std::endl;
            return "";
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string GetCombinedShaderSource(const std::string& shaderName)
    {
        std::string projectRoot = PROJECT_ROOT;
        std::string shCommonPath = projectRoot + "/EngineTest/shaders/SH_Common.metal";
        std::string testPrtPath = projectRoot + "/EngineTest/shaders/" + shaderName;

        std::string shCommon = ReadShaderFile(shCommonPath);
        std::string testPrt = ReadShaderFile(testPrtPath);

        if (shCommon.empty() || testPrt.empty())
        {
            return "";
        }

        // Simple include replacement
        std::string includeDirective = "#include \"SH_Common.metal\"";
        size_t pos = testPrt.find(includeDirective);
        if (pos != std::string::npos)
        {
            testPrt.replace(pos, includeDirective.length(), shCommon);
        }
        
        return testPrt;
    }

    struct Vertex
    {
        simd::float3 position;
        simd::float3 normal;
    };

    struct TransferSH
    {
        simd::float3 sh0;
        simd::float3 sh1;
        simd::float3 sh2;
    };

    void CreateSphere(float radius, int slices, int stacks, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
    {
        for(int i = 0; i <= stacks; ++i) {
            float v = (float)i / (float)stacks;
            float phi = v * M_PI;

            for(int j = 0; j <= slices; ++j) {
                float u = (float)j / (float)slices;
                float theta = u * 2.0f * M_PI;

                float x = radius * sin(phi) * cos(theta);
                float y = radius * cos(phi);
                float z = radius * sin(phi) * sin(theta);

                Vertex vert;
                vert.position = {x, y, z};
                vert.normal = {x/radius, y/radius, z/radius}; 
                vertices.push_back(vert);
            }
        }

        for(int i = 0; i < stacks; ++i) {
            for(int j = 0; j < slices; ++j) {
                int first = (i * (slices + 1)) + j;
                int second = first + slices + 1;

                indices.push_back(first);
                indices.push_back(second);
                indices.push_back(first + 1);

                indices.push_back(second);
                indices.push_back(second + 1);
                indices.push_back(first + 1);
            }
        }
    }

    struct Uniforms
    {
        simd::float4x4 modelViewProjectionMatrix;
        simd::float4x4 normalMatrix;
        // SH9Color envSH; // 9 * float3. But std140/metal alignment might be tricky.
        // float3 is 16-byte aligned in struct if it's an array?
        // In Metal, float3 is 16-byte aligned.
        // SH9Color struct in C++ has 9 * v3.
        // v3 in MathTypes.h on Mac is simd::float3.
        // simd::float3 is 16 bytes.
        // So it should match float3[9] in Metal if packed correctly.
        // Let's verify alignment.
        simd::float3 envSH[9]; 
    };

    TestResult TestPRTRendering()
    {
        // 1. Initialize Device
        DeviceDesc deviceDesc;
        deviceDesc.platform = RHIPlatform::Metal;
        deviceDesc.enableDebug = true;

        MetalDevice device(deviceDesc);
        if (!device.Initialize())
        {
            return TestResult::Failed;
        }

        // 2. Prepare Shader Source
        std::string shaderSource = GetCombinedShaderSource("TestPRT.metal");
        if (shaderSource.empty())
        {
            return TestResult::Failed;
        }

        // 3. Create Shaders
        ShaderHandle vsHandle = device.CreateShader(shaderSource.c_str(), shaderSource.length(), ShaderStage::Vertex, "prt_vertex");
        ShaderHandle fsHandle = device.CreateShader(shaderSource.c_str(), shaderSource.length(), ShaderStage::Pixel, "prt_fragment");

        TEST_ASSERT(vsHandle != handles::INVALID_SHADER, "Failed to create vertex shader");
        TEST_ASSERT(fsHandle != handles::INVALID_SHADER, "Failed to create fragment shader");

        // 4. Create Pipeline
        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vsHandle;
        pipelineDesc.pixelShader = fsHandle;
        
        // Vertex Layout
        // Binding 0, Stride 32
        VertexInputBinding binding;
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.perVertex = true;
        pipelineDesc.vertexBindings.push_back(binding);

        // Attribute 0: Position (Offset 0)
        VertexInputAttribute attrPos;
        attrPos.location = 0;
        attrPos.binding = 0;
        attrPos.format = DataFormat::RGB32_Float;
        attrPos.offset = offsetof(Vertex, position);
        pipelineDesc.vertexAttributes.push_back(attrPos);

        // Attribute 1: Normal (Offset 16)
        VertexInputAttribute attrNorm;
        attrNorm.location = 1;
        attrNorm.binding = 0;
        attrNorm.format = DataFormat::RGB32_Float;
        attrNorm.offset = offsetof(Vertex, normal);
        pipelineDesc.vertexAttributes.push_back(attrNorm);
        
        pipelineDesc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.depthStencilFormat = DataFormat::Unknown; 

        // 4.1 Create Descriptor Set Layout
        DescriptorSetLayoutBinding layoutBinding;
        layoutBinding.binding = 1; // Match shader binding
        layoutBinding.descriptorType = DescriptorType::UniformBuffer;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

        DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 1;
        layoutDesc.bindings = &layoutBinding;
        DescriptorSetLayoutHandle setLayout = device.CreateDescriptorSetLayout(layoutDesc);
        TEST_ASSERT(setLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "Failed to create descriptor set layout");

        // 4.2 Create Pipeline Layout
        PipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.setLayoutCount = 1;
        pipelineLayoutDesc.setLayouts = &setLayout;
        PipelineLayoutHandle pipelineLayout = device.CreatePipelineLayout(pipelineLayoutDesc);
        TEST_ASSERT(pipelineLayout != handles::INVALID_PIPELINE_LAYOUT, "Failed to create pipeline layout");

        pipelineDesc.layout = pipelineLayout;

        PipelineHandle pipelineHandle = device.CreateGraphicsPipeline(pipelineDesc);
        TEST_ASSERT(pipelineHandle != handles::INVALID_PIPELINE, "Failed to create pipeline");

        // 5. Create Vertex Buffer (Single Quad facing Z)
        Vertex vertices[] = {
            // Pos              // Normal
            {{-1, -1, 0}, {0, 0, 1}},
            {{ 1, -1, 0}, {0, 0, 1}},
            {{-1,  1, 0}, {0, 0, 1}},
            {{ 1,  1, 0}, {0, 0, 1}},
        };
        
        BufferDesc vbDesc{};
        vbDesc.size = sizeof(vertices);
        vbDesc.type = BufferType::Vertex;
        vbDesc.usage = GPUMemoryUsage::Static;
        vbDesc.memoryUsage = GPUMemoryUsage::Static;
        vbDesc.vertex.vertexCount = 4;
        vbDesc.vertex.vertexStride = sizeof(Vertex);
        
        ResourceHandle vbHandle = device.CreateBuffer(vbDesc);
        TEST_ASSERT(vbHandle != handles::INVALID_RESOURCE, "Failed to create vertex buffer");
        
        // Upload data
        void* vbData = device.MapBuffer(vbHandle, 0, sizeof(vertices));
        if(vbData) {
            memcpy(vbData, vertices, sizeof(vertices));
            device.UnmapBuffer(vbHandle);
        } else {
             TEST_ASSERT(false, "Failed to map vertex buffer");
        }

        // 6. Create Uniform Buffer
        Uniforms uniforms;
        // Identity matrices for full screen quad
        uniforms.modelViewProjectionMatrix = matrix_identity_float4x4;
        uniforms.normalMatrix = matrix_identity_float4x4;
        
        // Setup SH Environment (Ambient White Light)
        float intensity = 0.1f;
        float c0 = 0.28209479f;
        for(int i=0; i<9; ++i) uniforms.envSH[i] = {0,0,0};
        uniforms.envSH[0] = {intensity * c0, intensity * c0, intensity * c0};
        
        BufferDesc ubDesc{};
        ubDesc.size = sizeof(Uniforms);
        ubDesc.type = BufferType::Constant;
        ubDesc.usage = GPUMemoryUsage::Dynamic;
        ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        
        ResourceHandle ubHandle = device.CreateBuffer(ubDesc);
        TEST_ASSERT(ubHandle != handles::INVALID_RESOURCE, "Failed to create uniform buffer");
        
        // Upload data
        void* ubData = device.MapBuffer(ubHandle, 0, sizeof(Uniforms));
        if(ubData) {
            memcpy(ubData, &uniforms, sizeof(Uniforms));
            device.UnmapBuffer(ubHandle);
        } else {
            TEST_ASSERT(false, "Failed to map uniform buffer");
        }

        // 7. Create Output Texture
        TextureDesc texDesc;
        texDesc.size = {64, 64, 1};
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.type = TextureType::Texture2D;
        texDesc.usage = TextureUsage::RenderTarget;
        texDesc.memoryUsage = GPUMemoryUsage::Static;
        
        ResourceHandle texHandle = device.CreateTexture(texDesc);
        TEST_ASSERT(texHandle != handles::INVALID_RESOURCE, "Failed to create texture");

        // 7.5 Create and Update Descriptor Set
        DescriptorSetDesc setDesc;
        setDesc.layout = setLayout;
        DescriptorSetHandle descriptorSet = device.CreateDescriptorSet(setDesc);
        TEST_ASSERT(descriptorSet != handles::INVALID_DESCRIPTOR_SET, "Failed to create descriptor set");

        DescriptorBufferInfo bufferInfo;
        bufferInfo.buffer = ubHandle;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(Uniforms);

        WriteDescriptorSet write;
        write.dstSet = descriptorSet;
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = DescriptorType::UniformBuffer;
        write.bufferInfo = &bufferInfo;
        write.imageInfo = nullptr;
        
        device.UpdateDescriptorSets(1, &write);

        // 8. Render
        device.BeginFrame();
        CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device.GetCommandBuffer(cmdHandle);
        
        cmdBuffer->Begin();
        
        RenderPassDesc passDesc;
        RenderPassDesc::Attachment attachment;
        attachment.texture = texHandle;
        attachment.loadOp = LoadAction::Clear;
        attachment.storeOp = StoreAction::Store;
        attachment.clearValue = ClearValue{primal::math::v4{0, 0, 0, 1}};
        passDesc.colorAttachments.push_back(attachment);
        
        cmdBuffer->BeginRenderPass(passDesc);
        cmdBuffer->BindGraphicsPipeline(pipelineHandle);
        
        uint64_t vbOffset = 0;
        ResourceHandle vbHandles[] = {vbHandle};
        cmdBuffer->BindVertexBuffers(0, 1, vbHandles, &vbOffset);
        
        ResourceHandle sets[] = {descriptorSet};
        cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, sets, 0, nullptr);
        
        // Draw Quad
        cmdBuffer->Draw(4, 1, 0, 0);
        
        cmdBuffer->EndRenderPass();
        cmdBuffer->End();
        
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdHandle;
        device.Submit(submitInfo);
        
        device.EndFrame();
        device.WaitIdle();

        // 9. Cleanup
        device.DestroyDescriptorSet(descriptorSet);
        device.DestroyDescriptorSetLayout(setLayout);
        device.DestroyPipelineLayout(pipelineLayout);
        device.DestroyShader(vsHandle);
        device.DestroyShader(fsHandle);
        device.DestroyPipeline(pipelineHandle);
        device.DestroyBuffer(vbHandle);
        device.DestroyBuffer(ubHandle);
        device.DestroyTexture(texHandle);
        
        device.Shutdown();
        return TestResult::Passed;
    }

    TestResult TestPRTBakedRendering()
    {
        // 1. Initialize Device
        DeviceDesc deviceDesc;
        deviceDesc.platform = RHIPlatform::Metal;
        deviceDesc.enableDebug = true;

        MetalDevice device(deviceDesc);
        if (!device.Initialize())
        {
            return TestResult::Failed;
        }

        // 2. Prepare Shader Source
        std::string shaderSource = GetCombinedShaderSource("TestPRTBaked.metal");
        if (shaderSource.empty())
        {
            return TestResult::Failed;
        }

        // 3. Create Shaders
        ShaderHandle vsHandle = device.CreateShader(shaderSource.c_str(), shaderSource.length(), ShaderStage::Vertex, "prt_baked_vertex");
        ShaderHandle fsHandle = device.CreateShader(shaderSource.c_str(), shaderSource.length(), ShaderStage::Pixel, "prt_baked_fragment");

        TEST_ASSERT(vsHandle != handles::INVALID_SHADER, "Failed to create vertex shader");
        TEST_ASSERT(fsHandle != handles::INVALID_SHADER, "Failed to create fragment shader");

        // 4. Create Pipeline
        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vsHandle;
        pipelineDesc.pixelShader = fsHandle;
        
        // Vertex Layout
        // Binding 0: Pos, Normal
        VertexInputBinding binding0;
        binding0.binding = 0;
        binding0.stride = sizeof(Vertex);
        binding0.perVertex = true;
        pipelineDesc.vertexBindings.push_back(binding0);

        // Binding 1: Transfer SH
        VertexInputBinding binding1;
        binding1.binding = 1;
        binding1.stride = sizeof(TransferSH);
        binding1.perVertex = true;
        pipelineDesc.vertexBindings.push_back(binding1);

        // Attributes
        // Attr 0: Position
        VertexInputAttribute attrPos;
        attrPos.location = 0;
        attrPos.binding = 0;
        attrPos.format = DataFormat::RGB32_Float;
        attrPos.offset = offsetof(Vertex, position);
        pipelineDesc.vertexAttributes.push_back(attrPos);

        // Attr 1: Normal
        VertexInputAttribute attrNorm;
        attrNorm.location = 1;
        attrNorm.binding = 0;
        attrNorm.format = DataFormat::RGB32_Float;
        attrNorm.offset = offsetof(Vertex, normal);
        pipelineDesc.vertexAttributes.push_back(attrNorm);

        // Attr 2: SH0
        VertexInputAttribute attrSH0;
        attrSH0.location = 2;
        attrSH0.binding = 1;
        attrSH0.format = DataFormat::RGB32_Float;
        attrSH0.offset = offsetof(TransferSH, sh0);
        pipelineDesc.vertexAttributes.push_back(attrSH0);

        // Attr 3: SH1
        VertexInputAttribute attrSH1;
        attrSH1.location = 3;
        attrSH1.binding = 1;
        attrSH1.format = DataFormat::RGB32_Float;
        attrSH1.offset = offsetof(TransferSH, sh1);
        pipelineDesc.vertexAttributes.push_back(attrSH1);

        // Attr 4: SH2
        VertexInputAttribute attrSH2;
        attrSH2.location = 4;
        attrSH2.binding = 1;
        attrSH2.format = DataFormat::RGB32_Float;
        attrSH2.offset = offsetof(TransferSH, sh2);
        pipelineDesc.vertexAttributes.push_back(attrSH2);

        pipelineDesc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.depthStencilFormat = DataFormat::Unknown; 

        // 4.1 Create Descriptor Set Layout
        DescriptorSetLayoutBinding layoutBinding;
        layoutBinding.binding = 1; // Match shader binding
        layoutBinding.descriptorType = DescriptorType::UniformBuffer;
        layoutBinding.descriptorCount = 1;
        layoutBinding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

        DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 1;
        layoutDesc.bindings = &layoutBinding;
        DescriptorSetLayoutHandle setLayout = device.CreateDescriptorSetLayout(layoutDesc);
        TEST_ASSERT(setLayout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "Failed to create descriptor set layout");

        // 4.2 Create Pipeline Layout
        PipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.setLayoutCount = 1;
        pipelineLayoutDesc.setLayouts = &setLayout;
        PipelineLayoutHandle pipelineLayout = device.CreatePipelineLayout(pipelineLayoutDesc);
        TEST_ASSERT(pipelineLayout != handles::INVALID_PIPELINE_LAYOUT, "Failed to create pipeline layout");

        pipelineDesc.layout = pipelineLayout;

        PipelineHandle pipelineHandle = device.CreateGraphicsPipeline(pipelineDesc);
        TEST_ASSERT(pipelineHandle != handles::INVALID_PIPELINE, "Failed to create pipeline");

        // 5. Generate Geometry & Bake
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        CreateSphere(0.5f, 16, 16, vertices, indices);

        // Convert for Baker
        std::vector<primal::math::v3> bakerPos;
        std::vector<primal::math::v3> bakerNorm;
        for(const auto& v : vertices) {
            bakerPos.push_back(v.position);
            bakerNorm.push_back(v.normal);
        }

        PRTBakingDesc bakeDesc;
        bakeDesc.positions = bakerPos.data();
        bakeDesc.normals = bakerNorm.data();
        bakeDesc.vertex_count = (uint32_t)bakerPos.size();
        bakeDesc.indices = indices.data();
        bakeDesc.index_count = (uint32_t)indices.size();
        bakeDesc.num_samples = 100; // Fast bake for test

        primal::utl::vector<SH9> bakedCoeffs;
        bool bakeResult = PRTBaker::BakeShadowedTransfer(bakeDesc, bakedCoeffs);
        TEST_ASSERT(bakeResult, "PRT Baking failed");

        // Convert Baked Coeffs to TransferSH buffer
        std::vector<TransferSH> transferData;
        for(const auto& sh : bakedCoeffs) {
            TransferSH t;
            t.sh0 = {sh.coeffs[0], sh.coeffs[1], sh.coeffs[2]};
            t.sh1 = {sh.coeffs[3], sh.coeffs[4], sh.coeffs[5]};
            t.sh2 = {sh.coeffs[6], sh.coeffs[7], sh.coeffs[8]};
            transferData.push_back(t);
        }

        // 6. Create Buffers
        // Vertex Buffer
        BufferDesc vbDesc{};
        vbDesc.size = vertices.size() * sizeof(Vertex);
        vbDesc.type = BufferType::Vertex;
        vbDesc.usage = GPUMemoryUsage::Static;
        vbDesc.memoryUsage = GPUMemoryUsage::Static;
        
        ResourceHandle vbHandle = device.CreateBuffer(vbDesc);
        void* vbPtr = device.MapBuffer(vbHandle, 0, vbDesc.size);
        if(vbPtr) {
            memcpy(vbPtr, vertices.data(), vbDesc.size);
            device.UnmapBuffer(vbHandle);
        }

        // Index Buffer
        BufferDesc ibDesc{};
        ibDesc.size = indices.size() * sizeof(uint32_t);
        ibDesc.type = BufferType::Index;
        ibDesc.usage = GPUMemoryUsage::Static;
        ibDesc.memoryUsage = GPUMemoryUsage::Static;
        
        ResourceHandle ibHandle = device.CreateBuffer(ibDesc);
        void* ibPtr = device.MapBuffer(ibHandle, 0, ibDesc.size);
        if(ibPtr) {
            memcpy(ibPtr, indices.data(), ibDesc.size);
            device.UnmapBuffer(ibHandle);
        }

        // Transfer Buffer
        BufferDesc tbDesc{};
        tbDesc.size = transferData.size() * sizeof(TransferSH);
        tbDesc.type = BufferType::Vertex;
        tbDesc.usage = GPUMemoryUsage::Static;
        tbDesc.memoryUsage = GPUMemoryUsage::Static;

        ResourceHandle tbHandle = device.CreateBuffer(tbDesc);
        void* tbPtr = device.MapBuffer(tbHandle, 0, tbDesc.size);
        if(tbPtr) {
            memcpy(tbPtr, transferData.data(), tbDesc.size);
            device.UnmapBuffer(tbHandle);
        }

        // 7. Uniforms
        Uniforms uniforms;
        uniforms.modelViewProjectionMatrix = matrix_identity_float4x4;
        uniforms.normalMatrix = matrix_identity_float4x4;

        // Lighting
        float intensity = 1.0f;
        float c0 = 0.28209479f;
        for(int i=0; i<9; ++i) uniforms.envSH[i] = {0,0,0};
        uniforms.envSH[0] = {intensity * c0, intensity * c0, intensity * c0};

        BufferDesc ubDesc{};
        ubDesc.size = sizeof(Uniforms);
        ubDesc.type = BufferType::Constant;
        ubDesc.usage = GPUMemoryUsage::Dynamic;
        ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        
        ResourceHandle ubHandle = device.CreateBuffer(ubDesc);
        void* ubPtr = device.MapBuffer(ubHandle, 0, ubDesc.size);
        if(ubPtr) {
            memcpy(ubPtr, &uniforms, ubDesc.size);
            device.UnmapBuffer(ubHandle);
        }

        // 8. Texture
        TextureDesc texDesc;
        texDesc.size = {64, 64, 1};
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.type = TextureType::Texture2D;
        texDesc.usage = TextureUsage::RenderTarget;
        texDesc.memoryUsage = GPUMemoryUsage::Static;
        ResourceHandle texHandle = device.CreateTexture(texDesc);

        // 9. Descriptor Set
        DescriptorSetDesc setDesc;
        setDesc.layout = setLayout;
        DescriptorSetHandle descriptorSet = device.CreateDescriptorSet(setDesc);

        DescriptorBufferInfo bufferInfo;
        bufferInfo.buffer = ubHandle;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(Uniforms);

        WriteDescriptorSet write;
        write.dstSet = descriptorSet;
        write.dstBinding = 1;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = DescriptorType::UniformBuffer;
        write.bufferInfo = &bufferInfo;
        write.imageInfo = nullptr;
        
        device.UpdateDescriptorSets(1, &write);

        // 10. Render
        device.BeginFrame();
        CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device.GetCommandBuffer(cmdHandle);
        
        cmdBuffer->Begin();
        
        RenderPassDesc passDesc;
        RenderPassDesc::Attachment attachment;
        attachment.texture = texHandle;
        attachment.loadOp = LoadAction::Clear;
        attachment.storeOp = StoreAction::Store;
        attachment.clearValue = ClearValue{primal::math::v4{0, 0, 0, 1}};
        passDesc.colorAttachments.push_back(attachment);
        
        cmdBuffer->BeginRenderPass(passDesc);
        cmdBuffer->BindGraphicsPipeline(pipelineHandle);
        
        uint64_t vbOffsets[] = {0, 0};
        ResourceHandle vbHandles[] = {vbHandle, tbHandle};
        cmdBuffer->BindVertexBuffers(0, 2, vbHandles, vbOffsets);

        ResourceHandle sets[] = {descriptorSet};
        cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout, 0, 1, sets, 0, nullptr);
        
        cmdBuffer->BindIndexBuffer(ibHandle, DataFormat::R32_UInt, 0);
        cmdBuffer->DrawIndexed(indices.size(), 1, 0, 0, 0);
        
        cmdBuffer->EndRenderPass();
        cmdBuffer->End();
        
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdHandle;
        device.Submit(submitInfo);
        
        device.EndFrame();
        device.WaitIdle();

        // Cleanup
        device.DestroyDescriptorSet(descriptorSet);
        device.DestroyDescriptorSetLayout(setLayout);
        device.DestroyPipelineLayout(pipelineLayout);
        device.DestroyShader(vsHandle);
        device.DestroyShader(fsHandle);
        device.DestroyPipeline(pipelineHandle);
        device.DestroyBuffer(vbHandle);
        device.DestroyBuffer(ibHandle);
        device.DestroyBuffer(tbHandle);
        device.DestroyBuffer(ubHandle);
        device.DestroyTexture(texHandle);
        
        device.Shutdown();
        return TestResult::Passed;
    }
}

void RegisterMetalPRTTests(Engine::Test::TestSuite& suite)
{
    suite.AddTestCase(Engine::Test::TestCase("Metal PRT Rendering", TestPRTRendering));
    suite.AddTestCase(Engine::Test::TestCase("Metal PRT Baked Rendering", TestPRTBakedRendering));
}

int main()
{
    Engine::Test::TestSuite suite("Metal PRT Tests");
    RegisterMetalPRTTests(suite);
    auto stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
