#include "TestMaterialPreviewEditor.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/MaterialPreview/MaterialPreviewRenderer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "Engine/Graphics/MaterialGraph/Nodes/ConstantNode.h"
#include "Engine/Graphics/MaterialGraph/Nodes/MaterialOutputNode.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Input/Input.h"
#include "MacKeyboard.h"
#include <iostream>
#include <string>
#include <cmath>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using input_code = primal::input::input_code;  // struct alias (struct input_code { enum code : u32 {...}; })

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<MaterialPreviewEditorTestCase>())
{}

bool MaterialPreviewEditorTestCase::Initialize() {
    std::cout << "[TestMaterialPreviewEditor] Initializing (validates C API path)..." << std::endl;

    monitorKeyboardInput();

    primal::platform::window_init_info winInfo{};
    winInfo.caption = "Material Preview Editor (C API)";
    winInfo.width = 512;
    winInfo.height = 512;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) {
        std::cerr << "Failed to create window!" << std::endl;
        return false;
    }

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

    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = winInfo.width;
    sysInfo.height = winInfo.height;
    if (!renderSystem.Initialize(sysInfo)) {
        std::cerr << "Failed to initialize RenderSystem!" << std::endl;
        return false;
    }

    // ===== Validate C API path from here =====

    // 1. Create preview via C API (device handle cast to u64).
    preview_id_ = CreateMaterialPreview(reinterpret_cast<u64>(device.get()), 512, 512);
    if (preview_id_ == 0) {
        std::cerr << "CreateMaterialPreview failed" << std::endl;
        return false;
    }
    std::cout << "[TestMaterialPreviewEditor] preview_id=" << preview_id_ << std::endl;

    // 2. Build a MaterialGraph in C++, serialize to JSON, hand to C API.
    //    This is exactly the data path an external editor would take.
    using namespace primal::graphics::material_graph;
    MaterialGraph graph;
    auto c = std::make_unique<ConstantFloat4Node>();
    c->value = {0.85f, 0.35f, 0.15f, 1.0f};  // warm orange (matches TestMaterialPreview)
    u32 cid = graph.AddNode(std::move(c));
    auto out = std::make_unique<MaterialOutputNode>();
    u32 oid = graph.AddNode(std::move(out));
    graph.Connect(cid, 0, oid, 0);

    std::string json = MaterialGraphSerializer::Serialize(graph);
    std::cout << "[TestMaterialPreviewEditor] graph json = " << json << std::endl;
    if (!SetMaterialPreviewGraph(preview_id_, json.c_str())) {
        std::cerr << "SetMaterialPreviewGraph failed" << std::endl;
        return false;
    }

    // 3. Default to Teapot to verify D.2 lathe fix (was the original bug source).
    SetMaterialPreviewModel(preview_id_, static_cast<u32>(PreviewModelType::Teapot));

    std::cout << "[TestMaterialPreviewEditor] Controls:" << std::endl;
    std::cout << "  1-8 = Sphere/Box/Cylinder/Cone/Torus/Capsule/Teapot/Plane" << std::endl;
    std::cout << "  T   = toggle 2D / 3D" << std::endl;
    std::cout << "  M   = load sphere as override mesh" << std::endl;
    std::cout << "  C   = clear override" << std::endl;
    std::cout << "  Arrows = orbit camera (yaw/pitch around target)" << std::endl;
    std::cout << "  - / = = decrease / increase FOV" << std::endl;
    std::cout << "  L   = cycle light direction presets (front/back/left/right)" << std::endl;
    std::cout << "  B   = toggle background Gradient <-> SolidColor" << std::endl;
    std::cout << "  ESC = quit" << std::endl;
    return true;
}

bool MaterialPreviewEditorTestCase::JustPressed(u32 code_index) {
    primal::input::input_value val;
    primal::input::get(primal::input::input_source::keyboard,
                       static_cast<primal::input::input_code::code>(code_index), val);
    const bool down = val.current.x > 0.0f;
    const bool edge = down && !prev_key_down_[code_index];
    prev_key_down_[code_index] = down;
    return edge;
}

void MaterialPreviewEditorTestCase::Run() {
    const f32 dt = 1.0f / 60.0f;
    total_time_ += dt;

    // Model switching (1-8). Indices map to PreviewModelType enum order.
    using PMT = PreviewModelType;
    if (JustPressed(static_cast<u32>(input_code::key_1))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Sphere));
    if (JustPressed(static_cast<u32>(input_code::key_2))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Box));
    if (JustPressed(static_cast<u32>(input_code::key_3))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Cylinder));
    if (JustPressed(static_cast<u32>(input_code::key_4))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Cone));
    if (JustPressed(static_cast<u32>(input_code::key_5))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Torus));
    if (JustPressed(static_cast<u32>(input_code::key_6))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Capsule));
    if (JustPressed(static_cast<u32>(input_code::key_7))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Teapot));
    if (JustPressed(static_cast<u32>(input_code::key_8))) SetMaterialPreviewModel(preview_id_, static_cast<u32>(PMT::Plane));

    if (JustPressed(static_cast<u32>(input_code::key_t))) {
        const u32 cur = IsMaterialPreviewMode2D(preview_id_);
        SetMaterialPreviewMode2D(preview_id_, cur ? 0u : 1u);
        std::cout << "[TestMaterialPreviewEditor] Mode2D -> " << (cur ? 0 : 1) << std::endl;
    }

    if (JustPressed(static_cast<u32>(input_code::key_m))) {
        // Register a sphere asset and feed its content id to the preview.
        const primal::id::id_type gid = primal::content::create_sphere_mesh(0.9f, 32, 16);
        const u32 ok = SetMaterialPreviewMesh(preview_id_, static_cast<u64>(gid));
        std::cout << "[TestMaterialPreviewEditor] SetPreviewMesh gid=" << gid << " ok=" << ok << std::endl;
    }
    if (JustPressed(static_cast<u32>(input_code::key_c))) {
        ClearMaterialPreviewMesh(preview_id_);
        std::cout << "[TestMaterialPreviewEditor] Override cleared" << std::endl;
    }

    // --- Arrow keys: orbit camera (yaw/pitch around target) ---
    {
        const f32 orbit_step = 0.1f; // radians per press
        bool orbit_changed = false;
        if (JustPressed(static_cast<u32>(input_code::key_left)))  { orbit_yaw_   += orbit_step; orbit_changed = true; }
        if (JustPressed(static_cast<u32>(input_code::key_right))) { orbit_yaw_   -= orbit_step; orbit_changed = true; }
        if (JustPressed(static_cast<u32>(input_code::key_up)))    { orbit_pitch_ += orbit_step; orbit_changed = true; }
        if (JustPressed(static_cast<u32>(input_code::key_down)))  { orbit_pitch_ -= orbit_step; orbit_changed = true; }

        if (orbit_changed) {
            // Clamp pitch to avoid flipping.
            const f32 pitch_limit = 1.5f; // ~85 degrees
            if (orbit_pitch_ >  pitch_limit) orbit_pitch_ =  pitch_limit;
            if (orbit_pitch_ < -pitch_limit) orbit_pitch_ = -pitch_limit;

            // Convert spherical to Cartesian around target.
            const f32 cp = std::cos(orbit_pitch_);
            f32 px = orbit_radius_ * cp * std::sin(orbit_yaw_);
            f32 py = orbit_radius_ * std::sin(orbit_pitch_);
            f32 pz = orbit_radius_ * cp * std::cos(orbit_yaw_);
            f32 pos[3] = {px, py, pz};
            SetMaterialPreviewCameraPosition(preview_id_, pos);

            // Stop auto-rotation when user orbits manually.
            SetMaterialPreviewRotationSpeed(preview_id_, 0.0f);

            std::cout << "[TestMaterialPreviewEditor] Orbit yaw=" << orbit_yaw_
                      << " pitch=" << orbit_pitch_
                      << " pos=(" << px << "," << py << "," << pz << ")" << std::endl;
        }
    }

    // --- - / = : FOV decrease / increase ---
    if (JustPressed(static_cast<u32>(input_code::key_minus))) {
        cam_fov_ -= 0.1f;
        if (cam_fov_ < 0.1f) cam_fov_ = 0.1f;
        SetMaterialPreviewCameraFov(preview_id_, cam_fov_);
        std::cout << "[TestMaterialPreviewEditor] FOV=" << cam_fov_ << " ("
                  << cam_fov_ * 57.2958f << " deg)" << std::endl;
    }
    if (JustPressed(static_cast<u32>(input_code::key_plus))) {
        cam_fov_ += 0.1f;
        if (cam_fov_ > 2.5f) cam_fov_ = 2.5f;
        SetMaterialPreviewCameraFov(preview_id_, cam_fov_);
        std::cout << "[TestMaterialPreviewEditor] FOV=" << cam_fov_ << " ("
                  << cam_fov_ * 57.2958f << " deg)" << std::endl;
    }

    // --- L: cycle light direction presets ---
    if (JustPressed(static_cast<u32>(input_code::key_l))) {
        light_preset_ = (light_preset_ + 1) % 4;
        // Presets: 0=front, 1=back, 2=left, 3=right
        f32 dir[3];
        const char* names[4] = {"front", "back", "left", "right"};
        switch (light_preset_) {
            case 0: dir[0] =  0.0f; dir[1] = -0.5f; dir[2] =  0.8f; break; // front-top
            case 1: dir[0] =  0.0f; dir[1] = -0.5f; dir[2] = -0.8f; break; // back-top
            case 2: dir[0] = -0.8f; dir[1] = -0.5f; dir[2] =  0.0f; break; // left-top
            case 3: dir[0] =  0.8f; dir[1] = -0.5f; dir[2] =  0.0f; break; // right-top
        }
        SetMaterialPreviewLightDirection(preview_id_, dir);
        std::cout << "[TestMaterialPreviewEditor] Light preset: " << names[light_preset_]
                  << " dir=(" << dir[0] << "," << dir[1] << "," << dir[2] << ")" << std::endl;
    }

    // --- B: toggle background mode ---
    if (JustPressed(static_cast<u32>(input_code::key_b))) {
        // Track bg mode locally (no getter in C API for background mode).
        static u32 bg_mode = 0; // 0=Gradient, 1=SolidColor
        bg_mode = bg_mode ? 0 : 1;
        SetMaterialPreviewBackgroundMode(preview_id_, bg_mode);
        std::cout << "[TestMaterialPreviewEditor] Background mode: "
                  << (bg_mode ? "SolidColor" : "Gradient") << std::endl;
    }

    // Begin frame → back buffer
    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem.BeginFrame(backBuffer, signalFence)) return;

    auto* cmd = renderSystem.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
    u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

    cmd->Reset();
    cmd->Begin();

    // Render preview via C API (cmd handle cast to u64).
    const u64 previewColor =
        RenderMaterialPreview(preview_id_, reinterpret_cast<u64>(cmd), bufferIndex, dt);

    // Blit to swap chain via fragment shader. Metal swap chain drawables are
    // framebufferOnly and reject MTLBlitCommandEncoder writes — the renderer's
    // BlitToTexture renders a fullscreen triangle sampling previewColor into backBuffer.
    if (previewColor != 0 && backBuffer != handles::INVALID_RESOURCE) {
        BlitMaterialPreviewToBackbuffer(preview_id_, reinterpret_cast<u64>(cmd),
                                         backBuffer, 512, 512);
    }

    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device->Submit(submitInfo);

    renderSystem.EndFrame();
    frame_count_++;
}

void MaterialPreviewEditorTestCase::Shutdown() {
    std::cout << "[TestMaterialPreviewEditor] Shutting down..." << std::endl;

    // Tear down all previews via the C API (validates DestroyAllMaterialPreviews path).
    DestroyAllMaterialPreviews();
    preview_id_ = 0;

    renderSystem.Shutdown();

    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }

    // Clear procedural meshes from rhi_mesh_assets so ~free_list does not assert.
    primal::content::shutdown();

    device.reset();
    std::cout << "[TestMaterialPreviewEditor] Shutdown complete." << std::endl;
}
