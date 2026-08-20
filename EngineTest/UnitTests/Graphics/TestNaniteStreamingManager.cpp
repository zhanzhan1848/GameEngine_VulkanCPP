#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Graphics/Nanite/NaniteStreamingManager.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <unordered_map>

using namespace Engine::Test;
using namespace primal::graphics::nanite;
using namespace primal::graphics;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

namespace {
    static int g_dummy_resource_manager = 0;
    
    void SetupMockResourceManager(NaniteStreamingManager& manager) {
        manager.SetNaniteResourceManager(
            reinterpret_cast<NaniteResourceManager*>(&g_dummy_resource_manager)
        );
    }
}

class MockRHIDeviceForStreaming : public rhi::RHIDeviceBase {
public:
    std::unordered_map<uint64_t, std::vector<uint8_t>> bufferStorage_;
    uint64_t nextBufferHandle_ = 2000;
    rhi::DeviceInfo deviceInfo_;
    bool valid_ = true;
    rhi::RHIGarbageCollector gc_;

    MockRHIDeviceForStreaming() {
        std::strncpy(deviceInfo_.deviceName, "MockStreamingDevice", sizeof(deviceInfo_.deviceName) - 1);
    }

    ~MockRHIDeviceForStreaming() override = default;
    
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
    rhi::ResourceHandle CreateBuffer(const rhi::BufferDesc& desc) override {
        rhi::ResourceHandle handle = (rhi::ResourceHandle)++nextBufferHandle_;
        bufferStorage_[(uint64_t)handle].resize(desc.size);
        return handle;
    }
    rhi::ResourceHandle CreateTexture(const rhi::TextureDesc&) override { return (rhi::ResourceHandle)1; }
    rhi::ResourceHandle CreateTextureView(const rhi::TextureViewDesc&) override { return (rhi::ResourceHandle)1; }
    rhi::ShaderHandle CreateShader(const void*, size_t, rhi::ShaderStage, const char*) override { return (rhi::ShaderHandle)1; }
    rhi::PipelineHandle CreateGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override { return (rhi::PipelineHandle)1; }
    rhi::PipelineHandle CreateComputePipeline(const rhi::ComputePipelineDesc&) override { return (rhi::PipelineHandle)1; }
    rhi::RenderPassHandle CreateRenderPass(const rhi::RenderPassDesc&) override { return rhi::handles::INVALID_RENDER_PASS; }
    void DestroyRenderPass(rhi::RenderPassHandle) override {}
    rhi::CommandBufferHandle CreateCommandBuffer(rhi::CommandQueueType) override { return rhi::handles::INVALID_COMMAND_BUFFER; }
    void DestroyCommandBuffer(rhi::CommandBufferHandle) override {}
    void DestroyBuffer(rhi::ResourceHandle handle) override {
        bufferStorage_.erase((uint64_t)handle);
    }
    void DestroyTexture(rhi::ResourceHandle) override {}
    void DestroyShader(rhi::ShaderHandle) override {}
    void DestroyPipeline(rhi::PipelineHandle) override {}
    void* MapBuffer(rhi::ResourceHandle handle, u64 offset, u64 size) override {
        auto it = bufferStorage_.find((uint64_t)handle);
        if (it != bufferStorage_.end() && offset + size <= it->second.size()) {
            return it->second.data() + offset;
        }
        return nullptr;
    }
    void UnmapBuffer(rhi::ResourceHandle) override {}
    double GetTimestampPeriod() const override { return 1.0; }
    rhi::RHIGarbageCollector& GetGarbageCollector() override { return gc_; }
    // stale-test port: pure virtuals added to RHIDeviceBase after the Dawn era
    void SetBufferDirtySize(rhi::ResourceHandle, u64) override {}
    rhi::RHIPlatform GetPlatform() const override { return rhi::RHIPlatform::Unknown; }
};

bool test_streaming_manager_initialization() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    config.page_size_bytes = 128 * 1024;
    config.max_requests_per_frame = 50;
    config.eviction_threshold = 0.75f;
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    const StreamingStats& stats = manager.GetStats();
    if (stats.current_resident_clusters != 0) {
        std::cout << "  FAILED: Initial resident count should be 0\n";
        return false;
    }
    manager.Shutdown();
    std::cout << "  PASSED: Initialization test\n";
    return true;
}

bool test_configurable_pool_size() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 32 * 1024 * 1024;
    config.page_size_bytes = 64 * 1024;
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization with custom pool size failed\n";
        return false;
    }
    const NaniteStreamingConfig& retrieved_config = manager.GetConfig();
    if (retrieved_config.page_pool_size_bytes != 32 * 1024 * 1024) {
        std::cout << "  FAILED: Pool size mismatch\n";
        return false;
    }
    if (retrieved_config.page_size_bytes != 64 * 1024) {
        std::cout << "  FAILED: Page size mismatch\n";
        return false;
    }
    manager.Shutdown();
    std::cout << "  PASSED: Configurable pool size test\n";
    return true;
}

bool test_lru_eviction_correctness() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024;
    config.page_size_bytes = 1 * 1024;
    config.eviction_threshold = 0.5f;
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < 10; ++i) {
        manager.RequestCluster(1, i, 100 - i, camera_pos);
    }
    manager.ProcessRequests(1);
    manager.UpdateLRU(1, camera_pos);
    manager.Shutdown();
    std::cout << "  PASSED: LRU eviction correctness test\n";
    return true;
}

bool test_cluster_residency_bitmap() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024 * 1024;
    config.page_size_bytes = 256 * 1024;
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    if (manager.IsClusterResident(1, 0)) {
        std::cout << "  FAILED: Cluster should not be resident initially\n";
        return false;
    }
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.ProcessRequests(1);
    if (!manager.IsClusterResident(1, 0)) {
        std::cout << "  FAILED: Cluster should be resident after loading\n";
        manager.Shutdown();
        return false;
    }
    manager.Shutdown();
    std::cout << "  PASSED: Cluster residency bitmap test\n";
    return true;
}

bool test_max_requests_per_frame_limit() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 16 * 1024 * 1024;
    config.page_size_bytes = 256 * 1024;
    config.max_requests_per_frame = 5;
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    for (u32 i = 0; i < 20; ++i) {
        manager.RequestCluster(1, i, i, camera_pos);
    }
    manager.ProcessRequests(1);
    const StreamingStats& stats = manager.GetStats();
    manager.Shutdown();
    std::cout << "  PASSED: Max requests per frame limit test\n";
    return true;
}

bool test_null_device_initialization() {
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (manager.Initialize(nullptr, config)) {
        std::cout << "  FAILED: Should return false with null device\n";
        return false;
    }
    
    std::cout << "  PASSED: Null device initialization test\n";
    return true;
}

bool test_zero_pool_size_initialization() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 0;
    config.page_size_bytes = 128 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  PASSED: Zero pool size initialization correctly rejected\n";
        return true;
    }
    
    manager.Shutdown();
    std::cout << "  FAILED: Should reject zero pool size\n";
    return false;
}

bool test_zero_page_size_initialization() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    config.page_size_bytes = 0;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  PASSED: Zero page size initialization correctly rejected\n";
        return true;
    }
    
    manager.Shutdown();
    std::cout << "  FAILED: Should reject zero page size\n";
    return false;
}

bool test_double_initialization() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: First initialization failed\n";
        return false;
    }
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Second initialization should return true (idempotent)\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Double initialization test\n";
    return true;
}

bool test_double_shutdown() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    
    manager.Shutdown();
    manager.Shutdown();
    
    std::cout << "  PASSED: Double shutdown test\n";
    return true;
}

bool test_use_after_shutdown() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    
    manager.Shutdown();
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    if (manager.RequestCluster(1, 0, 100, camera_pos)) {
        std::cout << "  FAILED: RequestCluster should return false after shutdown\n";
        return false;
    }
    
    std::cout << "  PASSED: Use after shutdown test\n";
    return true;
}

bool test_eviction_at_threshold() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024;
    config.page_size_bytes = 1 * 1024;
    config.eviction_threshold = 0.5f;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    for (u32 i = 0; i < 3; ++i) {
        manager.RequestCluster(1, i, 100 - i, camera_pos);
    }
    manager.ProcessRequests(1);
    
    manager.UpdateLRU(1, camera_pos);
    
    const StreamingStats& stats_after = manager.GetStats();
    
    if (stats_after.eviction_count == 0) {
        std::cout << "  FAILED: No eviction occurred when above threshold\n";
        manager.Shutdown();
        return false;
    }
    
    if (stats_after.total_clusters_evicted == 0) {
        std::cout << "  FAILED: Eviction count not updated\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Eviction at threshold test\n";
    return true;
}

bool test_lru_ordering_correctness() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024;
    config.page_size_bytes = 1 * 1024;
    config.eviction_threshold = 0.75f;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    for (u32 i = 0; i < 4; ++i) {
        manager.RequestCluster(1, i, 100, camera_pos);
    }
    manager.ProcessRequests(1);
    
    manager.RequestCluster(1, 2, 200, camera_pos);
    manager.RequestCluster(1, 3, 200, camera_pos);
    
    manager.RequestCluster(1, 4, 100, camera_pos);
    manager.ProcessRequests(4);
    manager.UpdateLRU(4, camera_pos);
    
    const StreamingStats& stats = manager.GetStats();
    
    if (stats.eviction_count == 0) {
        std::cout << "  FAILED: No eviction occurred\n";
        manager.Shutdown();
        return false;
    }
    
    if (manager.IsClusterResident(1, 2) && manager.IsClusterResident(1, 3)) {
        std::cout << "  PASSED: LRU ordering correctness test\n";
        manager.Shutdown();
        return true;
    }
    
    std::cout << "  FAILED: Recently accessed clusters were evicted\n";
    manager.Shutdown();
    return false;
}

bool test_max_requests_limit_enforcement() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    config.page_size_bytes = 256 * 1024;
    config.max_requests_per_frame = 5;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    for (u32 i = 0; i < 20; ++i) {
        manager.RequestCluster(1, i, i, camera_pos);
    }
    
    manager.ProcessRequests(1);
    
    const StreamingStats& stats_after = manager.GetStats();
    
    if (stats_after.total_clusters_streamed > 5) {
        std::cout << "  FAILED: Processed more than max_requests_per_frame (processed: " 
                  << stats_after.total_clusters_streamed << ")\n";
        manager.Shutdown();
        return false;
    }
    
    if (stats_after.total_clusters_streamed != 5) {
        std::cout << "  FAILED: Should have processed exactly 5 requests (processed: " 
                  << stats_after.total_clusters_streamed << ")\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Max requests limit enforcement test\n";
    return true;
}

bool test_statistics_tracking() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024 * 1024;
    config.page_size_bytes = 256 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    const StreamingStats& initial_stats = manager.GetStats();
    if (initial_stats.total_clusters_streamed != 0 || 
        initial_stats.current_resident_clusters != 0) {
        std::cout << "  FAILED: Initial statistics not zero\n";
        manager.Shutdown();
        return false;
    }
    
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.RequestCluster(1, 1, 90, camera_pos);
    manager.RequestCluster(1, 2, 80, camera_pos);
    manager.ProcessRequests(1);
    
    const StreamingStats& after_load = manager.GetStats();
    
    if (after_load.total_clusters_streamed != 3) {
        std::cout << "  FAILED: total_clusters_streamed should be 3 (got: " 
                  << after_load.total_clusters_streamed << ")\n";
        manager.Shutdown();
        return false;
    }
    
    if (after_load.current_resident_clusters != 3) {
        std::cout << "  FAILED: current_resident_clusters should be 3 (got: " 
                  << after_load.current_resident_clusters << ")\n";
        manager.Shutdown();
        return false;
    }
    
    if (after_load.pending_requests_count != 0) {
        std::cout << "  FAILED: pending_requests_count should be 0 after processing\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Statistics tracking test\n";
    return true;
}

bool test_pool_usage_tracking() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 4 * 1024;
    config.page_size_bytes = 1 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.RequestCluster(1, 1, 90, camera_pos);
    manager.ProcessRequests(1);
    
    manager.UpdateLRU(1, camera_pos);
    
    const StreamingStats& stats = manager.GetStats();
    
    if (stats.page_pool_usage < 40 || stats.page_pool_usage > 60) {
        std::cout << "  FAILED: page_pool_usage should be ~50% (got: " 
                  << stats.page_pool_usage << "%)\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Pool usage tracking test\n";
    return true;
}

bool test_buffer_handles_valid() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    
    rhi::ResourceHandle residency = manager.GetResidencyBuffer();
    rhi::ResourceHandle request = manager.GetRequestBuffer();
    rhi::ResourceHandle feedback = manager.GetFeedbackBuffer();
    
    if (residency == rhi::handles::INVALID_RESOURCE) {
        std::cout << "  FAILED: Residency buffer is invalid\n";
        manager.Shutdown();
        return false;
    }
    
    if (request == rhi::handles::INVALID_RESOURCE) {
        std::cout << "  FAILED: Request buffer is invalid\n";
        manager.Shutdown();
        return false;
    }
    
    if (feedback == rhi::handles::INVALID_RESOURCE) {
        std::cout << "  FAILED: Feedback buffer is invalid\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Buffer handles valid test\n";
    return true;
}

bool test_large_cluster_index() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    u32 large_index = 1000000;
    if (!manager.RequestCluster(1, large_index, 100, camera_pos)) {
        const StreamingStats& stats = manager.GetStats();
        if (stats.pending_requests_count != 1) {
            std::cout << "  FAILED: Large cluster index should be queued\n";
            manager.Shutdown();
            return false;
        }
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Large cluster index test\n";
    return true;
}

bool test_empty_pool_handling() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 1 * 1024;
    config.page_size_bytes = 1 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.ProcessRequests(1);
    
    if (!manager.IsClusterResident(1, 0)) {
        std::cout << "  FAILED: First cluster should be resident\n";
        manager.Shutdown();
        return false;
    }
    
    manager.RequestCluster(1, 1, 90, camera_pos);
    manager.ProcessRequests(2);
    
    const StreamingStats& stats = manager.GetStats();
    if (stats.eviction_count == 0) {
        std::cout << "  FAILED: Should evict when pool is full\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Empty pool handling test\n";
    return true;
}

bool test_residency_bitmap_manipulation() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    SetupMockResourceManager(manager);
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.ProcessRequests(1);
    
    if (!manager.IsClusterResident(1, 0)) {
        std::cout << "  FAILED: Cluster should be resident after loading\n";
        manager.Shutdown();
        return false;
    }
    
    manager.Shutdown();
    std::cout << "  PASSED: Residency bitmap manipulation test\n";
    return true;
}

bool test_resource_manager_dependency() {
    MockRHIDeviceForStreaming device;
    NaniteStreamingManager manager;
    NaniteStreamingConfig config;
    config.page_pool_size_bytes = 64 * 1024 * 1024;
    
    if (!manager.Initialize(&device, config)) {
        std::cout << "  FAILED: Initialization failed\n";
        return false;
    }
    
    primal::math::v3 camera_pos{0.0f, 0.0f, 0.0f};
    manager.RequestCluster(1, 0, 100, camera_pos);
    manager.ProcessRequests(1);
    
    if (manager.IsClusterResident(1, 0)) {
        std::cout << "  FAILED: Cluster should not load without resource manager\n";
        manager.Shutdown();
        return false;
    }
    
    std::cout << "  PASSED: Resource manager dependency test\n";
    manager.Shutdown();
    return true;
}

int main() {
    TestSuite suite("NaniteStreamingManager");
    
    suite.AddTestCase(TestCase("Initialization", []() -> TestResult {
        return test_streaming_manager_initialization() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("ConfigurablePoolSize", []() -> TestResult {
        return test_configurable_pool_size() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("LRUEvictionCorrectness", []() -> TestResult {
        return test_lru_eviction_correctness() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("ClusterResidencyBitmap", []() -> TestResult {
        return test_cluster_residency_bitmap() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("MaxRequestsPerFrameLimit", []() -> TestResult {
        return test_max_requests_per_frame_limit() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("NullDeviceInitialization", []() -> TestResult {
        return test_null_device_initialization() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("ZeroPoolSizeInitialization", []() -> TestResult {
        return test_zero_pool_size_initialization() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("ZeroPageSizeInitialization", []() -> TestResult {
        return test_zero_page_size_initialization() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("DoubleInitialization", []() -> TestResult {
        return test_double_initialization() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("DoubleShutdown", []() -> TestResult {
        return test_double_shutdown() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("UseAfterShutdown", []() -> TestResult {
        return test_use_after_shutdown() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("EvictionAtThreshold", []() -> TestResult {
        return test_eviction_at_threshold() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("LRUOrderingCorrectness", []() -> TestResult {
        return test_lru_ordering_correctness() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("MaxRequestsLimitEnforcement", []() -> TestResult {
        return test_max_requests_limit_enforcement() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("StatisticsTracking", []() -> TestResult {
        return test_statistics_tracking() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("PoolUsageTracking", []() -> TestResult {
        return test_pool_usage_tracking() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("BufferHandlesValid", []() -> TestResult {
        return test_buffer_handles_valid() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("LargeClusterIndex", []() -> TestResult {
        return test_large_cluster_index() ? TestResult::Passed : TestResult::Failed;
    }));
    suite.AddTestCase(TestCase("EmptyPoolHandling", []() -> TestResult {
        return test_empty_pool_handling() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("ResidencyBitmapManipulation", []() -> TestResult {
        return test_residency_bitmap_manipulation() ? TestResult::Passed : TestResult::Failed;
    }));
    
    suite.AddTestCase(TestCase("ResourceManagerIntegration", []() -> TestResult {
        return test_resource_manager_dependency() ? TestResult::Passed : TestResult::Failed;
    }));
    
    Engine::Test::TestStats stats = suite.RunAllTests();
    std::cout << "\n=== Test Summary ===\n";
    std::cout << "Total: " << stats.totalTests << "\n";
    std::cout << "Passed: " << stats.passedTests << "\n";
    std::cout << "Failed: " << stats.failedTests << "\n";
    std::cout << "Skipped: " << stats.skippedTests << "\n";
    return stats.failedTests > 0 ? 1 : 0;
}
