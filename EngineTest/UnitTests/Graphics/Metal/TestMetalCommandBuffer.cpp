#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Graphics/RHI/Platforms/Metal/MetalBuffer.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

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
    BufferDesc srcDesc;
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
    BufferDesc dstDesc;
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

int main() {
    auto suite = std::make_shared<TestSuite>("MetalCommandBufferTests");
    suite->AddTestCase(TestCase("Creation", TestCommandBufferCreation));
    suite->AddTestCase(TestCase("Recording", TestCommandBufferRecording));
    suite->AddTestCase(TestCase("CopyBuffer", TestCommandBufferCopyBuffer));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}
