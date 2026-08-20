#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Graphics/RHI/Platforms/Metal/MetalShader.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#include "Graphics/RHI/Platforms/Metal/MetalDescriptorSet.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// ... existing code ...

// 测试命令缓冲区的创建和销毁
TestResult TestCommandBufferCreation() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    // 创建图形命令缓冲区
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
        device.Shutdown();
        return TestResult::Failed;
    }

    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);
    if (!cmdBuf) {
        device.DestroyCommandBuffer(cmdHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    device.DestroyCommandBuffer(cmdHandle);
    device.Shutdown();
    return TestResult::Passed;
}

// 测试命令缓冲区的记录和提交
TestResult TestCommandBufferRecording() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
        device.Shutdown();
        return TestResult::Failed;
    }

    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);

    // 开始记录
    if (!cmdBuf->Begin()) {
        device.DestroyCommandBuffer(cmdHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 结束记录
    if (!cmdBuf->End()) {
        device.DestroyCommandBuffer(cmdHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 提交
    if (!cmdBuf->Submit()) {
        device.DestroyCommandBuffer(cmdHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 等待完成
    if (!cmdBuf->WaitForCompletion()) {
        device.DestroyCommandBuffer(cmdHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    device.DestroyCommandBuffer(cmdHandle);
    device.Shutdown();
    return TestResult::Passed;
}

// 测试Buffer拷贝命令
TestResult TestCommandBufferCopyBuffer() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    // 创建源缓冲区
    BufferDesc srcDesc{};
    srcDesc.size = 256;
    srcDesc.type = BufferType::Raw;
    srcDesc.memoryUsage = GPUMemoryUsage::Dynamic; // 使用Dynamic以便CPU写入

    ResourceHandle srcHandle = device.CreateBuffer(srcDesc);
    if (srcHandle == handles::INVALID_RESOURCE) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 写入数据
    std::vector<uint32_t> srcData(64);
    for (size_t i = 0; i < srcData.size(); ++i) srcData[i] = i;
    device.GetBuffer(srcHandle)->UpdateData(srcData.data(), srcData.size() * sizeof(uint32_t), 0);

    // 创建目标缓冲区
    BufferDesc dstDesc{};
    dstDesc.size = 256;
    dstDesc.type = BufferType::Raw;
    dstDesc.memoryUsage = GPUMemoryUsage::Staging; // 使用Staging以便CPU读取验证

    ResourceHandle dstHandle = device.CreateBuffer(dstDesc);
    if (dstHandle == handles::INVALID_RESOURCE) {
        device.DestroyBuffer(srcHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 创建命令缓冲区
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Transfer); // 使用Transfer队列
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) {
        device.DestroyBuffer(srcHandle);
        device.DestroyBuffer(dstHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);

    cmdBuf->Begin();
    cmdBuf->CopyBuffer(srcHandle, dstHandle, 0, 0, 256);
    cmdBuf->End();
    cmdBuf->Submit();
    cmdBuf->WaitForCompletion();

    // 验证数据
    void* mappedData = device.GetBuffer(dstHandle)->Map(0, 256);
    bool dataCorrect = true;
    if (mappedData) {
        uint32_t* dstPtr = static_cast<uint32_t*>(mappedData);
        for (size_t i = 0; i < 64; ++i) {
            if (dstPtr[i] != srcData[i]) {
                dataCorrect = false;
                break;
            }
        }
        device.GetBuffer(dstHandle)->Unmap();
    } else {
        dataCorrect = false;
    }

    device.DestroyCommandBuffer(cmdHandle);
    device.DestroyBuffer(srcHandle);
    device.DestroyBuffer(dstHandle);
    device.Shutdown();

    return dataCorrect ? TestResult::Passed : TestResult::Failed;
}

// 测试RenderPass编码
TestResult TestRenderPassEncoding() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    // 创建颜色附件纹理
    TextureDesc colorDesc;
    colorDesc.size = {800, 600, 1};
    colorDesc.format = DataFormat::BGRA8_UNorm;
    colorDesc.usage = TextureUsage::RenderTarget;
    
    ResourceHandle colorTex = device.CreateTexture(colorDesc);
    if (colorTex == handles::INVALID_RESOURCE) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 创建RenderPass描述符
    RenderPassDesc passDesc;
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = colorTex;
    passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    passDesc.colorAttachments[0].storeOp = StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = ClearValue{primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f}};
    
    passDesc.viewport.size = {800.0f, 600.0f};
    passDesc.scissor.extent = {800, 600};

    // 创建命令缓冲区
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);

    cmdBuf->Begin();
    cmdBuf->BeginRenderPass(passDesc);
    
    // 设置视口和剪裁 (在BeginRenderPass中已经自动设置，这里测试再次设置)
    ViewportDesc vp;
    vp.size = {400.0f, 300.0f};
    cmdBuf->SetViewport(vp);
    
    primal::graphics::rhi::Rect scissor;
    scissor.extent = {400, 300};
    cmdBuf->SetScissor(scissor);
    
    cmdBuf->EndRenderPass();
    cmdBuf->End();
    cmdBuf->Submit();
    cmdBuf->WaitForCompletion();

    device.DestroyCommandBuffer(cmdHandle);
    device.DestroyTexture(colorTex);
    device.Shutdown();
    
    return TestResult::Passed;
}

// 测试GenerateMipmaps
TestResult TestGenerateMipmaps() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    // 1. 创建带Mipmaps的纹理
    TextureDesc texDesc;
    texDesc.size = {64, 64, 1};
    texDesc.format = DataFormat::BGRA8_UNorm;
    // Metal生成Mipmaps要求TextureUsage::RenderTarget和TextureUsage::ShaderResource
    texDesc.usage = static_cast<TextureUsage>(static_cast<uint32_t>(TextureUsage::RenderTarget) | static_cast<uint32_t>(TextureUsage::ShaderResource));
    texDesc.mipLevels = 3; // 64, 32, 16
    texDesc.type = TextureType::Texture2D;

    ResourceHandle texHandle = device.CreateTexture(texDesc);
    if (texHandle == handles::INVALID_RESOURCE) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 2. 创建源缓冲区 (Level 0: 64x64)
    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t bpp = 4;
    uint32_t dataSize = width * height * bpp;

    BufferDesc srcBufDesc{};
    srcBufDesc.size = dataSize;
    srcBufDesc.type = BufferType::Raw;
    srcBufDesc.memoryUsage = GPUMemoryUsage::Dynamic; // CPU Write

    ResourceHandle srcBufHandle = device.CreateBuffer(srcBufDesc);
    if (srcBufHandle == handles::INVALID_RESOURCE) {
        device.DestroyTexture(texHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 填充红色 (BGRA: B=0, G=0, R=255, A=255) -> 0xFFFF0000
    std::vector<uint32_t> srcData(width * height, 0xFFFF0000);
    device.GetBuffer(srcBufHandle)->UpdateData(srcData.data(), dataSize, 0);

    // 3. 创建回读缓冲区 (Level 1: 32x32)
    uint32_t level1Width = 32;
    uint32_t level1Height = 32;
    uint32_t level1Size = level1Width * level1Height * bpp;

    BufferDesc dstBufDesc{};
    dstBufDesc.size = level1Size;
    dstBufDesc.type = BufferType::Raw;
    dstBufDesc.memoryUsage = GPUMemoryUsage::Staging; // CPU Read

    ResourceHandle dstBufHandle = device.CreateBuffer(dstBufDesc);
    if (dstBufHandle == handles::INVALID_RESOURCE) {
        device.DestroyBuffer(srcBufHandle);
        device.DestroyTexture(texHandle);
        device.Shutdown();
        return TestResult::Failed;
    }

    // 4. 记录命令
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);

    cmdBuf->Begin();

    // Copy Buffer -> Texture (Level 0)
    BufferTextureCopyRegion region;
    region.bufferOffset = 0;
    region.bufferRowLength = width;
    region.bufferImageHeight = height;
    // region.imageSubresource.aspect = TextureAspect::Color; // RHI define has no aspect
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    cmdBuf->CopyBufferToTexture(srcBufHandle, texHandle, &region, 1);

    // Generate Mipmaps
    cmdBuf->GenerateMipmaps(texHandle);

    // Copy Texture (Level 1) -> Buffer
    BufferTextureCopyRegion regionReadback;
    regionReadback.bufferOffset = 0;
    regionReadback.bufferRowLength = level1Width;
    regionReadback.bufferImageHeight = level1Height;
    // regionReadback.imageSubresource.aspect = TextureAspect::Color;
    regionReadback.imageSubresource.mipLevel = 1;
    regionReadback.imageSubresource.baseArrayLayer = 0;
    regionReadback.imageSubresource.layerCount = 1;
    regionReadback.imageOffset = {0, 0, 0};
    regionReadback.imageExtent = {level1Width, level1Height, 1};

    cmdBuf->CopyTextureToBuffer(texHandle, dstBufHandle, &regionReadback, 1);

    cmdBuf->End();
    cmdBuf->Submit();
    cmdBuf->WaitForCompletion();

    // 5. 验证数据
    void* mappedData = device.GetBuffer(dstBufHandle)->Map(0, level1Size);
    bool dataCorrect = true;
    if (mappedData) {
        uint32_t* dstPtr = static_cast<uint32_t*>(mappedData);
        // 检查Level 1数据，应该是红色
        for (size_t i = 0; i < level1Width * level1Height; ++i) {
            if (dstPtr[i] != 0xFFFF0000) {
                dataCorrect = false;
                break;
            }
        }
        device.GetBuffer(dstBufHandle)->Unmap();
    } else {
        dataCorrect = false;
    }

    device.DestroyCommandBuffer(cmdHandle);
    device.DestroyBuffer(srcBufHandle);
    device.DestroyBuffer(dstBufHandle);
    device.DestroyTexture(texHandle);
    device.Shutdown();

    return dataCorrect ? TestResult::Passed : TestResult::Failed;
}

// 测试描述符数组绑定
TestResult TestDescriptorArrayBinding() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Skipped;
    }

    // 1. 准备Shader源码
    const char* shaderSource = R"(
        #include <metal_stdlib>
        using namespace metal;
        
        struct VertexOut {
            float4 position [[position]];
            float2 texCoord;
        };
        
        vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
            VertexOut out;
            // 全屏三角形
            float2 positions[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
            out.position = float4(positions[vertexID], 0.0, 1.0);
            out.texCoord = positions[vertexID] * 0.5 + 0.5;
            return out;
        }
        
        fragment float4 fragmentMain(VertexOut in [[stage_in]],
                                     array<texture2d<float>, 2> textures [[texture(0)]],
                                     sampler smp [[sampler(0)]]) {
            // 采样两个纹理并混合
            float4 c1 = textures[0].sample(smp, in.texCoord);
            float4 c2 = textures[1].sample(smp, in.texCoord);
            return (c1 + c2) * 0.5;
        }
    )";

    // 2. 创建Shader
    ShaderHandle vs = device.CreateShader(shaderSource, strlen(shaderSource), ShaderStage::Vertex, "vertexMain");
    ShaderHandle ps = device.CreateShader(shaderSource, strlen(shaderSource), ShaderStage::Pixel, "fragmentMain");

    if (vs == handles::INVALID_SHADER || ps == handles::INVALID_SHADER) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 3. 创建描述符集布局 (Binding 0: Texture Array [2])
    DescriptorSetLayoutBinding binding;
    binding.binding = 0;
    binding.descriptorType = DescriptorType::CombinedImageSampler;
    binding.descriptorCount = 2; 
    binding.stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &binding;

    DescriptorSetLayoutHandle dsl = device.CreateDescriptorSetLayout(dslDesc);
    if (dsl == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 4. 创建管线布局
    PipelineLayoutDesc plDesc;
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    
    PipelineLayoutHandle pl = device.CreatePipelineLayout(plDesc);
    if (pl == handles::INVALID_PIPELINE_LAYOUT) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 5. 创建管线
    GraphicsPipelineDesc psoDesc;
    psoDesc.vertexShader = vs;
    psoDesc.pixelShader = ps;
    psoDesc.layout = pl;
    psoDesc.renderTargetCount = 1;
    psoDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    psoDesc.topology = PrimitiveTopology::TriangleList;
    
    PipelineHandle pso = device.CreateGraphicsPipeline(psoDesc);
    if (pso == handles::INVALID_PIPELINE) {
        device.Shutdown();
        return TestResult::Failed;
    }

    // 6. 创建纹理和采样器
    TextureDesc texDesc;
    texDesc.size = {64, 64, 1};
    texDesc.format = DataFormat::BGRA8_UNorm;
    texDesc.usage = TextureUsage::ShaderResource;
    texDesc.type = TextureType::Texture2D;
    
    ResourceHandle tex1 = device.CreateTexture(texDesc);
    ResourceHandle tex2 = device.CreateTexture(texDesc);
    
    SamplerDesc smpDesc;
    smpDesc.minFilter = FilterMode::Linear;
    smpDesc.magFilter = FilterMode::Linear;
    SamplerHandle smp = device.CreateSampler(smpDesc);

    // 7. 创建并更新描述符集
    DescriptorSetDesc dsDesc;
    dsDesc.layout = dsl;
    DescriptorSetHandle ds = device.CreateDescriptorSet(dsDesc);
    
    DescriptorImageInfo imgInfos[2];
    imgInfos[0].imageView = tex1;
    imgInfos[0].sampler = smp;
    imgInfos[1].imageView = tex2;
    imgInfos[1].sampler = smp;
    
    WriteDescriptorSet write;
    write.dstSet = ds;
    write.dstBinding = 0;
    write.descriptorCount = 2;
    write.descriptorType = DescriptorType::CombinedImageSampler;
    write.imageInfo = imgInfos;
    
    device.UpdateDescriptorSets(1, &write);

    // 8. 创建渲染目标
    TextureDesc rtDesc;
    rtDesc.size = {64, 64, 1};
    rtDesc.format = DataFormat::BGRA8_UNorm;
    rtDesc.usage = TextureUsage::RenderTarget;
    ResourceHandle rt = device.CreateTexture(rtDesc);

    // 9. 记录命令
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* cmdBuf = device.GetCommandBuffer(cmdHandle);
    
    RenderPassDesc passDesc;
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = rt;
    passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    passDesc.colorAttachments[0].storeOp = StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = ClearValue{primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f}};
    passDesc.viewport.size = {64.0f, 64.0f};
    passDesc.scissor.extent = {64, 64};

    cmdBuf->Begin();
    cmdBuf->BeginRenderPass(passDesc);
    cmdBuf->BindGraphicsPipeline(pso);
    cmdBuf->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    cmdBuf->Draw(3, 1, 0, 0);
    cmdBuf->EndRenderPass();
    cmdBuf->End();
    
    cmdBuf->Submit();
    cmdBuf->WaitForCompletion();

    // 10. 清理
    device.DestroyCommandBuffer(cmdHandle);
    device.DestroyTexture(rt);
    device.DestroyDescriptorSet(ds);
    device.DestroySampler(smp);
    device.DestroyTexture(tex1);
    device.DestroyTexture(tex2);
    device.DestroyPipelineLayout(pl);
    device.DestroyDescriptorSetLayout(dsl);
    device.DestroyShader(vs);
    device.DestroyShader(ps);
    
    device.Shutdown();
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalCommandBufferTests");
    suite->AddTestCase(TestCase("Creation", TestCommandBufferCreation));
    suite->AddTestCase(TestCase("Recording", TestCommandBufferRecording));
    suite->AddTestCase(TestCase("CopyBuffer", TestCommandBufferCopyBuffer));
    suite->AddTestCase(TestCase("RenderPassEncoding", TestRenderPassEncoding));
    suite->AddTestCase(TestCase("GenerateMipmaps", TestGenerateMipmaps));
    suite->AddTestCase(TestCase("DescriptorArrayBinding", TestDescriptorArrayBinding));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}
