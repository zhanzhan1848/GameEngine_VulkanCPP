#pragma once

// T4.6.5 part 30.4 — Vulkan Sponza render via RenderSystem.
//
// Mirrors TestNaniteStreamingPipeline scaffolding (RenderTestRunner +
// RenderSystem + 60 FPS CFRunLoopTimer), but drives the production non-Editor
// path through StandardRenderPipeline::RenderWithCommandBuffer on a Vulkan
// device. Replaces the manual swapchain/semaphore/cmd-buffer management of
// the deleted TestVulkanStandardPipelineRender_Windowed test, which was the
// source of recurring fence/command-buffer-in-use validation errors.

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include "Engine/Graphics/RHI/Utils/IBLPrecomputer.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Graphics/Nanite/GPUMaterialRegistry.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Cluster.h"
#include "Engine/Components/Transform.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/Window.h"
#include "Engine/Platform/PlatformTypes.h"

#include <memory>
#include <vector>
#include <string>

class TestVulkanSponzaRenderGraph;

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};

class TestVulkanSponzaRenderGraph : public primal::test::RenderTestCase {
public:
    TestVulkanSponzaRenderGraph() = default;
    ~TestVulkanSponzaRenderGraph() override;

    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    bool InitializeDevice();
    bool InitializeWindowAndRenderSystem();
    bool InitializePipeline();
    bool LoadSponzaScene();
    // T4.6.5 part 37: load HDR + equirect→cube + IBL precompute + SetIBLResources.
    bool InitializeIBL();

    primal::platform::window window_;
    primal::graphics::rhi::RHIDeviceBase* device_{nullptr};
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> deviceOwnership_;
    primal::graphics::RenderSystem renderSystem_;
    std::unique_ptr<primal::graphics::StandardRenderPipeline> pipeline_;
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;

    // Sponza scene state — mirrors TestVulkanStandardPipelineRender_NonEditor.
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes_;
    std::vector<std::shared_ptr<primal::graphics::MaterialInstance>> materialInstances_;
    std::vector<primal::game_entity::entity> entities_;
    std::vector<primal::cluster::component> clusterComps_;
    std::shared_ptr<primal::graphics::Material> sharedMaterial_;
    primal::graphics::nanite::GPUMaterialRegistry* materialRegistry_{nullptr};

    // Cached handles for cleanup.
    primal::graphics::rhi::ResourceHandle fallbackDiffuse_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle fallbackNormal_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle fallbackORM_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::SamplerHandle materialSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};
    primal::graphics::rhi::SamplerHandle texSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};

    primal::math::m4x4 viewMatrix_{primal::graphics::rhi::math::MatrixIdentity()};
    primal::math::m4x4 projMatrix_{primal::graphics::rhi::math::MatrixIdentity()};

    // T4.6.5 part 37: IBL resources (Tier 5 visual fidelity). sunset.hdr →
    // envCube (TextureCube) → irradiance/prefilter cubes + brdfLUT 2D via
    // IBLPrecomputer. All forwarded to deferred_module_ via
    // pipeline_->SetIBLResources(). precomputer_ must outlive pipeline_ since
    // the generated ResourceHandles are owned by the device, but the
    // precomputer object manages pipeline/layout state used during generation.
    std::unique_ptr<primal::graphics::rhi::IBLPrecomputer> iblPrecomputer_;
    primal::graphics::rhi::ResourceHandle iblIrradiance_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblPrefilter_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblBrdfLUT_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    // Intermediate envCube + its 2DArray storage view (for equirect→cube write).
    primal::graphics::rhi::ResourceHandle envCube_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle envCubeArrayView_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle equirectTex_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // T4.6.5 part 31: WASD + right-mouse-look camera. Lazy-initialized on first
    // Run() to the same view {0,5,-10} looking +Z and slightly down that
    // LoadSponzaScene hard-coded before. Each frame: camera_.Update(1/60) +
    // view_.SetViewMatrix(camera_.GetViewMatrix()) + view_.UpdateFrustum().
    primal::graphics::rhi::RHICamera camera_;
    bool cameraInitialized_{false};

    // T4.6.5 part 30.13 (X5 fix): per-swapchain-image render-done semaphores.
    // Validation hint (VUID-vkQueueSubmit-pSignalSemaphores-00067): "Swapchain
    // image N was presented but was not re-acquired, so VkSemaphore may still
    // be in use and cannot be safely reused with image index M." Single
    // semaphore cycles too fast under FIFO; per-image index guarantees the
    // semaphore isn't reused until that image comes back via AcquireNextImage.
    static constexpr u32 kMaxSwapchainImages = 3;
    primal::graphics::rhi::SyncHandle renderDoneSemaphores_[kMaxSwapchainImages] = {
        primal::graphics::rhi::handles::INVALID_SYNC,
        primal::graphics::rhi::handles::INVALID_SYNC,
        primal::graphics::rhi::handles::INVALID_SYNC
    };

    u64 frameCount_{0};
    bool subsystemsInitialized_{false};
    bool hasShutdown_{false};  // T4.6.5 part 35.7: re-entrancy guard
};
