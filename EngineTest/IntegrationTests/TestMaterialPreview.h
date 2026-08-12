#pragma once

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/MaterialPreview/MaterialPreviewRenderer.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"

class MaterialPreviewTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::MaterialPreviewRenderer preview;

    u64 frame_count_{0};
    f32 total_time_{0.0f};
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
