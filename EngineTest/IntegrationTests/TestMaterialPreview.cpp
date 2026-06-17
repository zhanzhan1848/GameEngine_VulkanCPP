#include "TestMaterialPreview.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "Engine/Graphics/MaterialGraph/Nodes/ConstantNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/MaterialOutputNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/MathNodes.h"
#include "Engine/Graphics/MaterialGraph/Nodes/UtilityNodes.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Input/Input.h"
#include "MacKeyboard.h"
#include <iostream>
#include <cmath>

using namespace primal::graphics;
using namespace primal::graphics::rhi;

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<MaterialPreviewTestCase>())
{}

bool MaterialPreviewTestCase::Initialize() {
    std::cout << "[TestMaterialPreview] Initializing..." << std::endl;

    // 1. Keyboard input (for ESC to quit, model switching)
    monitorKeyboardInput();

    // 2. Create window sized to the preview (512x512) plus some chrome
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "Material Preview";
    winInfo.width = 512;
    winInfo.height = 512;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) {
        std::cerr << "Failed to create window!" << std::endl;
        return false;
    }

    // 3. Create Metal device
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        std::cerr << "Failed to create Metal device" << std::endl;
        delete metalDevice;
        return false;
    }
    device.reset(metalDevice);

    // 4. Initialize RenderSystem (creates swap chain bound to the window)
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = winInfo.width;
    sysInfo.height = winInfo.height;
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem!" << std::endl;
        return false;
    }

    // 5. Initialize MaterialPreviewRenderer
    if (!preview.Initialize(device.get(), winInfo.width, winInfo.height)) {
        std::cerr << "Failed to initialize MaterialPreviewRenderer!" << std::endl;
        return false;
    }

    // 6. Set a default material graph (ConstantFloat4 → MaterialOutput)
    {
        using namespace primal::graphics::material_graph;
        MaterialGraph graph;
        auto c = std::make_unique<ConstantFloat4Node>();
        c->value = {0.85f, 0.35f, 0.15f, 1.0f};  // warm orange
        u32 cid = graph.AddNode(std::move(c));
        auto out = std::make_unique<MaterialOutputNode>();
        u32 oid = graph.AddNode(std::move(out));
        graph.Connect(cid, 0, oid, 0);

        if (!preview.SetMaterialGraph(graph)) {
            std::cerr << "Failed to set default material graph!" << std::endl;
            return false;
        }
    }

    preview.SetPreviewModel(primal::graphics::PreviewModelType::Teapot);

    std::cout << "[TestMaterialPreview] Initialization complete." << std::endl;
    std::cout << "[TestMaterialPreview] Controls: ESC=Quit" << std::endl;
    return true;
}

void MaterialPreviewTestCase::Run() {
    const f32 dt = 1.0f / 60.0f;
    total_time_ += dt;

    // ESC to quit
    primal::input::input_value val;
    primal::input::get(primal::input::input_source::keyboard,
                       primal::input::input_code::key_escape, val);
    if (val.current.x > 0.0f) {
        // Delegate quit to platform window close (handled by RenderTestRunner)
    }

    // Begin frame → get back buffer
    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem.BeginFrame(backBuffer, signalFence)) {
        return;
    }

    auto* cmd = renderSystem.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
    u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

    cmd->Reset();
    cmd->Begin();

    // Render the preview to its offscreen target
    ResourceHandle previewColor = preview.Render(cmd, bufferIndex, dt);

    // Blit preview color → back buffer via fragment shader. Metal swap chain drawables
    // are framebufferOnly and reject MTLBlitCommandEncoder writes, so we render a
    // fullscreen triangle sampling previewColor into backBuffer.
    if (previewColor != handles::INVALID_RESOURCE && backBuffer != handles::INVALID_RESOURCE) {
        preview.BlitToTexture(cmd, backBuffer, 512, 512);
    }

    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device->Submit(submitInfo);

    renderSystem.EndFrame();
    frame_count_++;
}

void MaterialPreviewTestCase::Shutdown() {
    std::cout << "[TestMaterialPreview] Shutting down..." << std::endl;

    preview.Shutdown();
    renderSystem.Shutdown();

    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }

    // Clear procedural meshes from global rhi_mesh_assets before static destructors
    // fire — otherwise ~free_list asserts _size != 0. Must run before device.reset()
    // since GPU resources already destroyed above.
    primal::content::shutdown();

    device.reset();
    std::cout << "[TestMaterialPreview] Shutdown complete." << std::endl;
}
