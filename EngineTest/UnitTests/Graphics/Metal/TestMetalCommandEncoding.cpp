#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

class TestMetalCommandEncodingSuite : public TestSuite {
public:
    TestMetalCommandEncodingSuite() : TestSuite("MetalCommandEncoding") {
        AddTestCase(TestCase("TestRenderPassEncoding", [this]() { return RunTest([this]() { return TestRenderPassEncoding(); }); }, "测试RenderPass编码"));
        AddTestCase(TestCase("TestBlitEncoding", [this]() { return RunTest([this]() { return TestBlitEncoding(); }); }, "测试Blit编码"));
        AddTestCase(TestCase("TestEncoderSwitching", [this]() { return RunTest([this]() { return TestEncoderSwitching(); }); }, "测试Encoder自动切换"));
    }

private:
    MetalDevice* device_ = nullptr;

    void SetUp() {
        DeviceDesc desc;
        desc.platform = RHIPlatform::Metal;
        device_ = new MetalDevice(desc);
        device_->Initialize();
    }

    void TearDown() {
        if (device_) {
            device_->Shutdown();
            delete device_;
            device_ = nullptr;
        }
    }

    TestResult RunTest(std::function<TestResult()> testFunc) {
        SetUp();
        TestResult result = testFunc();
        TearDown();
        return result;
    }

    TestResult TestRenderPassEncoding() {
        std::cout << "[Test] Starting TestRenderPassEncoding" << std::endl;
        // 1. 创建 CommandBuffer
        CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);
        TEST_ASSERT_NOT_NULL(cmdBuffer, "Failed to create command buffer");

        // 2. 准备 RenderPassDesc
        RenderPassDesc passDesc;
        RenderPassDesc::Attachment colorAtt;
        // 这里我们不需要真正的 Texture，只要 RenderPass 不崩溃即可
        // 但 Metal 可能会校验 Texture 是否存在。为了稳妥，创建一个简单的 Texture。
        
        TextureDesc texDesc;
        texDesc.type = TextureType::Texture2D;
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.size = {256, 256, 1};
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.usage = static_cast<TextureUsage>(
            static_cast<uint32_t>(TextureUsage::RenderTarget) | 
            static_cast<uint32_t>(TextureUsage::CopySource) | 
            static_cast<uint32_t>(TextureUsage::CopyDest)
        );
        ResourceHandle texHandle = device_->CreateTexture(texDesc);
        TEST_ASSERT_NE(texHandle, handles::INVALID_RESOURCE, "Failed to create texture");
        std::cout << "[Test] Texture created" << std::endl;

        colorAtt.texture = texHandle;
        colorAtt.loadOp = LoadAction::Clear;
        colorAtt.storeOp = StoreAction::Store;
        colorAtt.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 1.0f);
        passDesc.colorAttachments.push_back(colorAtt);

        passDesc.viewport.topLeft = { 0, 0 };
        passDesc.viewport.size = { 256, 256 };
        passDesc.viewport.minDepth = 0.0f;
        passDesc.viewport.maxDepth = 1.0f;

        passDesc.scissor.offset = { 0, 0 };
        passDesc.scissor.extent = { 256, 256 };

        // 3. 编码 RenderPass
        cmdBuffer->Begin();
        std::cout << "[Test] CommandBuffer Begin called" << std::endl;

        std::cout << "[Test] Calling BeginRenderPass" << std::endl;
        cmdBuffer->BeginRenderPass(passDesc);
        std::cout << "[Test] BeginRenderPass called" << std::endl;
        
        // 设置 Viewport 和 Scissor (已经在 BeginRenderPass 中自动设置，这里再次手动设置以验证)
        cmdBuffer->SetViewport(passDesc.viewport);
        cmdBuffer->SetScissor(passDesc.scissor);

        // Dummy Draw (没有绑定 Pipeline 和 Buffer，可能会有 Validation Error，但在 Unit Test 中只要不 Crash 就算通过)
        // 注意：Metal 验证层可能会报错，但我们主要测试 CommandBuffer 的状态流转
        // cmdBuffer->Draw(3, 0, 1, 0);
        // std::cout << "[Test] Draw called" << std::endl;

        cmdBuffer->EndRenderPass();
        std::cout << "[Test] EndRenderPass called" << std::endl;

        cmdBuffer->End();
        std::cout << "[Test] CommandBuffer End called" << std::endl;

        // 4. 提交
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdBufferHandle;
        device_->Submit(submitInfo);
        std::cout << "[Test] Submit called" << std::endl;
        
        // 清理
        device_->DestroyTexture(texHandle);
        device_->DestroyCommandBuffer(cmdBufferHandle);

        return TestResult::Passed;
    }

    TestResult TestBlitEncoding() {
        std::cout << "[Test] Starting TestBlitEncoding" << std::endl;
        // 1. 创建 CommandBuffer
        CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);
        TEST_ASSERT_NOT_NULL(cmdBuffer, "Failed to create command buffer");

        ResourceHandle buf1 = device_->CreateBuffer(BufferDesc{ 256, BufferType::Vertex, GPUMemoryUsage::Dynamic });
        ResourceHandle buf2 = device_->CreateBuffer(BufferDesc{ 256, BufferType::Vertex, GPUMemoryUsage::Dynamic });
        TEST_ASSERT_NE(buf1, handles::INVALID_RESOURCE, "Failed to create buffer 1");
        TEST_ASSERT_NE(buf2, handles::INVALID_RESOURCE, "Failed to create buffer 2");

        cmdBuffer->Begin();
        
        // 2. 编码 Blit 命令 (CopyBuffer)
        // 这应该自动开始一个 BlitCommandEncoder
        cmdBuffer->CopyBuffer(buf1, buf2, 0, 0, 256);
        std::cout << "[Test] CopyBuffer called" << std::endl;

        cmdBuffer->End();
        std::cout << "[Test] CommandBuffer End called" << std::endl;
        
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdBufferHandle;
        device_->Submit(submitInfo);
        std::cout << "[Test] Submit called" << std::endl;
        
        device_->DestroyBuffer(buf1);
        device_->DestroyBuffer(buf2);
        device_->DestroyCommandBuffer(cmdBufferHandle);

        return TestResult::Passed;
    }

    TestResult TestEncoderSwitching() {
        std::cout << "[Test] Starting TestEncoderSwitching" << std::endl;
        // 1. 创建 CommandBuffer
        CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);
        
        TextureDesc texDesc;
        texDesc.type = TextureType::Texture2D;
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.size = {256, 256, 1};
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.usage = static_cast<TextureUsage>(
            static_cast<uint32_t>(TextureUsage::RenderTarget) | 
            static_cast<uint32_t>(TextureUsage::CopySource) | 
            static_cast<uint32_t>(TextureUsage::CopyDest)
        );
        ResourceHandle texHandle = device_->CreateTexture(texDesc);

        RenderPassDesc passDesc;
        RenderPassDesc::Attachment colorAtt;
        colorAtt.texture = texHandle;
        colorAtt.loadOp = LoadAction::Clear;
        colorAtt.storeOp = StoreAction::Store;
        colorAtt.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 1.0f);
        passDesc.colorAttachments.push_back(colorAtt);
        
        passDesc.viewport.topLeft = { 0, 0 };
        passDesc.viewport.size = { 256, 256 };
        passDesc.viewport.minDepth = 0.0f;
        passDesc.viewport.maxDepth = 1.0f;

        passDesc.scissor.offset = { 0, 0 };
        passDesc.scissor.extent = { 256, 256 };

        ResourceHandle buf1 = device_->CreateBuffer(BufferDesc{ 256, BufferType::Vertex, GPUMemoryUsage::Dynamic });
        ResourceHandle buf2 = device_->CreateBuffer(BufferDesc{ 256, BufferType::Vertex, GPUMemoryUsage::Dynamic });

        cmdBuffer->Begin();

        // 1. Render Encoder
        std::cout << "[Test] Switching to Render Encoder" << std::endl;
        cmdBuffer->BeginRenderPass(passDesc);
        // cmdBuffer->Draw(3, 0, 1, 0); // Skip Draw
        cmdBuffer->EndRenderPass();

        // 2. Blit Encoder (Switch from Render)
        // CopyBuffer 内部会调用 getBlitEncoder，这会结束之前的 Render Encoder (如果还没结束) 并开始 Blit Encoder
        // 注意：EndRenderPass 只是标记 RenderPass 逻辑结束，真正的 Encoder 结束可能延迟到需要切换时
        // 但 MetalCommandBuffer::EndRenderPass 实际上调用了 endCurrentEncoder()，所以这里应该已经是 None 状态
        std::cout << "[Test] Switching to Blit Encoder" << std::endl;
        cmdBuffer->CopyBuffer(buf1, buf2, 0, 0, 256);

        // 3. Compute Encoder (Switch from Blit)
        std::cout << "[Test] Switching to Compute Encoder" << std::endl;
        ResourceHandle buffers[] = { buf1 };
        uint64_t offsets[] = { 0 };
        cmdBuffer->BindComputeBuffers(0, 1, buffers, offsets);

        // 4. Render Encoder (Switch from Compute)
        std::cout << "[Test] Switching back to Render Encoder" << std::endl;
        cmdBuffer->BeginRenderPass(passDesc);
        // cmdBuffer->Draw(3, 0, 1, 0); // Skip Draw
        cmdBuffer->EndRenderPass();

        cmdBuffer->End();
        
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdBufferHandle;
        device_->Submit(submitInfo);
        std::cout << "[Test] Submit called" << std::endl;

        device_->DestroyBuffer(buf1);
        device_->DestroyBuffer(buf2);
        device_->DestroyTexture(texHandle);
        device_->DestroyCommandBuffer(cmdBufferHandle);

        return TestResult::Passed;
    }
};

int main() {
    TestMetalCommandEncodingSuite suite;
    suite.RunAllTests();
    return 0;
}
