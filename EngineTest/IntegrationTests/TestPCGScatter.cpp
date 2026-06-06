#include "TestPCGScatter.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Input/Input.h"
#include "MacKeyboard.h"
#include <iostream>
#include <cmath>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;

// ============================================================================
// Engine_Test
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<PCGScatterTestCase>())
{}

// ============================================================================
// PCGScatterTestCase::Initialize
// ============================================================================

bool PCGScatterTestCase::Initialize() {
    std::cout << "[TestPCGScatter] Initializing..." << std::endl;

    monitorKeyboardInput();

    // 1. Window + Device + RenderSystem
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestPCGScatter - PCG Phase 1";
    winInfo.width = 1280;
    winInfo.height = 720;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) return false;

    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) { delete metalDevice; return false; }
    device.reset(metalDevice);

    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = winInfo.width;
    sysInfo.height = winInfo.height;
    if (!renderSystem.Initialize(sysInfo)) return false;

    // 2. Pipeline + Editor Mode
    pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device.get())) return false;
    pipeline->SetOutputResource(handles::INVALID_RESOURCE, {});
    pipeline->SetViewportSize(winInfo.width, winInfo.height);

    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(lumenConfig);
    pipeline->SetEditorMode(true);

    // 3. Load scene
    const char* scenePaths[] = {
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process_rebuild.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza.model",
    };
    bool loaded = false;
    for (const char* path : scenePaths) {
        if (pipeline->LoadEditorScene(path)) {
            std::cout << "[TestPCGScatter] Loaded scene: " << path << std::endl;
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        std::cerr << "[TestPCGScatter] No scene loaded, PCG instances will have no mesh" << std::endl;
    }

    // 4. Build and execute PCG graph
    ExecutePCGGraph();

    // 5. Camera
    scene = new RenderScene();
    RenderProxy proxy;
    proxy.meshId = primal::id::invalid_id;
    proxy.materialId = primal::id::invalid_id;
    simd::float4 col0 = {1, 0, 0, 0};
    simd::float4 col1 = {0, 1, 0, 0};
    simd::float4 col2 = {0, 0, 1, 0};
    simd::float4 col3 = {0, 0, 0, 1};
    proxy.transform = simd_matrix(col0, col1, col2, col3);
    scene->AddProxy(proxy);

    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = winInfo.width;
    viewport.size.y = winInfo.height;
    view->SetViewport(viewport);
    UpdateCamera();

    std::cout << "[TestPCGScatter] Ready. Controls: WASD=Move, QE=Up/Down, Arrows=Rotate, ESC=Quit" << std::endl;
    return true;
}

// ============================================================================
// PCGScatterTestCase::Run
// ============================================================================

void PCGScatterTestCase::Run() {
    timer_.begin();
    HandleInput(1.0f / 60.0f);
    UpdateCamera();

    if (pipeline && scene && view) {
        view->UpdateFrustum();
        view->Cull(*scene);

        ResourceHandle backBuffer;
        SyncHandle signalFence;
        if (!renderSystem.BeginFrame(backBuffer, signalFence)) {
            timer_.end();
            return;
        }

        auto* cmd = renderSystem.GetCurrentCommandBuffer();
        auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
        u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

        cmd->Reset();
        cmd->Begin();

        pipeline->RenderWithCommandBuffer(
            *scene, *view, backBuffer, renderSystem.GetBackBufferDesc(),
            cmd, bufferIndex, cmdHandle, signalFence);

        cmd->End();

        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdHandle;
        submitInfo.signalFence = signalFence;
        device->Submit(submitInfo);

        renderSystem.EndFrame();

        auto* fwd = pipeline->GetForwardRenderer();
        if (fwd) {
            fwd->UpdateAsyncTextures(static_cast<u32>(frame_count_));
        }
        frame_count_++;
    }

    timer_.end();
}

// ============================================================================
// PCGScatterTestCase::Shutdown
// ============================================================================

void PCGScatterTestCase::Shutdown() {
    std::cout << "[TestPCGScatter] Shutting down..." << std::endl;

    if (pipeline) { delete pipeline; pipeline = nullptr; }
    renderSystem.Shutdown();
    if (scene) { delete scene; scene = nullptr; }
    if (view) { delete view; view = nullptr; }
    if (window.is_valid()) { primal::platform::remove_window(window.get_id()); }
    device.reset();
    primal::content::shutdown();
}

// ============================================================================
// PCGScatterTestCase::ExecutePCGGraph
// ============================================================================

void PCGScatterTestCase::ExecutePCGGraph() {
    using namespace primal::graphics::pcg;

    PCGGraph graph;

    // Node 0: ReferenceField (ground plane SDF)
    auto refNode = std::make_unique<ReferenceFieldNode>();
    graph.AddNode(std::move(refNode)); // node 0

    // Node 1: NoiseField (density variation)
    auto noiseNode = std::make_unique<NoiseFieldNode>();
    noiseNode->frequency = 0.05f;
    noiseNode->octaves = 4;
    noiseNode->seed = 123;
    graph.AddNode(std::move(noiseNode)); // node 1

    // Node 2: FieldScatter (density from noise, scatter in volume)
    auto scatterNode = std::make_unique<FieldScatterNode>();
    scatterNode->target_count = 2000;
    scatterNode->bounds_min = {-40, 1, -40};
    scatterNode->bounds_max = {40, 3, 40};
    scatterNode->seed = 42;
    scatterNode->points_per_unit_area = 1.0f;
    graph.AddNode(std::move(scatterNode)); // node 2

    // Node 3: SDFConstraint (keep points above ground: SDF > 0)
    auto sdfNode = std::make_unique<SDFConstraintNode>();
    sdfNode->min_dist = 0.5f;
    sdfNode->max_dist = 100.0f;
    graph.AddNode(std::move(sdfNode)); // node 3

    // Node 4: DensityFilter (keep points with density in [0.3, 1.0])
    auto densityFilterNode = std::make_unique<DensityFilterNode>();
    densityFilterNode->min_density = 0.3f;
    densityFilterNode->max_density = 1.0f;
    graph.AddNode(std::move(densityFilterNode)); // node 4

    // Node 5: Transform (random scale + position jitter)
    auto transformNode = std::make_unique<TransformNode>();
    transformNode->scale_min = {0.3f, 0.3f, 0.3f};
    transformNode->scale_max = {1.0f, 2.0f, 1.0f};
    transformNode->position_jitter = 0.5f;
    transformNode->seed = 99;
    graph.AddNode(std::move(transformNode)); // node 5

    // Node 6: MeshAssign (weighted: 70% mesh 0, 30% mesh 1)
    auto meshAssignNode = std::make_unique<MeshAssignNode>();
    meshAssignNode->weights = {0.7f, 0.3f};
    graph.AddNode(std::move(meshAssignNode)); // node 6

    // Connect: Noise → Scatter (density input)
    graph.Connect(1, 0, 2, 0);
    // Connect: Scatter → SDFConstraint (point set input)
    graph.Connect(2, 0, 3, 0);
    // Connect: ReferenceField → SDFConstraint (SDF input)
    graph.Connect(0, 0, 3, 1);
    // Connect: SDFConstraint → DensityFilter
    graph.Connect(3, 0, 4, 0);
    // Connect: DensityFilter → Transform
    graph.Connect(4, 0, 5, 0);
    // Connect: Transform → MeshAssign
    graph.Connect(5, 0, 6, 0);

    // Execute
    graph.Execute();

    // Verify intermediate results
    auto* scatterOut = graph.GetOutputPoints(2);
    auto* sdfOut = graph.GetOutputPoints(3);
    auto* filterOut = graph.GetOutputPoints(4);
    std::cout << "[TestPCGScatter] Pipeline: scatter=" << (scatterOut ? scatterOut->count : 0)
              << " → sdf=" << (sdfOut ? sdfOut->count : 0)
              << " → density_filter=" << (filterOut ? filterOut->count : 0) << std::endl;

    auto* points = graph.GetOutputPoints(6);
    if (!points || points->count == 0) {
        std::cerr << "[TestPCGScatter] PCG graph produced 0 points!" << std::endl;
        return;
    }

    // Verify multi-mesh assignment: count mesh indices
    u32 mesh0 = 0, mesh1 = 0;
    for (u32 i = 0; i < points->count; ++i) {
        u32 idx = static_cast<u32>(points->GetAttr(i, PCGAttr::MeshIndex));
        if (idx == 0) mesh0++; else mesh1++;
    }
    std::cout << "[TestPCGScatter] Mesh assignment: mesh0=" << mesh0 << " mesh1=" << mesh1
              << " (expected ~70/30)" << std::endl;

    std::cout << "[TestPCGScatter] PCG graph produced " << points->count << " points" << std::endl;

    // Build instances
    PCGInstanceBuilder builder;
    std::vector<pcg::PCGInstanceData> instances;
    builder.Build(*points, instances);

    // Pass to renderer
    auto* fwd = pipeline->GetForwardRenderer();
    if (fwd) {
        fwd->SetPCGInstances(instances);
        std::cout << "[TestPCGScatter] Uploaded " << instances.size() << " PCG instances to renderer" << std::endl;
    }

    // Print first 5 instances for verification
    for (u32 i = 0; i < std::min(points->count, 5u); ++i) {
        auto& p = points->positions[i];
        std::cout << "  [" << i << "] pos=(" << p.x << ", " << p.y << ", " << p.z << ")"
                  << " scale=(" << points->GetAttr(i, PCGAttr::ScaleX)
                  << ", " << points->GetAttr(i, PCGAttr::ScaleY)
                  << ", " << points->GetAttr(i, PCGAttr::ScaleZ) << ")"
                  << " mesh=" << static_cast<u32>(points->GetAttr(i, PCGAttr::MeshIndex))
                  << std::endl;
    }
}

// ============================================================================
// PCGScatterTestCase::HandleInput
// ============================================================================

void PCGScatterTestCase::HandleInput(float dt) {
    using namespace primal::input;
    input_value val;

    float cy = std::cos(camera_yaw_), sy = std::sin(camera_yaw_);
    v3 cam_forward{sy, 0, -cy};
    v3 cam_right{cy, 0, sy};

    v3 move_dir{0, 0, 0};
    get(input_source::keyboard, input_code::key_w, val);
    if (val.current.x > 0.0f) move_dir = move_dir + cam_forward;
    get(input_source::keyboard, input_code::key_s, val);
    if (val.current.x > 0.0f) move_dir = move_dir - cam_forward;
    get(input_source::keyboard, input_code::key_a, val);
    if (val.current.x > 0.0f) move_dir = move_dir - cam_right;
    get(input_source::keyboard, input_code::key_d, val);
    if (val.current.x > 0.0f) move_dir = move_dir + cam_right;
    get(input_source::keyboard, input_code::key_q, val);
    if (val.current.x > 0.0f) move_dir.y -= 1;
    get(input_source::keyboard, input_code::key_e, val);
    if (val.current.x > 0.0f) move_dir.y += 1;

    float len = std::sqrt(move_dir.x * move_dir.x + move_dir.y * move_dir.y + move_dir.z * move_dir.z);
    if (len > 0.001f) {
        move_dir /= len;
        camera_pos_ = camera_pos_ + move_dir * move_speed_ * dt;
    }

    get(input_source::keyboard, input_code::key_left, val);
    if (val.current.x > 0.0f) camera_yaw_ += rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_right, val);
    if (val.current.x > 0.0f) camera_yaw_ -= rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_up, val);
    if (val.current.x > 0.0f) camera_pitch_ += rotate_speed_ * dt;
    get(input_source::keyboard, input_code::key_down, val);
    if (val.current.x > 0.0f) camera_pitch_ -= rotate_speed_ * dt;
    camera_pitch_ = std::max(-1.5f, std::min(1.5f, camera_pitch_));

    get(input_source::keyboard, input_code::key_escape, val);
    if (val.current.x > 0.0f) {
#ifdef __APPLE__
        NS::Application::sharedApplication()->terminate(nullptr);
#endif
    }
}

// ============================================================================
// PCGScatterTestCase::UpdateCamera
// ============================================================================

void PCGScatterTestCase::UpdateCamera() {
    float cy = std::cos(camera_yaw_), sy = std::sin(camera_yaw_);
    float cp = std::cos(camera_pitch_), sp = std::sin(camera_pitch_);
    v3 forward{sy * cp, sp, -cy * cp};
    v3 up{0, 1, 0};
    v3 target = camera_pos_ + simd_normalize(forward);
    m4x4 viewMat = metal::CreateLookAtMatrix(camera_pos_, target, up);
    constexpr float fov = 60.0f * (pi / 180.0f);
    m4x4 projMat = metal::CreatePerspectiveMatrix(fov, 1280.0f / 720.0f, 0.1f, 1000.0f);
    if (view) {
        view->SetViewMatrix(viewMat);
        view->SetProjectionMatrix(projMat);
    }
}
