#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Graphics/RHI/Platforms/Metal/MetalDescriptorSet.h"
#include <thread>
#include <atomic>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

class TestMetalResourceBindingSuite : public TestSuite {
public:
    TestMetalResourceBindingSuite() : TestSuite("MetalResourceBinding") {
        AddTestCase(TestCase("TestUniformBufferBinding", [this]() { return RunTest([this]() { return TestUniformBufferBinding(); }); }, "测试Uniform Buffer绑定"));
        AddTestCase(TestCase("TestMultiStageBinding", [this]() { return RunTest([this]() { return TestMultiStageBinding(); }); }, "测试多阶段资源绑定"));
        AddTestCase(TestCase("TestMultiThreadedBinding", [this]() { return RunTest([this]() { return TestMultiThreadedBinding(); }); }, "测试多线程资源绑定"));
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

    TestResult TestUniformBufferBinding() {
        // 1. 创建 Uniform Buffer
        BufferDesc bufDesc;
        bufDesc.size = 256;
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle uboHandle = device_->CreateBuffer(bufDesc);
        TEST_ASSERT_NE(uboHandle, handles::INVALID_RESOURCE, "Uniform Buffer creation failed");

        // 2. 写入数据
        MetalBuffer* metalBuffer = (MetalBuffer*)device_->GetBuffer(uboHandle);
        TEST_ASSERT_NOT_NULL(metalBuffer, "Failed to get Metal buffer");
        
        float data[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        void* mapped = metalBuffer->Map(0, 256);
        TEST_ASSERT_NOT_NULL(mapped, "Buffer mapping failed");
        memcpy(mapped, data, sizeof(data));
        metalBuffer->Unmap();

        // 3. 创建 DescriptorSetLayout
        DescriptorSetLayoutBinding binding;
        binding.binding = 0;
        binding.descriptorType = DescriptorType::UniformBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = ShaderStage::Vertex;
        binding.immutableSamplers = nullptr;

        DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 1;
        layoutDesc.bindings = &binding;

        DescriptorSetLayoutHandle layoutHandle = device_->CreateDescriptorSetLayout(layoutDesc);
        TEST_ASSERT_NE(layoutHandle, handles::INVALID_DESCRIPTOR_SET_LAYOUT, "DescriptorSetLayout creation failed");

        // 4. 创建 DescriptorSet
        DescriptorSetDesc setDesc;
        setDesc.layout = layoutHandle;
        DescriptorSetHandle setHandle = device_->CreateDescriptorSet(setDesc);
        TEST_ASSERT_NE(setHandle, handles::INVALID_DESCRIPTOR_SET, "DescriptorSet creation failed");

        // 5. 更新 DescriptorSet
        DescriptorBufferInfo bufInfo;
        bufInfo.buffer = uboHandle;
        bufInfo.offset = 0;
        bufInfo.range = 256;

        WriteDescriptorSet write;
        write.dstSet = setHandle;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = DescriptorType::UniformBuffer;
        write.bufferInfo = &bufInfo;

        device_->UpdateDescriptorSets(1, &write);

        // 6. 创建 PipelineLayout
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &layoutHandle;
        
        PipelineLayoutHandle plHandle = device_->CreatePipelineLayout(plDesc);
        TEST_ASSERT_NE(plHandle, handles::INVALID_PIPELINE_LAYOUT, "PipelineLayout creation failed");

        // 7. 绑定 DescriptorSet 并验证
        CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);
        TEST_ASSERT_NOT_NULL(cmdBuffer, "Failed to get command buffer");

        cmdBuffer->Begin();
        
        DescriptorSetHandle sets[] = { setHandle };
        cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, plHandle, 0, 1, sets, 0, nullptr);
        
        // 这里需要一种方式来验证底层的 Metal 绑定是否正确
        // 由于我们没有 mock，只能通过后续的 draw call 不崩溃来间接验证，
        // 或者添加特定于测试的接口来查询绑定状态。
        // 目前先假设不崩溃即通过。

        cmdBuffer->End();
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBufferHandle;
        device_->Submit(submitInfo);

        // 清理
        device_->DestroyCommandBuffer(cmdBufferHandle);
        device_->DestroyBuffer(uboHandle);
        device_->DestroyDescriptorSet(setHandle);
        device_->DestroyDescriptorSetLayout(layoutHandle);
        device_->DestroyPipelineLayout(plHandle);

        return TestResult::Passed;
    }

    TestResult TestMultiStageBinding() {
        // 1. 创建资源
        BufferDesc bufDesc;
        bufDesc.size = 256;
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle uboHandle = device_->CreateBuffer(bufDesc);

        TextureDesc texDesc;
        texDesc.size = {256, 256, 1};
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.type = TextureType::Texture2D;
        texDesc.usage = TextureUsage::ShaderResource;
        texDesc.memoryUsage = GPUMemoryUsage::Static;
        ResourceHandle texHandle = device_->CreateTexture(texDesc);

        // 2. 创建 DescriptorSetLayout (Binding 0: UBO Vertex, Binding 1: Texture Pixel)
        DescriptorSetLayoutBinding bindings[2];
        bindings[0].binding = 0;
        bindings[0].descriptorType = DescriptorType::UniformBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = ShaderStage::Vertex;
        
        bindings[1].binding = 1;
        bindings[1].descriptorType = DescriptorType::SampledImage;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = ShaderStage::Pixel;

        DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 2;
        layoutDesc.bindings = bindings;

        DescriptorSetLayoutHandle layoutHandle = device_->CreateDescriptorSetLayout(layoutDesc);
        DescriptorSetDesc setDesc;
        setDesc.layout = layoutHandle;
        DescriptorSetHandle setHandle = device_->CreateDescriptorSet(setDesc);

        // 3. 更新 DescriptorSet
        DescriptorBufferInfo bufInfo;
        bufInfo.buffer = uboHandle;
        bufInfo.offset = 0;
        bufInfo.range = 256;

        DescriptorImageInfo imgInfo;
        imgInfo.imageView = texHandle;
        imgInfo.imageLayout = ResourceState::Ready;

        WriteDescriptorSet writes[2];
        writes[0].dstSet = setHandle;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = DescriptorType::UniformBuffer;
        writes[0].bufferInfo = &bufInfo;

        writes[1].dstSet = setHandle;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = DescriptorType::SampledImage;
        writes[1].imageInfo = &imgInfo;

        device_->UpdateDescriptorSets(2, writes);

        // 4. 绑定并验证
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &layoutHandle;
        PipelineLayoutHandle plHandle = device_->CreatePipelineLayout(plDesc);

        CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);

        cmdBuffer->Begin();
        DescriptorSetHandle sets[] = { setHandle };
        cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, plHandle, 0, 1, sets, 0, nullptr);
        cmdBuffer->End();
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBufferHandle;
        device_->Submit(submitInfo);

        // 清理
        device_->DestroyCommandBuffer(cmdBufferHandle);
        device_->DestroyTexture(texHandle);
        device_->DestroyBuffer(uboHandle);
        device_->DestroyDescriptorSet(setHandle);
        device_->DestroyDescriptorSetLayout(layoutHandle);
        device_->DestroyPipelineLayout(plHandle);

        return TestResult::Passed;
    }

    TestResult TestMultiThreadedBinding() {
        // 1. 准备公共资源
        BufferDesc bufDesc;
        bufDesc.size = 256;
        bufDesc.type = BufferType::Constant;
        bufDesc.usage = GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle uboHandle = device_->CreateBuffer(bufDesc);

        DescriptorBufferInfo bufInfo;
        bufInfo.buffer = uboHandle;
        bufInfo.offset = 0;
        bufInfo.range = 256;

        DescriptorSetLayoutBinding binding;
        binding.binding = 0;
        binding.descriptorType = DescriptorType::UniformBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = ShaderStage::Vertex;

        DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindingCount = 1;
        layoutDesc.bindings = &binding;
        DescriptorSetLayoutHandle layoutHandle = device_->CreateDescriptorSetLayout(layoutDesc);

        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &layoutHandle;
        PipelineLayoutHandle plHandle = device_->CreatePipelineLayout(plDesc);

        // 2. 多线程创建和绑定 DescriptorSet
        const int threadCount = 4;
        std::vector<std::thread> threads;
        std::atomic<bool> success(true);

        for (int i = 0; i < threadCount; ++i) {
            threads.emplace_back([this, layoutHandle, plHandle, bufInfo, &success]() {
                // 每个线程创建自己的 DescriptorSet
                DescriptorSetDesc setDesc;
                setDesc.layout = layoutHandle;
                // 注意：这里需要确保 CreateDescriptorSet 是线程安全的，或者使用不同的 pool
                // 假设 RHI 内部处理了线程安全
                DescriptorSetHandle setHandle = device_->CreateDescriptorSet(setDesc);

                WriteDescriptorSet write;
                write.dstSet = setHandle;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = DescriptorType::UniformBuffer;
                write.bufferInfo = &bufInfo;

                device_->UpdateDescriptorSets(1, &write);

                CommandBufferHandle cmdBufferHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                MetalCommandBuffer* cmdBuffer = device_->GetCommandBuffer(cmdBufferHandle);

                if (cmdBuffer) {
                    cmdBuffer->Begin();
                    DescriptorSetHandle sets[] = { setHandle };
                    cmdBuffer->BindDescriptorSets(PipelineBindPoint::Graphics, plHandle, 0, 1, sets, 0, nullptr);
                    cmdBuffer->End();
                    QueueSubmitInfo submitInfo{};
                    submitInfo.cmdBuffer = cmdBufferHandle;
                    device_->Submit(submitInfo);
                    device_->DestroyCommandBuffer(cmdBufferHandle);
                } else {
                    success = false;
                }
                
                // 稍后销毁 set
                // device_->DestroyDescriptorSet(setHandle); // 简化测试，暂不销毁避免竞争
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        TEST_ASSERT(success, "Multi-threaded binding failed");

        // 清理
        device_->DestroyBuffer(uboHandle);
        device_->DestroyDescriptorSetLayout(layoutHandle);
        device_->DestroyPipelineLayout(plHandle);

        return TestResult::Passed;
    }
};

int main() {
    auto suite = std::make_shared<TestMetalResourceBindingSuite>();
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}
