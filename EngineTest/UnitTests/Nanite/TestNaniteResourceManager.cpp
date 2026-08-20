#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "Engine/Graphics/RHI/Core/RHIDevice.h"

using namespace Engine::Test;
using namespace primal::graphics;
using namespace primal::graphics::nanite;
using namespace primal::id;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;
using Engine::Test::TestStats;
using primal::graphics::nanite::NaniteResourceManager;
using primal::graphics::nanite::NaniteRuntimeResource;

namespace {
// 与 TestNaniteStreamingManager 相同的 Mock 模式：NaniteResourceManager 在
// dev 上改为必须 Initialize(device) 才允许 GetOrCreateResource（device_ 门槛），
// 单测无真实 GPU 设备，用空实现 mock 翻过门槛。
class MockRHIDeviceForResourceManager : public primal::graphics::rhi::RHIDeviceBase {
public:
    rhi::DeviceInfo deviceInfo_;
    bool valid_ = true;
    rhi::RHIGarbageCollector gc_;

    MockRHIDeviceForResourceManager() {
        std::strncpy(deviceInfo_.deviceName, "MockResourceManagerDevice",
                     sizeof(deviceInfo_.deviceName) - 1);
    }
    ~MockRHIDeviceForResourceManager() override = default;

    bool IsValid() const override { return valid_; }
    const rhi::DeviceInfo& GetDeviceInfo() const override { return deviceInfo_; }
    const rhi::DeviceDesc& GetDesc() const override { static rhi::DeviceDesc desc; return desc; }
    void WaitIdle() const override {}
    void Shutdown() override { valid_ = false; }
    bool Submit(const rhi::QueueSubmitInfo&) override { return true; }
    rhi::SyncHandle CreateSync() override { return (rhi::SyncHandle)1; }
    bool WaitForSync(rhi::SyncHandle, u32) override { return true; }
    void DestroySync(rhi::SyncHandle) override {}
    rhi::QueryPoolHandle CreateQueryPool(const rhi::QueryPoolDesc&) override { return rhi::handles::INVALID_QUERY_POOL; }
    void DestroyQueryPool(rhi::QueryPoolHandle) override {}
    bool GetQueryPoolResults(rhi::QueryPoolHandle, u32, u32, void*, size_t) override { return false; }
    rhi::SamplerHandle CreateSampler(const rhi::SamplerDesc&) override { return (rhi::SamplerHandle)1; }
    void DestroySampler(rhi::SamplerHandle) override {}
    rhi::DescriptorSetLayoutHandle CreateDescriptorSetLayout(const rhi::DescriptorSetLayoutDesc&) override { return (rhi::DescriptorSetLayoutHandle)1; }
    void DestroyDescriptorSetLayout(rhi::DescriptorSetLayoutHandle) override {}
    rhi::PipelineLayoutHandle CreatePipelineLayout(const rhi::PipelineLayoutDesc&) override { return (rhi::PipelineLayoutHandle)1; }
    void DestroyPipelineLayout(rhi::PipelineLayoutHandle) override {}
    rhi::DescriptorSetHandle CreateDescriptorSet(const rhi::DescriptorSetDesc&) override { return (rhi::DescriptorSetHandle)1; }
    void DestroyDescriptorSet(rhi::DescriptorSetHandle) override {}
    void UpdateDescriptorSets(u32, const rhi::WriteDescriptorSet*) override {}
    rhi::RHISwapChain* CreateSwapChain(const rhi::SwapChainDesc&) override { return nullptr; }
    void DestroySwapChain(rhi::RHISwapChain*) override {}
    rhi::ResourceHandle CreateBuffer(const rhi::BufferDesc&) override { return (rhi::ResourceHandle)1001; }
    rhi::ResourceHandle CreateTexture(const rhi::TextureDesc&) override { return (rhi::ResourceHandle)1; }
    rhi::ResourceHandle CreateTextureView(const rhi::TextureViewDesc&) override { return (rhi::ResourceHandle)1; }
    rhi::ShaderHandle CreateShader(const void*, size_t, rhi::ShaderStage, const char*) override { return (rhi::ShaderHandle)1; }
    rhi::PipelineHandle CreateGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override { return (rhi::PipelineHandle)1; }
    rhi::PipelineHandle CreateComputePipeline(const rhi::ComputePipelineDesc&) override { return (rhi::PipelineHandle)1; }
    rhi::RenderPassHandle CreateRenderPass(const rhi::RenderPassDesc&) override { return rhi::handles::INVALID_RENDER_PASS; }
    void DestroyRenderPass(rhi::RenderPassHandle) override {}
    rhi::CommandBufferHandle CreateCommandBuffer(rhi::CommandQueueType) override { return rhi::handles::INVALID_COMMAND_BUFFER; }
    void DestroyCommandBuffer(rhi::CommandBufferHandle) override {}
    void DestroyBuffer(rhi::ResourceHandle) override {}
    void DestroyTexture(rhi::ResourceHandle) override {}
    void DestroyShader(rhi::ShaderHandle) override {}
    void DestroyPipeline(rhi::PipelineHandle) override {}
    void* MapBuffer(rhi::ResourceHandle, u64, u64) override { return nullptr; }
    void UnmapBuffer(rhi::ResourceHandle) override {}
    double GetTimestampPeriod() const override { return 1.0; }
    rhi::RHIGarbageCollector& GetGarbageCollector() override { return gc_; }
    void SetBufferDirtySize(rhi::ResourceHandle, u64) override {}
    rhi::RHIPlatform GetPlatform() const override { return rhi::RHIPlatform::Unknown; }
};

MockRHIDeviceForResourceManager g_mockDevice;


TestResult TestReferenceCountBasic() {
    auto& manager = NaniteResourceManager::Get();
    
    manager.Shutdown();
    manager.Initialize(&g_mockDevice);
    
    const id_type test_geometry_id = 1;
    
    auto* resource = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");
    
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Initial ref count should be 1");
    
    manager.AddGeometryRef(test_geometry_id);
    TEST_ASSERT_EQ(2u, resource->ref_count.load(), "Ref count should be 2 after add");
    
    manager.ReleaseGeometryRef(test_geometry_id);
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Ref count should be 1 after release");
    
    manager.ReleaseGeometryRef(test_geometry_id);
    
    auto* resourceAfterRelease = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_EQ(resourceAfterRelease, nullptr, 
        "Resource should return nullptr after final release");
    
    manager.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestReferenceCountMultiInstance() {
    auto& manager = NaniteResourceManager::Get();
    
    manager.Shutdown();
    manager.Initialize(&g_mockDevice);
    
    const id_type geometry_id = 100;
    
    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");
    
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Initial ref count should be 1");
    
    for (u32 i = 0; i < 10; ++i) {
        manager.AddGeometryRef(geometry_id);
    }
    TEST_ASSERT_EQ(11u, resource->ref_count.load(), "Ref count should be 11 after 10 adds");
    
    for (u32 i = 0; i < 5; ++i) {
        manager.ReleaseGeometryRef(geometry_id);
    }
    TEST_ASSERT_EQ(6u, resource->ref_count.load(), "Ref count should be 6 after 5 releases");
    
    manager.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestReferenceCountThreadSafety() {
    auto& manager = NaniteResourceManager::Get();
    
    manager.Shutdown();
    manager.Initialize(&g_mockDevice);
    
    const id_type geometry_id = 200;
    constexpr u32 num_threads = 8;
    constexpr u32 iterations_per_thread = 1000;
    
    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");
    
    // Test 1: Concurrent AddRef operations
    std::vector<std::thread> add_threads;
    std::atomic<u32> total_adds{0};
    
    for (u32 t = 0; t < num_threads; ++t) {
        add_threads.emplace_back([&]() {
            for (u32 i = 0; i < iterations_per_thread; ++i) {
                manager.AddGeometryRef(geometry_id);
                total_adds++;
            }
        });
    }
    
    for (auto& thread : add_threads) {
        thread.join();
    }
    
    TEST_ASSERT_EQ(num_threads * iterations_per_thread, total_adds.load(), 
        "Total adds should match expected count");
    
    const u32 expected_count_after_adds = 1 + (num_threads * iterations_per_thread);
    TEST_ASSERT_EQ(expected_count_after_adds, resource->ref_count.load(),
        "Ref count should be correct after concurrent adds");
    
    // Test 2: Concurrent Release operations (release half of the refs)
    std::vector<std::thread> release_threads;
    std::atomic<u32> total_releases{0};
    constexpr u32 releases_per_thread = iterations_per_thread / 2;
    
    for (u32 t = 0; t < num_threads; ++t) {
        release_threads.emplace_back([&]() {
            for (u32 i = 0; i < releases_per_thread; ++i) {
                manager.ReleaseGeometryRef(geometry_id);
                total_releases++;
            }
        });
    }
    
    for (auto& thread : release_threads) {
        thread.join();
    }
    
    TEST_ASSERT_EQ(num_threads * releases_per_thread, total_releases.load(),
        "Total releases should match expected count");
    
    const u32 expected_count_after_releases = expected_count_after_adds - (num_threads * releases_per_thread);
    TEST_ASSERT_EQ(expected_count_after_releases, resource->ref_count.load(),
        "Ref count should be correct after concurrent releases");
    
    // Test 3: Concurrent AddRef and Release on multiple resources
    constexpr u32 num_resources = 4;
    id_type resource_ids[num_resources] = {201, 202, 203, 204};
    
    for (u32 i = 0; i < num_resources; ++i) {
        auto* res = manager.GetOrCreateResource(resource_ids[i]);
        TEST_ASSERT_NOT_NULL(res, "Resource should be created");
    }
    
    std::vector<std::thread> mixed_threads;
    std::atomic<u32> total_mixed_ops{0};
    
    for (u32 t = 0; t < num_threads; ++t) {
        mixed_threads.emplace_back([&, t]() {
            for (u32 i = 0; i < iterations_per_thread; ++i) {
                u32 resource_idx = i % num_resources;
                if (i % 2 == 0) {
                    manager.AddGeometryRef(resource_ids[resource_idx]);
                } else {
                    manager.AddGeometryRef(resource_ids[resource_idx]);
                    manager.ReleaseGeometryRef(resource_ids[resource_idx]);
                }
                total_mixed_ops++;
            }
        });
    }
    
    for (auto& thread : mixed_threads) {
        thread.join();
    }
    
    TEST_ASSERT_EQ(num_threads * iterations_per_thread, total_mixed_ops.load(),
        "Total mixed operations should match expected count");
    
    manager.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestInvalidGeometryIdHandling() {
    auto& manager = NaniteResourceManager::Get();
    
    manager.Shutdown();
    manager.Initialize(&g_mockDevice);
    
    manager.AddGeometryRef(primal::id::invalid_id);
    
    manager.ReleaseGeometryRef(primal::id::invalid_id);
    
    manager.AddGeometryRef(99999);
    
    manager.ReleaseGeometryRef(99999);
    
    manager.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestIntegration1000InstancesShareOneResource() {
    auto& manager = NaniteResourceManager::Get();
    
    manager.Shutdown();
    manager.Initialize(&g_mockDevice);
    
    constexpr id_type shared_geometry_id = 1000;
    
    auto* sharedResource = manager.GetOrCreateResource(shared_geometry_id);
    TEST_ASSERT_NOT_NULL(sharedResource, "Shared resource should be created");
    
    TEST_ASSERT_EQ(1u, sharedResource->ref_count.load(), 
        "Shared resource ref count should be 1");
    
    for (u32 i = 0; i < 1000; ++i) {
        manager.AddGeometryRef(shared_geometry_id);
    }
    
    TEST_ASSERT_EQ(1001u, sharedResource->ref_count.load(), 
        "Shared resource ref count should be 1001");
    
    for (u32 i = 0; i < 1000; ++i) {
        manager.ReleaseGeometryRef(shared_geometry_id);
    }
    
    TEST_ASSERT_EQ(1u, sharedResource->ref_count.load(), 
        "Shared resource ref count should be 1");
    
    manager.Shutdown();
    
    return TestResult::Passed;
}

} // anonymous namespace

int main() {
    TestSuite suite("NaniteResourceManager Tests");
    
    suite.AddTestCase(TestCase("ReferenceCountBasic", TestReferenceCountBasic));
    suite.AddTestCase(TestCase("ReferenceCountMultiInstance", TestReferenceCountMultiInstance));
    suite.AddTestCase(TestCase("ReferenceCountThreadSafety", TestReferenceCountThreadSafety));
    suite.AddTestCase(TestCase("InvalidGeometryIdHandling", TestInvalidGeometryIdHandling));
    suite.AddTestCase(TestCase("Integration1000InstancesShareOneResource", TestIntegration1000InstancesShareOneResource));
    
    TestStats stats = suite.RunAllTests();
    
    std::cout << "\n=== NaniteResourceManager Tests ===" << std::endl;
    if (stats.failedTests == 0) {
        std::cout << "✅ All NaniteResourceManager tests passed!" << std::endl;
    } else {
        std::cout << "❌ " << stats.failedTests << " test(s) failed." << std::endl;
    }
    
    return stats.failedTests == 0 ? 0 : 1;
}