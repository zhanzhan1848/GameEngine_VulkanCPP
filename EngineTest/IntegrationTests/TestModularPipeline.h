#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/Nanite/GPUMaterialRegistry.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Cluster.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/AsyncResourceLoader.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Platform/Platform.h"
#include "ShaderCompilation.h"
#include <memory>
#include <string>
#include <unordered_map>

class TestModularPipeline : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Resize(uint32_t width, uint32_t height) override;
    void Run() override;
    void Shutdown() override;

private:
    // Core RHI
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device_;
    primal::platform::window window_;
    primal::graphics::RenderSystem renderSystem_;

    // Modular pipeline
    std::unique_ptr<primal::graphics::StandardRenderPipeline> pipeline_;

    // Scene
    primal::graphics::RenderScene scene_;
    primal::graphics::RenderView view_;

    // Camera
    primal::graphics::rhi::RHICamera camera_;
    primal::math::m4x4 cameraView_;
    primal::math::m4x4 cameraProj_;

    // Material system
    std::unique_ptr<primal::graphics::nanite::GPUMaterialRegistry> gpuMaterialRegistry_;
    primal::utl::vector<primal::graphics::SceneDataMeshInfo> sceneMeshes_;

    // Shader handles (stashed between CompileShaders and InitializePipeline)
    primal::graphics::StandardRenderPipeline::ShaderHandles shaderHandles_;

    // Shader cache
    struct StringHash {
        size_t operator()(const std::string& key) const {
            uint32_t hash;
            primal::utl::MurmurHash3_x86_32(key.c_str(), (int)key.length(), 0, &hash);
            return hash;
        }
    };
    std::unordered_map<std::string, primal::graphics::rhi::ShaderHandle, StringHash> shaderMap_;

    // State
    uint32_t frameCount_ = 0;
    uint32_t renderWidth_ = 1280;
    uint32_t renderHeight_ = 720;
    bool isShutdown_ = false;

    // Helpers
    bool InitializeDevice();
    bool CompileShaders();
    bool InitializePipeline();
    void SetupCamera();
    bool LoadSponzaScene();
    bool LoadMaterialTextures();
};

#ifdef TEST_MODULAR_PIPELINE
class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
#endif
