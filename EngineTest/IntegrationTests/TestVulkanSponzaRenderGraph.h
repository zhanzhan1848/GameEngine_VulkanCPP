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

    u64 frameCount_{0};
    bool subsystemsInitialized_{false};
};
