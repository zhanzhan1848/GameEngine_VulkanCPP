/**
 * @file TestRHIAdaptiveMemoryPool.cpp
 * @brief RHI自适应内存池系统单元测试
 * @details 基于项目自定义测试框架，验证自适应内存池的核心功能，特别是句柄映射和内存块管理
 * 
 * @author Engine开发团队
 * @date 2026-01-08
 * @version 1.1
 */

#include "../../TestFramework.h"
#include "CommonHeaders.h"
#include "Engine/Graphics/RHI/Core/RHIAdaptiveMemoryPool.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// === Compatibility Macros ===
#define TEST_ASSERT_TRUE(cond, msg) TEST_ASSERT(cond, msg)
#define TEST_ASSERT_FALSE(cond, msg) TEST_ASSERT(!(cond), msg)
#define TEST_SECTION(name) std::cout << "\n[SECTION] " << name << std::endl;

// === Mock Device Implementation ===
class MockRHIDevice : public RHIDeviceBase {
public:
    DeviceInfo info;
    DeviceDesc desc;

    MockRHIDevice() {
        info.platform = RHIPlatform::Metal;
        info.dedicatedVideoMemory = 1024 * 1024 * 1024; // 1GB
        info.sharedSystemMemory = 8 * 1024 * 1024 * 1024ULL; // 8GB
    }

    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { return info; }
    const DeviceDesc& GetDesc() const override { return desc; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool Submit(const QueueSubmitInfo& info) override { return true; }
    SyncHandle CreateSync() override { return SyncHandle{}; }
    bool WaitForSync(SyncHandle handle, u32 timeoutMs) override { return true; }
    void DestroySync(SyncHandle handle) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override { return QueryPoolHandle{}; }
    void DestroyQueryPool(QueryPoolHandle handle) override {}
    SamplerHandle CreateSampler(const SamplerDesc& desc) override { return SamplerHandle{}; }
    void DestroySampler(SamplerHandle handle) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override { return DescriptorSetLayoutHandle{}; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override { return DescriptorSetHandle{}; }
    void DestroyDescriptorSet(DescriptorSetHandle handle) override {}
    void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override { return PipelineLayoutHandle{}; }
    void DestroyPipelineLayout(PipelineLayoutHandle handle) override {}
    
    // Implement missing pure virtuals
    RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) override { return nullptr; }
    void DestroySwapChain(RHISwapChain* swapChain) override {}
    
    ResourceHandle CreateBuffer(const BufferDesc& desc) override { return handles::INVALID_RESOURCE; }
    ResourceHandle CreateTexture(const TextureDesc& desc) override { return handles::INVALID_RESOURCE; }
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type) override { return handles::INVALID_COMMAND_BUFFER; }
    void DestroyBuffer(ResourceHandle handle) override {}
    void DestroyTexture(ResourceHandle handle) override {}
    ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") override { return handles::INVALID_SHADER; }
    void DestroyShader(ShaderHandle handle) override {}
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override { return handles::INVALID_PIPELINE; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle handle) override {}
    void* MapBuffer(ResourceHandle handle, u64 offset = 0, u64 size = 0) override { return nullptr; }
    void UnmapBuffer(ResourceHandle handle) override {}
    
    RHIGarbageCollector& GetGarbageCollector() override {
        static RHIGarbageCollector gc;
        return gc;
    }
};

// === 测试用例 ===

TestResult TestInitialization() {
    TEST_SECTION("Initialization");
    
    MockRHIDevice device;
    MemoryPoolDesc desc;
    desc.poolSize = 1024 * 1024; // 1MB
    desc.blockSize = 256;
    
    RHIAdaptiveMemoryPool pool(device, desc);
    
    TEST_ASSERT_TRUE(pool.Initialize(), "Pool initialization should succeed");
    
    return TestResult::Passed;
}

TestResult TestAllocationAndHandleMapping() {
    TEST_SECTION("Allocation and Handle Mapping");
    
    MockRHIDevice device;
    MemoryPoolDesc desc;
    desc.poolSize = 1024 * 1024; // 1MB
    
    RHIAdaptiveMemoryPool pool(device, desc);
    pool.Initialize();
    
    // 分配内存
    u32 handle1 = pool.Allocate(1024, 256, GPUMemoryUsage::Dynamic);
    TEST_ASSERT_TRUE(handle1 != 0, "Allocation should return valid handle");
    
    // 验证句柄映射
    MemoryBlock block1 = pool.GetMemoryBlock(handle1);
    TEST_ASSERT_EQ(block1.size, 1024, "Block size should match allocation");
    TEST_ASSERT_EQ(static_cast<u32>(block1.state), static_cast<u32>(MemoryBlockState::Allocated), "Block state should be Allocated");
    
    // 分配第二个块
    u32 handle2 = pool.Allocate(512, 256, GPUMemoryUsage::Static);
    TEST_ASSERT_TRUE(handle2 != 0, "Second allocation should return valid handle");
    TEST_ASSERT_TRUE(handle1 != handle2, "Handles should be unique");
    
    // 验证句柄映射
    MemoryBlock block2 = pool.GetMemoryBlock(handle2);
    TEST_ASSERT_EQ(block2.size, 512, "Block 2 size should match");
    
    return TestResult::Passed;
}

TestResult TestDeallocation() {
    TEST_SECTION("Deallocation");
    
    MockRHIDevice device;
    MemoryPoolDesc desc;
    desc.poolSize = 1024 * 1024;
    
    RHIAdaptiveMemoryPool pool(device, desc);
    pool.Initialize();
    
    u32 handle = pool.Allocate(1024, 256, GPUMemoryUsage::Dynamic);
    TEST_ASSERT_TRUE(handle != 0, "Allocation should succeed");
    
    // 释放内存
    bool result = pool.Deallocate(handle);
    TEST_ASSERT_TRUE(result, "Deallocation should succeed");
    
    // 验证释放后无法获取块信息（或者获取到无效/空闲块）
    // 注意：具体行为取决于实现，通常句柄映射会被移除
    MemoryBlock block = pool.GetMemoryBlock(handle);
    // 如果映射被移除，GetMemoryBlock应该返回默认构造的MemoryBlock (size=0)
    TEST_ASSERT_EQ(block.size, 0, "Block should not be retrievalbe after deallocation");
    
    return TestResult::Passed;
}

TestResult TestDefragmentation() {
    TEST_SECTION("Defragmentation (Simulated)");
    
    MockRHIDevice device;
    MemoryPoolDesc desc;
    desc.poolSize = 1024 * 1024;
    
    RHIAdaptiveMemoryPool pool(device, desc);
    pool.Initialize();
    
    // 分配多个块
    u32 h1 = pool.Allocate(1024);
    u32 h2 = pool.Allocate(1024);
    u32 h3 = pool.Allocate(1024);
    
    // 释放中间的块，制造空洞
    pool.Deallocate(h2);
    
    // 触发整理
    // 注意：Defragment的具体触发条件可能很复杂，这里直接调用如果它是public的，
    // 或者我们假设Allocate会在需要时触发。
    // RHIAdaptiveMemoryPool::Defragment 是 public 的。
    bool defragResult = pool.Defragment();
    // 当前实现暂时禁用碎片整理，应返回 false
    TEST_ASSERT_FALSE(defragResult, "Defragment is currently disabled");
    
    // 验证未释放的块仍然有效且可访问
    MemoryBlock b1 = pool.GetMemoryBlock(h1);
    TEST_ASSERT_EQ(b1.size, 1024, "Block 1 should still be accessible");
    
    MemoryBlock b3 = pool.GetMemoryBlock(h3);
    TEST_ASSERT_EQ(b3.size, 1024, "Block 3 should still be accessible");
    
    return TestResult::Passed;
}

int main() {
    std::cout << "Running TestRHIAdaptiveMemoryPool..." << std::endl;
    
    bool allPassed = true;
    
    if (TestInitialization() != TestResult::Passed) allPassed = false;
    if (TestAllocationAndHandleMapping() != TestResult::Passed) allPassed = false;
    if (TestDeallocation() != TestResult::Passed) allPassed = false;
    if (TestDefragmentation() != TestResult::Passed) allPassed = false;
    
    if (allPassed) {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    } else {
        std::cerr << "Some tests failed!" << std::endl;
        return 1;
    }
}
