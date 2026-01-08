/**
 * @file TestMetalDescriptorSet.cpp
 * @brief Metal 描述符集测试
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 1.0
 */

#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDescriptorSet.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestDescriptorSetUpdate() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Failed;
    }

    // 1. Create Buffer
    BufferDesc bufferDesc;
    bufferDesc.size = 256;
    bufferDesc.type = BufferType::Constant;
    bufferDesc.usage = GPUMemoryUsage::Staging;
    ResourceHandle buffer = device.CreateBuffer(bufferDesc);
    TEST_ASSERT(buffer != handles::INVALID_RESOURCE, "Failed to create uniform buffer");

    // 2. Create Layout
    DescriptorSetLayoutBinding binding;
    binding.binding = 0;
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = ShaderStage::Vertex;

    DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindingCount = 1;
    layoutDesc.bindings = &binding;
    ResourceHandle layout = device.CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_RESOURCE, "Failed to create descriptor set layout");

    // 3. Create Set
    DescriptorSetDesc setDesc;
    setDesc.layout = layout;
    ResourceHandle set = device.CreateDescriptorSet(setDesc);
    TEST_ASSERT(set != handles::INVALID_RESOURCE, "Failed to create descriptor set");

    // 4. Update Set
    DescriptorBufferInfo bufferInfo;
    bufferInfo.buffer = buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = 256;

    WriteDescriptorSet write;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = DescriptorType::UniformBuffer;
    write.descriptorCount = 1;
    write.bufferInfo = &bufferInfo;
    write.imageInfo = nullptr;

    device.UpdateDescriptorSets(1, &write);

    // 5. Verify internal state
    MetalDescriptorSet* mtlSet = device.GetDescriptorSet(set);
    TEST_ASSERT(mtlSet != nullptr, "Failed to get MetalDescriptorSet pointer");
    
    const auto& bindings = mtlSet->GetBindings();
    TEST_ASSERT(bindings.size() == 1, "Binding count mismatch");
    TEST_ASSERT(bindings.count(0) == 1, "Binding 0 not found");
    TEST_ASSERT(bindings.at(0).resource == buffer, "Buffer handle mismatch");
    TEST_ASSERT(bindings.at(0).type == DescriptorType::UniformBuffer, "Descriptor type mismatch");

    // 6. Bind (requires CommandBuffer)
    CommandBufferHandle cmdHandle = device.CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmdHandle != handles::INVALID_COMMAND_BUFFER, "Failed to create command buffer");
    
    MetalCommandBuffer* cmd = device.GetCommandBuffer(cmdHandle);
    TEST_ASSERT(cmd != nullptr, "Failed to get command buffer pointer");
    
    cmd->Begin();
    
    // We need to begin a render pass to bind graphics descriptors
    RenderPassDesc passDesc;
    // Just minimal setup to satisfy validation if any
    cmd->BeginRenderPass(passDesc);
    
    // Bind the set
    // Note: BindDescriptorSets takes a PipelineLayoutHandle, but Metal implementation currently ignores it
    // passing handles::INVALID_PIPELINE_LAYOUT is safe
    DescriptorSetHandle sets[] = { set };
    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, handles::INVALID_PIPELINE_LAYOUT, 0, 1, sets, 0, nullptr);
    
    cmd->EndRenderPass();
    cmd->End();

    // Clean up
    device.DestroyCommandBuffer(cmdHandle);
    device.DestroyDescriptorSet(set);
    device.DestroyDescriptorSetLayout(layout);
    device.DestroyBuffer(buffer);
    device.Shutdown();

    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalDescriptorSetTests");
    suite->AddTestCase(TestCase("TestDescriptorSetUpdate", TestDescriptorSetUpdate));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    
    return 0;
}
