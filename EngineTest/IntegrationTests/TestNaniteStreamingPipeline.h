#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include "Engine/Graphics/RenderGraph/RenderGraph.h"
#include "Engine/Graphics/Nanite/GPUCullingPipeline.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Nanite/NaniteStreamingManager.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/DepthHistoryManager.h"
#include "Engine/Graphics/Nanite/VisibilityBufferSystem.h"
#include "Engine/Graphics/Scene/RenderSceneSnapshot.h"
#include "Engine/Components/Cluster.h"
#include "Engine/Graphics/Scene/SceneExtractionSystem.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/Window.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "ShaderCompilation.h"
#include <memory>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <string>

class TestNaniteStreamingPipeline;

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};

class TestNaniteStreamingPipeline : public primal::test::RenderTestCase {
public:
    TestNaniteStreamingPipeline() = default;
    ~TestNaniteStreamingPipeline() override;

    // RenderTestCase interface
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;
    void Resize(u32 width, u32 height) override;

    // Test-specific methods
    void TestStreamingInitialization();
    void TestGPURequestGeneration();
    void TestLRUEviction();
    void TestResidencyBuffer();
    void TestEndToEndStreaming();
    void TestPerformance();

private:
    static TestNaniteStreamingPipeline* instance;
    // Core rendering components
    primal::graphics::rhi::RHIDeviceBase* device_{ nullptr };
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_ownership_;
    primal::platform::window window_;
    primal::graphics::rhi::ResourceHandle sceneDepthTexture_{ primal::graphics::rhi::handles::INVALID_RESOURCE };  // Actual rendered depth for HZB
    primal::graphics::RenderSystem renderSystem_;
    std::unique_ptr<primal::graphics::rendergraph::RenderGraph> renderGraph_;

    // Nanite streaming components
    primal::graphics::nanite::GPUCullingPipeline* cullingPipeline_{ nullptr };
    primal::graphics::nanite::GPUDrivenDrawPipeline* gpuDrawPipeline_{ nullptr };
    primal::graphics::nanite::NaniteStreamingManager* streamingManager_{ nullptr };
    primal::graphics::nanite::NaniteResourceManager* resourceManager_{ nullptr };
    std::unique_ptr<primal::graphics::SceneExtractionSystem> extractionSystem_;

    // HZB and Visibility Buffer components
    std::unique_ptr<primal::graphics::nanite::HZBSystem> hzbSystem_;
    std::unique_ptr<primal::graphics::nanite::DepthHistoryManager> depthHistoryManager_;
    std::unique_ptr<primal::graphics::nanite::VisibilityBufferSystem> visibilityBufferSystem_;

    // Blit pipeline for final presentation
    primal::graphics::rhi::PipelineHandle blit_pipeline_{ primal::graphics::rhi::handles::INVALID_PIPELINE };
    primal::graphics::rhi::PipelineLayoutHandle blit_layout_{ primal::graphics::rhi::handles::INVALID_PIPELINE_LAYOUT };
    primal::graphics::rhi::DescriptorSetLayoutHandle blit_set_layout_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    primal::graphics::rhi::DescriptorSetHandle blit_descriptor_set_{ primal::graphics::rhi::handles::INVALID_DESCRIPTOR_SET };

    // Shader management (following TestParticleSponza pattern)
    struct StringHash {
        size_t operator()(const std::string& key) const {
            uint32_t hash;
            primal::utl::MurmurHash3_x86_32(key.c_str(), (int)key.length(), 0, &hash);
            return hash;
        }
    };
    std::unordered_map<std::string, primal::graphics::rhi::ShaderHandle, StringHash> shaderVariantMap;

    // Scene data
    primal::graphics::RenderSceneSnapshot sceneSnapshot_;
    std::vector<primal::graphics::nanite::NaniteRuntimeResource*> testResources_;

    // Sponza scene data
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes_;
    
    // Test state
    u32 frameCount_{ 0 };
    u32 renderWidth_{ 0 };
    u32 renderHeight_{ 0 };
    bool isShutdown_{ false };
    primal::graphics::rhi::RHICamera camera_{};
    math::m4x4 cameraView_{ primal::graphics::rhi::math::MatrixIdentity() };
    math::m4x4 cameraProj_{ primal::graphics::rhi::math::MatrixIdentity() };

    // Culling debug data storage for final frame only
    primal::utl::vector<primal::graphics::nanite::CullingDebugData> finalFrameCullingDebugData_;

    // CRITICAL FIX: Triple-buffered camera data to match MAX_FRAMES_IN_FLIGHT = 3
    // This prevents array out-of-bounds and frame synchronization issues
    struct CameraBuffer {
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        u32 frame_index;
        u32 padding[3];
    } cameraBuffers_[3];

    // Test configuration
    struct TestConfig {
        u32 max_clusters{ 10000 };
        u32 max_instances{ 1000 };
        u32 streaming_pool_size_mb{ 64 };
        u32 max_requests_per_frame{ 100 };
        float eviction_threshold{ 0.8f };
        bool enable_streaming{ true };
        bool enable_lod_selection{ true };
        bool enable_occlusion_culling{ true };
    } testConfig_;

    // Test results tracking
    struct TestResults {
        std::atomic<u32> clusters_streamed{ 0 };
        std::atomic<u32> clusters_evicted{ 0 };
        std::atomic<u32> requests_generated{ 0 };
        std::atomic<u32> requests_processed{ 0 };
        std::atomic<float> avg_streaming_latency_ms{ 0.0f };
        std::atomic<float> avg_processing_time_ms{ 0.0f };
    } testResults_;

    // Helper methods
    bool InitializeDevice();
    bool InitializeWindowAndRenderSystem();
    bool InitializeStreamingComponents();
    bool CreateTestScene();
    bool LoadSponzaScene();
    bool SetupBasicRenderingPipeline();
    void UpdateTestScene();
    void BuildRenderGraph(primal::graphics::rendergraph::RenderGraph& graph, primal::graphics::rhi::ResourceHandle backBuffer, u32 currentBufferIndex);
    void ProcessStreamingFeedback();
    void RecordTestMetrics();
    void ValidateResults();
    void PrintAllInstanceBounds();
    void PrintFinalFrameCullingDebugData();

    // Input handling
    struct KeyStateTracker {
        bool f1_prev{ false };  // Toggle streaming visualization
        bool f2_prev{ false };  // Stress test mode
        bool f3_prev{ false };  // Performance benchmark
        bool space_prev{ false }; // Pause/resume streaming
    } keyState_;
};