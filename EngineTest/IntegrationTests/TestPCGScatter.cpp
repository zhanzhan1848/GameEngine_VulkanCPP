#include "TestPCGScatter.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/PCG/GPU/PCGScatterCompute.h"
#include "Engine/Graphics/PCG/PCGEntityFactory.h"
#include "Engine/EngineAPI/GameEntity_impl.h"
#include "Engine/Components/Geometry.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Input/Input.h"
#include "MacKeyboard.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <unordered_map>

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

    // 0. Initialize JobSystem for parallel loading
    primal::jobsystem::JobSystem::Initialize();

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
    rhi::g_deviceManager.RegisterDevice(device.get());

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

    // 3. Load scene via Content system (Phase 4 full path)
    // Read binary scene → ImportResources → load .asset textures → RegisterMeshEntity
    const char* scenePaths[] = {
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process_rebuild.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_process.model",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza.model",
    };

    // Find and load the binary scene file
    const char* scenePath = nullptr;
    for (const char* path : scenePaths) {
        std::ifstream test(path, std::ios::binary);
        if (test.good()) { scenePath = path; break; }
    }

    if (scenePath) {
        std::ifstream file(scenePath, std::ios::binary | std::ios::ate);
        std::streamsize fsize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buf(fsize);
        file.read(buf.data(), fsize);

        // Import mesh resources (registers RHIMeshAssets via content system)
        auto imported = SceneDataAdapter::ImportResources(buf.data(), (u32)fsize);
        std::cout << "[ContentImport] ImportResources returned " << imported.meshes.size()
                  << " mesh resources" << std::endl;

        // Base paths for texture resolution
        const std::string texBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
        const std::string encodeDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/fbx_textures_encode/";

        // Phase 1: Collect unique texture paths across all meshes
        std::vector<std::string> uniquePaths;
        std::unordered_map<std::string, u32> pathToIndex;
        auto addPath = [&](const std::string& relPath) {
            if (relPath.empty()) return;
            if (pathToIndex.find(relPath) != pathToIndex.end()) return;
            pathToIndex[relPath] = (u32)uniquePaths.size();
            uniquePaths.push_back(relPath);
        };
        for (auto& entry : imported.meshes) {
            addPath(entry.diffuse_path);
            addPath(entry.normal_path);
            addPath(entry.orm_path);
        }

        // Phase 2: Resolve asset paths
        std::vector<std::string> assetPaths(uniquePaths.size());
        for (u32 i = 0; i < uniquePaths.size(); ++i) {
            std::string basename = uniquePaths[i];
            size_t slash = basename.find_last_of("/\\");
            if (slash != std::string::npos) basename = basename.substr(slash + 1);
            size_t dot = basename.rfind('.');
            if (dot != std::string::npos) basename = basename.substr(0, dot);
            assetPaths[i] = encodeDir + basename + ".asset";
        }

        // Phase 3: Load textures in parallel via JobSystem
        std::vector<primal::id::id_type> texIds(uniquePaths.size(), primal::id::invalid_id);
        std::cout << "[ContentImport] Loading " << uniquePaths.size()
                  << " unique textures in parallel..." << std::endl;

        if (primal::jobsystem::JobSystem::IsRunning() && uniquePaths.size() > 1) {
            auto handle = primal::jobsystem::JobSystem::ParallelFor(
                (u32)uniquePaths.size(),
                [&](u32 i) {
                    // Read .asset file
                    std::ifstream tf(assetPaths[i], std::ios::binary | std::ios::ate);
                    if (!tf.good()) {
                        tf.open(texBaseDir + uniquePaths[i], std::ios::binary | std::ios::ate);
                        if (!tf.good()) return;
                    }
                    std::streamsize tsize = tf.tellg();
                    tf.seekg(0, std::ios::beg);
                    std::vector<u8> tbuf(tsize);
                    tf.read(reinterpret_cast<char*>(tbuf.data()), tsize);
                    texIds[i] = primal::content::create_resource(
                        tbuf.data(), primal::content::asset_type::texture);
                },
                primal::jobsystem::JobPriority::Normal);
            handle.Wait();
        } else {
            for (u32 i = 0; i < uniquePaths.size(); ++i) {
                std::ifstream tf(assetPaths[i], std::ios::binary | std::ios::ate);
                if (!tf.good()) {
                    tf.open(texBaseDir + uniquePaths[i], std::ios::binary | std::ios::ate);
                    if (!tf.good()) continue;
                }
                std::streamsize tsize = tf.tellg();
                tf.seekg(0, std::ios::beg);
                std::vector<u8> tbuf(tsize);
                tf.read(reinterpret_cast<char*>(tbuf.data()), tsize);
                texIds[i] = primal::content::create_resource(
                    tbuf.data(), primal::content::asset_type::texture);
            }
        }

        // Build cache from loaded IDs
        std::unordered_map<std::string, primal::id::id_type> texCache;
        for (u32 i = 0; i < uniquePaths.size(); ++i) {
            if (texIds[i] != primal::id::invalid_id) {
                texCache[uniquePaths[i]] = texIds[i];
            }
        }

        // Phase 4: Register entities with cached texture IDs
        auto getTexId = [&](const std::string& relPath) -> primal::id::id_type {
            if (relPath.empty()) return primal::id::invalid_id;
            auto it = texCache.find(relPath);
            return it != texCache.end() ? it->second : primal::id::invalid_id;
        };

        u32 registeredCount = 0;
        u32 texFailCount = 0;
        primal::id::id_type firstAlbedoId = primal::id::invalid_id;
        for (size_t mi = 0; mi < imported.meshes.size(); ++mi) {
            auto& entry = imported.meshes[mi];
            primal::id::id_type albedoId = getTexId(entry.diffuse_path);
            primal::id::id_type normalId = getTexId(entry.normal_path);
            primal::id::id_type ormId = getTexId(entry.orm_path);

            if (mi == 0) firstAlbedoId = albedoId;
            if (albedoId == primal::id::invalid_id && !entry.diffuse_path.empty()) texFailCount++;
            if (normalId == primal::id::invalid_id && !entry.normal_path.empty()) texFailCount++;

            primal::id::id_type texIdsArr[3] = { albedoId, normalId, ormId };

            primal::id::id_type eid = pipeline->RegisterMeshEntity(
                entry.mesh_content_id, texIdsArr, 3);
            if (eid != primal::id::invalid_id) {
                registeredCount++;
            }
        }
        std::cout << "[ContentImport] Registered " << registeredCount << "/"
                  << imported.meshes.size() << " mesh entities via content system" << std::endl;
        std::cout << "[ContentImport] Texture failures: " << texFailCount
                  << ", unique textures loaded: " << texCache.size() << std::endl;

        // Diagnose first mesh: check texture handles
        if (!imported.meshes.empty()) {
            auto& e0 = imported.meshes[0];
            std::cout << "[ContentImport] First mesh: content_id=" << e0.mesh_content_id
                      << " diff='" << e0.diffuse_path << "' norm='" << e0.normal_path
                      << "' orm='" << e0.orm_path << "'" << std::endl;
            if (firstAlbedoId != primal::id::invalid_id) {
                auto h = primal::content::get_rhi_texture_handle(firstAlbedoId);
                std::cout << "[ContentImport] First albedo handle: " << h
                          << (h == rhi::handles::INVALID_RESOURCE ? " (INVALID)" : " (OK)") << std::endl;
            }
        }
    } else {
        std::cerr << "[TestPCGScatter] No scene file found" << std::endl;
    }

    // 3b. Register procedural meshes (cylinder=tree trunk, cone=tree crown, box=rock)
    {
        using namespace primal::content;
        auto cylGeo = create_cylinder_mesh(0.15f, 2.0f, 8);
        auto coneGeo = create_cone_mesh(0.8f, 2.5f, 12);
        auto boxGeo  = create_box_mesh(0.4f, 0.4f, 0.4f);
        auto torusGeo = create_torus_mesh(0.6f, 0.15f, 16, 8);
        auto capGeo  = create_capsule_mesh(0.2f, 1.5f, 8, 4);
        auto hemiGeo = create_hemisphere_mesh(0.5f, 12, 6);
        auto pyrGeo  = create_pyramid_mesh(0.8f, 1.5f);
        auto discGeo = create_disc_mesh(0.5f, 12);
        auto planeGeo = create_plane_mesh(2.0f, 2.0f, 4, 4);

        // Register via pipeline to get slot indices (white default texture)
        primal::id::id_type noTex[3] = {
            primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
        };
        primal::id::id_type cylEnt [[maybe_unused]] = pipeline->RegisterMeshEntity(cylGeo, noTex, 3);
        primal::id::id_type coneEnt [[maybe_unused]] = pipeline->RegisterMeshEntity(coneGeo, noTex, 3);
        primal::id::id_type boxEnt [[maybe_unused]] = pipeline->RegisterMeshEntity(boxGeo, noTex, 3);
        pipeline->RegisterMeshEntity(torusGeo, noTex, 3);
        pipeline->RegisterMeshEntity(capGeo, noTex, 3);
        pipeline->RegisterMeshEntity(hemiGeo, noTex, 3);
        pipeline->RegisterMeshEntity(pyrGeo, noTex, 3);
        pipeline->RegisterMeshEntity(discGeo, noTex, 3);
        pipeline->RegisterMeshEntity(planeGeo, noTex, 3);

        // Get the slot indices from ForwardSceneRenderer
        auto* fwd = pipeline->GetForwardRenderer();
        if (fwd) {
            u32 meshCount = fwd->GetMeshInfoCount();
            // Last 9 are our procedural meshes
            slot_cylinder_ = meshCount - 9;
            slot_cone_     = meshCount - 8;
            slot_box_      = meshCount - 7;
            slot_torus_    = meshCount - 6;
            slot_capsule_  = meshCount - 5;
            slot_hemisphere_ = meshCount - 4;
            slot_pyramid_  = meshCount - 3;
            slot_disc_     = meshCount - 2;
            slot_plane_    = meshCount - 1;
            procedural_slot_base_ = slot_cylinder_;
        }
        std::cout << "[ProceduralMesh] Registered 9 meshes: cyl=" << slot_cylinder_
                  << " cone=" << slot_cone_ << " box=" << slot_box_
                  << " torus=" << slot_torus_ << " capsule=" << slot_capsule_
                  << " hemi=" << slot_hemisphere_ << " pyr=" << slot_pyramid_
                  << " disc=" << slot_disc_ << " plane=" << slot_plane_ << std::endl;
    }

    // 4. Run Phase 2 CPU unit tests (no GPU required)
    RunPhase2UnitTests();

    // 4b. Run Phase 2.5 editor API unit tests
    RunPhase25UnitTests();

    // 5. Build and execute PCG graph
    ExecutePCGGraph();

    // 5b. Execute field-driven scatter (Geometry → Field → PCG → Entity)
    ExecuteFieldDrivenScatter();

    // 5c. Create geometry demo entities (Line/Arc/Spline/Polyline at Y=5)
    CreateGeometryDemo();

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

    std::cout << "[TestPCGScatter] Ready. WASD=Move, QE=Up/Down, Arrows=Rotate, R=ReScatter, T=+Count, G=-Count, N=NewSeed, ESC=Quit" << std::endl;
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
    primal::jobsystem::JobSystem::Shutdown();
}

// ============================================================================
// ============================================================================
// PCGScatterTestCase::RunPhase2UnitTests
// ============================================================================

void PCGScatterTestCase::RunPhase2UnitTests() {
    using namespace primal::graphics::pcg;
    u32 pass = 0, fail = 0;

    auto check = [&](bool cond, const char* name) {
        if (cond) { pass++; }
        else { fail++; std::cerr << "  FAIL: " << name << std::endl; }
    };

    std::cout << "[Phase2Test] === P2-2: Y-axis Rotation ===" << std::endl;
    {
        PCGPointSet points;
        points.Init(10);
        for (u32 i = 0; i < 10; ++i) {
            points.positions[i] = {0, 1, (float)i};
            ScatterContext::WriteDefaultAttributes(points, 42);
        }

        // Verify default RotationY is 0
        bool allZero = true;
        for (u32 i = 0; i < 10; ++i)
            allZero &= (points.GetAttr(i, PCGAttr::RotationY) == 0.0f);
        check(allZero, "ScatterContext::WriteDefaultAttributes sets RotationY=0");

        // TransformNode with rotation_range=6.28
        TransformNode tn;
        tn.rotation_range = 6.2832f;
        tn.seed = 77;
        tn.inputs.resize(1);
        tn.inputs[0].data = &points;
        tn.Execute();
        auto* out = tn.outputs[0].AsPointSet();

        bool hasNonZeroRotation = false;
        bool allInRange = true;
        for (u32 i = 0; i < out->count; ++i) {
            f32 rot = out->GetAttr(i, PCGAttr::RotationY);
            if (rot != 0.0f) hasNonZeroRotation = true;
            allInRange &= (rot >= 0.0f && rot <= 6.2832f);
        }
        check(hasNonZeroRotation, "TransformNode produces non-zero RotationY");
        check(allInRange, "RotationY in [0, rotation_range]");

        // PCGInstanceBuilder encodes rotation
        PCGInstanceBuilder builder;
        std::vector<PCGInstanceData> instances;
        builder.Build(*out, instances);
        check(instances.size() == out->count, "InstanceBuilder output count matches");

        // Verify rotation is encoded in model matrix (non-identity)
        // If rotation=0 and scale=1 and pos=0, matrix column 0 = (1,0,0,0)
        // With rotation, it should differ
        bool matrixReflectsRotation = false;
        for (u32 i = 0; i < std::min(out->count, 5u); ++i) {
            f32 rot = out->GetAttr(i, PCGAttr::RotationY);
            if (std::abs(rot) > 0.01f) {
                // Column 0 of the model matrix should reflect cos(angle) * scaleX
                simd::float4 c0 = instances[i].model_matrix.columns[0];
                f32 sx = out->GetAttr(i, PCGAttr::ScaleX);
                if (sx == 0.0f) sx = 1.0f;
                f32 expected_cos = std::cos(rot) * sx;
                matrixReflectsRotation |= (std::abs(c0[0] - expected_cos) < 0.001f);
            }
        }
        check(matrixReflectsRotation, "Model matrix encodes RotationY correctly");
    }

    std::cout << "[Phase2Test] === P2-3: Worley / Ridged Noise ===" << std::endl;
    {
        // Test Worley noise output range
        f32 worley_min = 1e10f, worley_max = -1e10f;
        for (u32 i = 0; i < 100; ++i) {
            f32 v = WorleyF1_2D(i * 0.37f, i * 0.71f, 42);
            worley_min = std::min(worley_min, v);
            worley_max = std::max(worley_max, v);
        }
        check(worley_min >= 0.0f, "WorleyF1 output >= 0");
        check(worley_max > 0.0f, "WorleyF1 produces non-zero values");

        // Test Ridged noise output range
        f32 ridged_min = 1e10f, ridged_max = -1e10f;
        for (u32 i = 0; i < 100; ++i) {
            f32 v = RidgedFBM2D(i * 0.37f, i * 0.71f, 4, 2.0f, 0.5f, 42);
            ridged_min = std::min(ridged_min, v);
            ridged_max = std::max(ridged_max, v);
        }
        check(ridged_min >= 0.0f, "RidgedFBM output >= 0");
        check(ridged_max <= 1.0f, "RidgedFBM output <= 1");
        check(ridged_max > 0.0f, "RidgedFBM produces non-zero values");

        // Test NoiseFieldNode with different noise types produces different output
        PCGNoiseField simplexField, worleyField, ridgedField;

        NoiseFieldNode sfn;
        sfn.noise_type = PCGNoiseField::NoiseType::Simplex;
        sfn.frequency = 0.05f;
        sfn.seed = 99;
        sfn.Execute();
        simplexField = *static_cast<PCGNoiseField*>(sfn.outputs[0].data);

        NoiseFieldNode wfn;
        wfn.noise_type = PCGNoiseField::NoiseType::Worley;
        wfn.frequency = 0.05f;
        wfn.seed = 99;
        wfn.Execute();
        worleyField = *static_cast<PCGNoiseField*>(wfn.outputs[0].data);

        NoiseFieldNode rfn;
        rfn.noise_type = PCGNoiseField::NoiseType::Ridged;
        rfn.frequency = 0.05f;
        rfn.seed = 99;
        rfn.Execute();
        ridgedField = *static_cast<PCGNoiseField*>(rfn.outputs[0].data);

        math::v3 testPos{10.0f, 0.0f, 10.0f};
        f32 sv = simplexField.SampleFloat(testPos);
        f32 wv = worleyField.SampleFloat(testPos);
        f32 rv = ridgedField.SampleFloat(testPos);

        check(sv != wv, "Simplex and Worley produce different values at same point");
        check(sv != rv, "Simplex and Ridged produce different values at same point");
        check(wv >= 0.0f, "Worley field output >= 0");
        check(rv >= 0.0f && rv <= 1.0f, "Ridged field output in [0,1]");
    }

    std::cout << "[Phase2Test] === P2-4: HalfToFloat (SDF Readback) ===" << std::endl;
    {
        // Test known half→float conversions
        using PCGSDFReadbackManager = primal::graphics::pcg::PCGSDFReadbackManager;

        // half 0x3C00 = 1.0
        u16 h_one = 0x3C00;
        f32 f_one = PCGSDFReadbackManager::HalfToFloat(h_one);
        check(std::abs(f_one - 1.0f) < 0.001f, "HalfToFloat(0x3C00) ≈ 1.0");

        // half 0x4000 = 2.0
        u16 h_two = 0x4000;
        f32 f_two = PCGSDFReadbackManager::HalfToFloat(h_two);
        check(std::abs(f_two - 2.0f) < 0.001f, "HalfToFloat(0x4000) ≈ 2.0");

        // half 0x0000 = 0.0
        u16 h_zero = 0x0000;
        f32 f_zero = PCGSDFReadbackManager::HalfToFloat(h_zero);
        check(f_zero == 0.0f, "HalfToFloat(0x0000) = 0.0");

        // half 0x8000 = -0.0
        u16 h_neg_zero = 0x8000;
        f32 f_neg_zero = PCGSDFReadbackManager::HalfToFloat(h_neg_zero);
        check(f_neg_zero == 0.0f, "HalfToFloat(0x8000) = -0.0");

        // half 0xBC00 = -1.0
        u16 h_neg_one = 0xBC00;
        f32 f_neg_one = PCGSDFReadbackManager::HalfToFloat(h_neg_one);
        check(std::abs(f_neg_one - (-1.0f)) < 0.001f, "HalfToFloat(0xBC00) ≈ -1.0");

        // half 0x3555 ≈ 0.333
        u16 h_third = 0x3555;
        f32 f_third = PCGSDFReadbackManager::HalfToFloat(h_third);
        check(f_third > 0.3f && f_third < 0.4f, "HalfToFloat(0x3555) ≈ 0.333");
    }

    std::cout << "[Phase2Test] === P2-5: Serialization ===" << std::endl;
    {
        // Test node factory
        auto n1 = PCGSerializer::CreateNode("NoiseField");
        check(n1 != nullptr, "CreateNode('NoiseField') returns non-null");
        check(strcmp(n1->TypeName(), "NoiseField") == 0, "Created node has correct TypeName");

        auto n2 = PCGSerializer::CreateNode("FieldScatter");
        check(n2 != nullptr, "CreateNode('FieldScatter') returns non-null");

        auto n_bad = PCGSerializer::CreateNode("NonExistent");
        check(n_bad == nullptr, "CreateNode('NonExistent') returns null");

        // Test SetParam
        auto noise = PCGSerializer::CreateNode("NoiseField");
        PCGSerializer::SetParam(noise.get(), "frequency", 0.1f);
        PCGSerializer::SetParam(noise.get(), "octaves", 3.0f);
        PCGSerializer::SetParam(noise.get(), "seed", 55.0f);
        auto* nf = static_cast<NoiseFieldNode*>(noise.get());
        check(std::abs(nf->frequency - 0.1f) < 0.001f, "SetParam frequency works");
        check(nf->octaves == 3, "SetParam octaves works");
        check(nf->seed == 55, "SetParam seed works");

        // Test SerializeNode
        std::string serialized = PCGSerializer::SerializeNode(*noise);
        check(serialized.find("\"type\": \"NoiseField\"") != std::string::npos,
              "SerializeNode contains type field");
        check(serialized.find("\"frequency\"") != std::string::npos,
              "SerializeNode contains frequency param");

        // Test DeserializeGraph round-trip
        std::vector<std::unique_ptr<PCGNode>> nodes;
        std::vector<std::array<u32, 4>> conns;
        std::string json = R"({
            "nodes": [
                {"type": "NoiseField", "params": {"frequency": 0.05, "octaves": 4, "seed": 123}},
                {"type": "FieldScatter", "params": {"target_count": 500, "seed": 42}},
                {"type": "Transform", "params": {"rotation_range": 3.14, "position_jitter": 0.5, "seed": 7}}
            ],
            "connections": [
                {"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}
            ]
        })";
        bool ok = PCGSerializer::DeserializeGraph(json, nodes, conns);
        check(ok, "DeserializeGraph succeeds");
        check(nodes.size() == 3, "DeserializeGraph creates 3 nodes");
        check(conns.size() == 1, "DeserializeGraph creates 1 connection");
        check(conns[0][0] == 0 && conns[0][2] == 1, "Connection: node 0 → node 1");

        // Verify deserialized params
        auto* dn = static_cast<NoiseFieldNode*>(nodes[0].get());
        check(std::abs(dn->frequency - 0.05f) < 0.001f, "Deserialized frequency = 0.05");
        check(dn->octaves == 4, "Deserialized octaves = 4");
        check(dn->seed == 123, "Deserialized seed = 123");

        auto* dt = static_cast<TransformNode*>(nodes[2].get());
        check(std::abs(dt->rotation_range - 3.14f) < 0.01f, "Deserialized rotation_range = 3.14");
        check(std::abs(dt->position_jitter - 0.5f) < 0.001f, "Deserialized position_jitter = 0.5");
    }

    std::cout << "[Phase2Test] === Full graph with Worley noise ===" << std::endl;
    {
        PCGGraph graph;

        auto noiseNode = std::make_unique<NoiseFieldNode>();
        noiseNode->noise_type = PCGNoiseField::NoiseType::Worley;
        noiseNode->frequency = 0.05f;
        noiseNode->seed = 200;
        u32 nid = graph.AddNode(std::move(noiseNode));

        auto scatterNode = std::make_unique<FieldScatterNode>();
        scatterNode->target_count = 500;
        scatterNode->bounds_min = {-20, 1, -20};
        scatterNode->bounds_max = {20, 3, 20};
        scatterNode->seed = 42;
        u32 sid = graph.AddNode(std::move(scatterNode));

        auto transformNode = std::make_unique<TransformNode>();
        transformNode->rotation_range = 3.14159f;
        transformNode->seed = 77;
        u32 tid = graph.AddNode(std::move(transformNode));

        graph.Connect(nid, 0, sid, 0);
        graph.Connect(sid, 0, tid, 0);
        graph.Execute();

        auto* points = graph.GetOutputPoints(tid);
        check(points != nullptr && points->count > 0, "Worley graph produces > 0 points");

        bool hasRotation = false;
        for (u32 i = 0; i < std::min(points->count, 20u); ++i) {
            f32 rot = points->GetAttr(i, PCGAttr::RotationY);
            if (rot > 0.01f) hasRotation = true;
        }
        check(hasRotation, "Worley+Transform graph has RotationY > 0");
    }

    std::cout << "[Phase2Test] === Full graph with Ridged noise ===" << std::endl;
    {
        PCGGraph graph;

        auto noiseNode = std::make_unique<NoiseFieldNode>();
        noiseNode->noise_type = PCGNoiseField::NoiseType::Ridged;
        noiseNode->frequency = 0.08f;
        noiseNode->seed = 300;
        u32 nid = graph.AddNode(std::move(noiseNode));

        auto scatterNode = std::make_unique<FieldScatterNode>();
        scatterNode->target_count = 800;
        scatterNode->bounds_min = {-30, 0, -30};
        scatterNode->bounds_max = {30, 5, 30};
        scatterNode->seed = 11;
        u32 sid = graph.AddNode(std::move(scatterNode));

        auto sdfNode = std::make_unique<SDFConstraintNode>();
        sdfNode->min_dist = 0.1f;
        sdfNode->max_dist = 100.0f;
        u32 sdfid = graph.AddNode(std::move(sdfNode));

        auto refNode = std::make_unique<ReferenceFieldNode>();
        u32 refid = graph.AddNode(std::move(refNode));

        graph.Connect(nid, 0, sid, 0);
        graph.Connect(sid, 0, sdfid, 0);
        graph.Connect(refid, 0, sdfid, 1);
        graph.Execute();

        auto* points = graph.GetOutputPoints(sdfid);
        check(points != nullptr && points->count > 0, "Ridged+Scatter+SDF graph produces > 0 points");

        // Verify Ridged density values are in [0,1]
        bool densityValid = true;
        for (u32 i = 0; i < points->count; ++i) {
            f32 d = points->GetAttr(i, PCGAttr::Density);
            densityValid &= (d >= 0.0f && d <= 1.0f);
        }
        check(densityValid, "Ridged graph density values in [0,1]");
    }

    std::cout << "[Phase2Test] === Full Serialize round-trip ===" << std::endl;
    {
        // Build a graph, serialize it, deserialize, verify params match
        PCGGraph src_graph;

        auto noiseNode = std::make_unique<NoiseFieldNode>();
        noiseNode->frequency = 0.07f;
        noiseNode->octaves = 5;
        noiseNode->seed = 999;
        u32 nid = src_graph.AddNode(std::move(noiseNode));

        auto scatterNode = std::make_unique<FieldScatterNode>();
        scatterNode->target_count = 200;
        scatterNode->seed = 42;
        scatterNode->bounds_min = {-30, 0, -30};
        scatterNode->bounds_max = {30, 5, 30};
        u32 sid = src_graph.AddNode(std::move(scatterNode));

        auto transformNode = std::make_unique<TransformNode>();
        transformNode->rotation_range = 1.57f;
        transformNode->position_jitter = 0.3f;
        transformNode->seed = 7;
        transformNode->scale_min = {0.5f, 0.5f, 0.5f};
        transformNode->scale_max = {2.0f, 2.0f, 2.0f};
        u32 tid = src_graph.AddNode(std::move(transformNode));

        auto meshNode = std::make_unique<MeshAssignNode>();
        meshNode->weights = {0.6f, 0.3f, 0.1f};
        u32 mid = src_graph.AddNode(std::move(meshNode));

        src_graph.Connect(nid, 0, sid, 0);
        src_graph.Connect(sid, 0, tid, 0);
        src_graph.Connect(tid, 0, mid, 0);

        // Serialize
        std::string json = PCGSerializer::Serialize(src_graph);
        check(!json.empty() && json != "{}", "Serialize produces non-empty JSON");
        check(json.find("\"nodes\"") != std::string::npos, "JSON contains nodes array");
        check(json.find("\"connections\"") != std::string::npos, "JSON contains connections array");
        check(json.find("\"NoiseField\"") != std::string::npos, "JSON contains NoiseField");
        check(json.find("\"MeshAssign\"") != std::string::npos, "JSON contains MeshAssign");
        check(json.find("0.6") != std::string::npos, "JSON contains mesh weight 0.6");

        // Verify connection count in JSON
        check(json.find("\"from_node\":0") != std::string::npos, "JSON has connection from node 0");
        check(json.find("\"from_node\":1") != std::string::npos, "JSON has connection from node 1");
        check(json.find("\"from_node\":2") != std::string::npos, "JSON has connection from node 2");

        // Deserialize
        std::vector<std::unique_ptr<PCGNode>> d_nodes;
        std::vector<std::array<u32, 4>> d_conns;
        bool ok = PCGSerializer::DeserializeGraph(json, d_nodes, d_conns);
        check(ok, "Deserialize succeeds on serialized output");
        check(d_nodes.size() == 4, "Deserialized 4 nodes");
        check(d_conns.size() == 3, "Deserialized 3 connections");

        // Verify scalar params survived round-trip
        auto* d_noise = static_cast<NoiseFieldNode*>(d_nodes[0].get());
        check(std::abs(d_noise->frequency - 0.07f) < 0.01f, "Round-trip noise frequency");
        check(d_noise->octaves == 5, "Round-trip noise octaves");
        check(d_noise->seed == 999, "Round-trip noise seed");

        auto* d_scatter = static_cast<FieldScatterNode*>(d_nodes[1].get());
        check(d_scatter->target_count == 200, "Round-trip scatter target_count");
        check(std::abs(d_scatter->bounds_min.x - (-30.f)) < 0.01f, "Round-trip bounds_min.x");
        check(std::abs(d_scatter->bounds_max.x - 30.f) < 0.01f, "Round-trip bounds_max.x");

        auto* d_transform = static_cast<TransformNode*>(d_nodes[2].get());
        check(std::abs(d_transform->rotation_range - 1.57f) < 0.01f, "Round-trip rotation_range");
        check(std::abs(d_transform->position_jitter - 0.3f) < 0.01f, "Round-trip position_jitter");
        check(std::abs(d_transform->scale_min.x - 0.5f) < 0.01f, "Round-trip scale_min.x");
        check(std::abs(d_transform->scale_max.x - 2.0f) < 0.01f, "Round-trip scale_max.x");

        auto* d_mesh = static_cast<MeshAssignNode*>(d_nodes[3].get());
        check(d_mesh->weights.size() == 3, "Round-trip mesh weights count");
        if (d_mesh->weights.size() >= 3) {
            check(std::abs(d_mesh->weights[0] - 0.6f) < 0.01f, "Round-trip weight[0]");
            check(std::abs(d_mesh->weights[2] - 0.1f) < 0.01f, "Round-trip weight[2]");
        }
    }

    std::cout << "[Phase2Test] === P2-6: GPU Compute Scatter ===" << std::endl;
    {
        using namespace primal::graphics::pcg;

        PCGScatterCompute gpu_scatter;
        bool init_ok = gpu_scatter.Initialize(device.get());
        check(init_ok, "PCGScatterCompute::Initialize succeeds");
        check(gpu_scatter.IsInitialized(), "PCGScatterCompute reports initialized");

        if (init_ok) {
            PCGScatterCompute::ScatterParams params;
            params.bounds_min = {-20, 1, -20};
            params.bounds_max = {20, 3, 20};
            params.target_count = 500;
            params.seed = 42;
            params.density_scale = 1.0f;
            params.use_noise = true;
            gpu_scatter.SetParams(params);

            auto points = gpu_scatter.DispatchAndReadback();
            check(points.count > 0, "GPU scatter produces > 0 points");
            check(points.count < 1000, "GPU scatter count < 1000 (within bounds)");

            // Verify positions are within bounds
            bool bounds_ok = true;
            for (u32 i = 0; i < std::min(points.count, 20u); ++i) {
                auto& p = points.positions[i];
                if (p.x < params.bounds_min.x - 1 || p.x > params.bounds_max.x + 1 ||
                    p.z < params.bounds_min.z - 1 || p.z > params.bounds_max.z + 1) {
                    bounds_ok = false;
                    break;
                }
            }
            check(bounds_ok, "GPU scatter positions within bounds");
        }
    }
    if (fail > 0) {
        std::cerr << "[Phase2Test] SOME TESTS FAILED" << std::endl;
    } else {
        std::cout << "[Phase2Test] All tests passed" << std::endl;
    }
}

// ============================================================================
// PCGScatterTestCase::RunPhase25UnitTests
// ============================================================================

void PCGScatterTestCase::RunPhase25UnitTests() {
    using namespace primal::graphics::pcg;
    u32 fail = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::cerr << "[Phase25Test] FAIL: " << msg << std::endl; ++fail; }
    };

    // --- 1. Reflection: param descriptors for each node type ---
    std::cout << "[Phase25Test] === P25-1: Param Descriptors ===" << std::endl;
    {
        auto noise = std::make_unique<NoiseFieldNode>();
        u32 count;
        auto* descs = noise->GetParamDescriptors(count);
        check(count == 6, "NoiseFieldNode has 6 params");
        check(std::strcmp(descs[0].name, "noise_type") == 0, "NoiseField param[0] = noise_type");
        check(std::strcmp(descs[1].name, "frequency") == 0, "NoiseField param[1] = frequency");
        check(descs[0].type == PCGParamType::Enum, "noise_type is Enum");
        check(descs[1].type == PCGParamType::Float, "frequency is Float");

        auto scatter = std::make_unique<FieldScatterNode>();
        descs = scatter->GetParamDescriptors(count);
        check(count == 5, "FieldScatterNode has 5 params");

        auto transform = std::make_unique<TransformNode>();
        descs = transform->GetParamDescriptors(count);
        check(count == 5, "TransformNode has 5 params");
        check(descs[0].type == PCGParamType::Vec3, "scale_min is Vec3");

        auto mesh = std::make_unique<MeshAssignNode>();
        descs = mesh->GetParamDescriptors(count);
        check(count == 1, "MeshAssignNode has 1 param");
        check(descs[0].type == PCGParamType::FloatArray, "weights is FloatArray");
    }

    // --- 2. SetParamByName ---
    std::cout << "[Phase25Test] === P25-2: SetParamByName ===" << std::endl;
    {
        auto noise = std::make_unique<NoiseFieldNode>();
        check(noise->SetParamByName("frequency", 0.1f), "Set frequency");
        check(std::abs(noise->frequency - 0.1f) < 0.001f, "frequency == 0.1");
        check(noise->SetParamByName("octaves", 4.0f), "Set octaves");
        check(noise->octaves == 4, "octaves == 4");
        check(!noise->SetParamByName("nonexistent", 1.0f), "Nonexistent param fails");

        auto scatter = std::make_unique<FieldScatterNode>();
        check(scatter->SetParamByName("target_count", 500.0f), "Set target_count");
        check(scatter->target_count == 500, "target_count == 500");

        auto transform = std::make_unique<TransformNode>();
        check(transform->SetParamByName("scale_min", math::v3{0.5f, 0.5f, 0.5f}), "Set scale_min v3");
        check(std::abs(transform->scale_min.x - 0.5f) < 0.001f, "scale_min.x == 0.5");

        auto mesh = std::make_unique<MeshAssignNode>();
        f32 weights[] = {0.6f, 0.3f, 0.1f};
        check(mesh->SetParamArrayByName("weights", weights, 3), "Set weights array");
        check(mesh->weights.size() == 3, "weights count == 3");
        check(std::abs(mesh->weights[0] - 0.6f) < 0.001f, "weights[0] == 0.6");
    }

    // --- 3. Pin descriptors ---
    std::cout << "[Phase25Test] === P25-3: Pin Descriptors ===" << std::endl;
    {
        auto noise = std::make_unique<NoiseFieldNode>();
        u32 count;
        auto* pins = noise->GetPinDescriptors(count);
        check(count == 1, "NoiseFieldNode has 1 pin");
        check(!pins[0].is_input, "NoiseField pin is output");
        check(pins[0].data_type == PCGDataType::Field, "NoiseField pin is Field");

        auto sdf = std::make_unique<SDFConstraintNode>();
        pins = sdf->GetPinDescriptors(count);
        check(count == 3, "SDFConstraintNode has 3 pins");
        check(pins[0].is_input && pins[1].is_input && !pins[2].is_input,
              "SDFConstraint: 2 inputs, 1 output");
    }

    // --- 4. Graph mutation: RemoveNode ---
    std::cout << "[Phase25Test] === P25-4: Graph Mutation ===" << std::endl;
    {
        PCGGraph graph;
        auto n0 = graph.AddNode(std::make_unique<NoiseFieldNode>());
        auto n1 = graph.AddNode(std::make_unique<FieldScatterNode>());
        auto n2 = graph.AddNode(std::make_unique<DensityFilterNode>());
        graph.Connect(n0, 0, n1, 0);
        graph.Connect(n1, 0, n2, 0);

        check(graph.GetNodes().size() == 3, "3 nodes before remove");
        check(graph.GetConnections().size() == 2, "2 connections before remove");

        graph.RemoveNode(n1);  // Remove middle node
        check(graph.GetNodes().size() == 2, "2 nodes after remove");
        check(graph.GetConnections().size() == 0, "0 connections after remove (both referenced n1)");

        // Verify remaining nodes' indices shifted
        check(std::strcmp(graph.GetNodes()[0]->TypeName(), "NoiseField") == 0, "Node 0 is NoiseField");
        check(std::strcmp(graph.GetNodes()[1]->TypeName(), "DensityFilter") == 0, "Node 1 is DensityFilter");

        // Test Disconnect
        PCGGraph g2;
        auto a = g2.AddNode(std::make_unique<NoiseFieldNode>());
        auto b = g2.AddNode(std::make_unique<FieldScatterNode>());
        g2.Connect(a, 0, b, 0);
        check(g2.GetConnections().size() == 1, "1 connection before disconnect");
        g2.Disconnect(a, 0, b, 0);
        check(g2.GetConnections().size() == 0, "0 connections after disconnect");

        // Test Clear
        g2.Clear();
        check(g2.GetNodes().empty(), "No nodes after clear");
    }

    // --- 5. Mesh slots ---
    std::cout << "[Phase25Test] === P25-5: Mesh Slots ===" << std::endl;
    {
        PCGGraph graph;
        auto s0 = graph.AddMeshSlot("Content/Tree.model", "Oak Tree");
        auto s1 = graph.AddMeshSlot("Content/Rock.model", "Granite");
        check(s0 == 0 && s1 == 1, "Slot indices 0 and 1");
        check(graph.GetMeshSlotCount() == 2, "2 mesh slots");
        check(std::strcmp(graph.GetMeshSlot(0).name.c_str(), "Oak Tree") == 0, "Slot 0 name");
        check(std::strcmp(graph.GetMeshSlot(1).path.c_str(), "Content/Rock.model") == 0, "Slot 1 path");

        graph.SetMeshSlot(0, "Content/Pine.model", "Pine");
        check(std::strcmp(graph.GetMeshSlot(0).name.c_str(), "Pine") == 0, "Slot 0 updated");
        check(std::strcmp(graph.GetMeshSlot(0).path.c_str(), "Content/Pine.model") == 0, "Slot 0 path updated");

        graph.RemoveMeshSlot(0);
        check(graph.GetMeshSlotCount() == 1, "1 slot after remove");
        check(std::strcmp(graph.GetMeshSlot(0).name.c_str(), "Granite") == 0, "Remaining slot is Granite");
    }

    // --- 6. Error reporting ---
    std::cout << "[Phase25Test] === P25-6: Error Reporting ===" << std::endl;
    {
        PCGGraph graph;
        graph.AddNode(std::make_unique<FieldScatterNode>()); // no density input → will produce output regardless
        graph.Execute();
        // ScatterNode with no input still executes (produces 0 points)
        // Let's test with a node that has inputs but no upstream connection
        graph.Clear();
        auto scatter_id = graph.AddNode(std::make_unique<FieldScatterNode>());
        graph.Execute();
        // FieldScatterNode with no density input still produces output (it has a default path)
        // The error check is for nodes whose Execute() leaves outputs empty
    }

    // --- 7. Serializer with mesh_slots ---
    std::cout << "[Phase25Test] === P25-7: Serialize with Mesh Slots ===" << std::endl;
    {
        PCGGraph graph;
        graph.AddMeshSlot("Content/Tree.model", "Oak");
        graph.AddMeshSlot("Content/Rock.model", "Granite");

        auto noise_id = graph.AddNode(std::make_unique<NoiseFieldNode>());
        graph.GetNodes()[noise_id]->SetParamByName("frequency", 0.05f);

        auto scatter_id = graph.AddNode(std::make_unique<FieldScatterNode>());
        graph.GetNodes()[scatter_id]->SetParamByName("target_count", 100.0f);
        graph.Connect(noise_id, 0, scatter_id, 0);

        std::string json = PCGSerializer::Serialize(graph);
        check(json.find("\"mesh_slots\"") != std::string::npos, "JSON contains mesh_slots");
        check(json.find("Oak") != std::string::npos, "JSON contains Oak");
        check(json.find("Granite") != std::string::npos, "JSON contains Granite");

        // Deserialize into new graph
        PCGGraph graph2;
        bool ok = PCGSerializer::DeserializeIntoGraph(json, graph2);
        check(ok, "DeserializeIntoGraph succeeds");
        check(graph2.GetMeshSlotCount() == 2, "Deserialized 2 mesh slots");
        check(std::strcmp(graph2.GetMeshSlot(0).name.c_str(), "Oak") == 0, "Deserialized slot 0 name");
        check(graph2.GetNodes().size() == 2, "Deserialized 2 nodes");
        check(graph2.GetConnections().size() == 1, "Deserialized 1 connection");
    }

    // --- 8. Node type registry ---
    std::cout << "[Phase25Test] === P25-8: Node Type Registry ===" << std::endl;
    {
        check(PCGSerializer::GetRegisteredNodeTypeCount() == 10, "10 registered node types");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(0), "ReferenceField") == 0, "Type 0 = ReferenceField");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(6), "MeshAssign") == 0, "Type 6 = MeshAssign");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(7), "RasterizedField") == 0, "Type 7 = RasterizedField");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(8), "ScatterOnGeometry") == 0, "Type 8 = ScatterOnGeometry");
        check(std::strcmp(PCGSerializer::GetRegisteredNodeTypeName(9), "CurveAlign") == 0, "Type 9 = CurveAlign");
        check(PCGSerializer::GetRegisteredNodeTypeName(10) == nullptr, "Type 10 = null (out of range)");
    }

    if (fail > 0) {
        std::cerr << "[Phase25Test] " << fail << " TESTS FAILED" << std::endl;
    } else {
        std::cout << "[Phase25Test] All tests passed" << std::endl;
    }
}

// ============================================================================
// PCGScatterTestCase::ExecutePCGGraph
// ============================================================================

void PCGScatterTestCase::ExecutePCGGraph() {
    using namespace primal::graphics::pcg;

    pcg_graph_ = std::make_unique<PCGGraph>();

    // Node 0: ReferenceField (ground plane SDF)
    auto refNode = std::make_unique<ReferenceFieldNode>();
    pcg_graph_->AddNode(std::move(refNode)); // node 0

    // Node 1: NoiseField (density variation)
    auto noiseNode = std::make_unique<NoiseFieldNode>();
    noiseNode->frequency = 0.05f;
    noiseNode->octaves = 4;
    noiseNode->seed = 123;
    pcg_noise_node_id_ = pcg_graph_->AddNode(std::move(noiseNode)); // node 1

    // Node 2: FieldScatter (density from noise, scatter in volume)
    auto scatterNode = std::make_unique<FieldScatterNode>();
    scatterNode->target_count = pcg_target_count_;
    scatterNode->bounds_min = {-40, 1, -40};
    scatterNode->bounds_max = {40, 3, 40};
    scatterNode->seed = 42;
    scatterNode->points_per_unit_area = 1.0f;
    pcg_scatter_node_id_ = pcg_graph_->AddNode(std::move(scatterNode)); // node 2

    // Node 3: SDFConstraint (keep points above ground: SDF > 0)
    auto sdfNode = std::make_unique<SDFConstraintNode>();
    sdfNode->min_dist = 0.5f;
    sdfNode->max_dist = 100.0f;
    pcg_graph_->AddNode(std::move(sdfNode)); // node 3

    // Node 4: DensityFilter (keep points with density in [0.3, 1.0])
    auto densityFilterNode = std::make_unique<DensityFilterNode>();
    densityFilterNode->min_density = 0.3f;
    densityFilterNode->max_density = 1.0f;
    pcg_graph_->AddNode(std::move(densityFilterNode)); // node 4

    // Node 5: Transform (random scale + position jitter)
    auto transformNode = std::make_unique<TransformNode>();
    transformNode->scale_min = {0.3f, 0.3f, 0.3f};
    transformNode->scale_max = {1.0f, 2.0f, 1.0f};
    transformNode->position_jitter = 0.5f;
    transformNode->seed = 99;
    pcg_graph_->AddNode(std::move(transformNode)); // node 5

    // Node 6: MeshAssign (weighted: cylinder, cone, box, hemisphere, pyramid)
    auto meshAssignNode = std::make_unique<MeshAssignNode>();
    meshAssignNode->weights = {0.30f, 0.20f, 0.10f, 0.25f, 0.15f};
    pcg_mesh_node_id_ = pcg_graph_->AddNode(std::move(meshAssignNode)); // node 6

    // Connect: Noise → Scatter (density input)
    pcg_graph_->Connect(1, 0, 2, 0);
    // Connect: Scatter → SDFConstraint (point set input)
    pcg_graph_->Connect(2, 0, 3, 0);
    // Connect: ReferenceField → SDFConstraint (SDF input)
    pcg_graph_->Connect(0, 0, 3, 1);
    // Connect: SDFConstraint → DensityFilter
    pcg_graph_->Connect(3, 0, 4, 0);
    // Connect: DensityFilter → Transform
    pcg_graph_->Connect(4, 0, 5, 0);
    // Connect: Transform → MeshAssign
    pcg_graph_->Connect(5, 0, 6, 0);

    // Execute
    pcg_graph_->Execute();

    // Verify intermediate results
    auto* scatterOut = pcg_graph_->GetOutputPoints(2);
    auto* sdfOut = pcg_graph_->GetOutputPoints(3);
    auto* filterOut = pcg_graph_->GetOutputPoints(4);
    std::cout << "[TestPCGScatter] Pipeline: scatter=" << (scatterOut ? scatterOut->count : 0)
              << " → sdf=" << (sdfOut ? sdfOut->count : 0)
              << " → density_filter=" << (filterOut ? filterOut->count : 0) << std::endl;

    auto* points = pcg_graph_->GetOutputPoints(6);
    if (!points || points->count == 0) {
        std::cerr << "[TestPCGScatter] PCG graph produced 0 points!" << std::endl;
        return;
    }

    // Verify multi-mesh assignment: count mesh indices
    u32 meshCounts[5] = {};
    for (u32 i = 0; i < points->count; ++i) {
        u32 idx = static_cast<u32>(points->GetAttr(i, PCGAttr::MeshIndex));
        if (idx < 5) meshCounts[idx]++;
    }
    std::cout << "[TestPCGScatter] Mesh assignment: cyl=" << meshCounts[0]
              << " cone=" << meshCounts[1] << " box=" << meshCounts[2]
              << " hemi=" << meshCounts[3] << " pyr=" << meshCounts[4]
              << " (expected ~30/20/10/25/15)" << std::endl;

    std::cout << "[TestPCGScatter] PCG graph produced " << points->count << " points" << std::endl;

    // Create ECS entities from PCG points (Phase 3b)
    auto factoryResult = pcg::PCGEntityFactory::CreateEntities(*points);

    // Offset mesh_slot_indices from abstract {0,1,2} to actual procedural mesh slots
    for (auto& slot : factoryResult.mesh_slot_indices) {
        slot += procedural_slot_base_;
    }

    // Verify entities were created
    u32 alive_count = 0;
    for (auto eid : factoryResult.entity_ids) {
        if (primal::game_entity::is_alive(primal::game_entity::entity_id{eid}))
            alive_count++;
    }
    std::cout << "[TestPCGScatter] ECS entities: " << alive_count << "/" << factoryResult.entity_ids.size()
              << " alive" << std::endl;

    // Store for hot-reload
    pcg_entity_ids_ = factoryResult.entity_ids;
    pcg_mesh_slots_ = factoryResult.mesh_slot_indices;

    // Pass entity IDs + mesh slots to pipeline for RenderScene sync (Phase 3c)
    pipeline->SetPCGEntities(std::move(factoryResult.entity_ids),
                              std::move(factoryResult.mesh_slot_indices));

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
// PCGScatterTestCase::ExecuteFieldDrivenScatter
// ============================================================================

void PCGScatterTestCase::ExecuteFieldDrivenScatter() {
    using namespace primal::graphics::pcg;
    namespace geo = primal::geometry;

    geo::init();

    // Curve 1: at Y=15 for direct on-curve scatter (visible above Sponza)
    std::vector<primal::math::v3> pathPtsHigh;
    pathPtsHigh.push_back(primal::math::v3{-20, 15, 0});
    pathPtsHigh.push_back(primal::math::v3{-10, 15, 8});
    pathPtsHigh.push_back(primal::math::v3{0, 15, -5});
    pathPtsHigh.push_back(primal::math::v3{10, 15, 6});
    pathPtsHigh.push_back(primal::math::v3{20, 15, 0});
    auto curveHigh = geo::create_spline(pathPtsHigh, false);
    curve_handle_ = curveHigh;

    // Curve 2: at Y=0 for field-based scatter (ground level)
    std::vector<primal::math::v3> pathPtsGround;
    pathPtsGround.push_back(primal::math::v3{-20, 0, 0});
    pathPtsGround.push_back(primal::math::v3{-10, 0, 8});
    pathPtsGround.push_back(primal::math::v3{0, 0, -5});
    pathPtsGround.push_back(primal::math::v3{10, 0, 6});
    pathPtsGround.push_back(primal::math::v3{20, 0, 0});
    auto curveGround = geo::create_spline(pathPtsGround, false);

    std::vector<primal::id::id_type> all_geometry_entities;
    std::vector<u32> all_mesh_slots;

    // ========================================================================
    // Path 1: Direct scatter ON curve (ScatterOnGeometryNode)
    // ========================================================================
    {
        PCGGraph graph;

        auto scatterNode = std::make_unique<ScatterOnGeometryNode>();
        scatterNode->geometry_handles.push_back(curveHigh);
        scatterNode->target_count = 30;
        scatterNode->position_jitter = 0.3f;
        scatterNode->seed = 42;
        u32 sid = graph.AddNode(std::move(scatterNode));

        auto transformNode = std::make_unique<TransformNode>();
        transformNode->scale_min = primal::math::v3{0.2f, 0.3f, 0.2f};
        transformNode->scale_max = primal::math::v3{0.5f, 1.2f, 0.5f};
        transformNode->position_jitter = 0.0f;
        transformNode->rotation_range = 0.0f; // preserve curve-aligned RotationY
        transformNode->seed = 77;
        u32 tid = graph.AddNode(std::move(transformNode));

        auto meshNode = std::make_unique<MeshAssignNode>();
        meshNode->weights = {0.5f, 0.5f}; // cylinder + cone (trees along curve)
        u32 mid = graph.AddNode(std::move(meshNode));

        graph.Connect(sid, 0, tid, 0);
        graph.Connect(tid, 0, mid, 0);
        graph.Execute();

        auto* points = graph.GetOutputPoints(mid);
        if (points && points->count > 0) {
            // Verify points are near the curve
            u32 on_curve = 0;
            for (u32 i = 0; i < points->count; ++i) {
                f32 d = geo::distance_to(curveHigh, points->positions[i]);
                if (d < 2.0f) on_curve++;
            }
            std::cout << "[DirectScatter] " << points->count << " points, "
                      << on_curve << " near curve" << std::endl;

            // Verify RotationY varies (not all the same)
            f32 first_rot = points->GetAttr(0, PCGAttr::RotationY);
            bool varies = false;
            for (u32 i = 1; i < points->count; ++i) {
                if (std::abs(points->GetAttr(i, PCGAttr::RotationY) - first_rot) > 0.1f) {
                    varies = true;
                    break;
                }
            }
            std::cout << "[DirectScatter] RotationY varies: " << (varies ? "YES" : "NO") << std::endl;

            auto result = pcg::PCGEntityFactory::CreateEntities(*points);
            // Offset mesh slots to actual procedural mesh slots
            for (auto& slot : result.mesh_slot_indices) {
                slot += procedural_slot_base_;
            }
            for (auto eid : result.entity_ids) all_geometry_entities.push_back(eid);
            for (auto slot : result.mesh_slot_indices) all_mesh_slots.push_back(slot);
        }
    }

    // ========================================================================
    // Path 2: Field-based scatter + CurveAlign
    // ========================================================================
    {
        PCGGraph graph;

        auto rasterNode = std::make_unique<RasterizedFieldNode>();
        rasterNode->geometry_handles.push_back(curveGround);
        rasterNode->bounds_min = primal::math::v3{-25, -2, -15};
        rasterNode->bounds_max = primal::math::v3{25, 4, 15};
        rasterNode->resolution_x = 64;
        rasterNode->resolution_y = 16;
        rasterNode->resolution_z = 40;
        rasterNode->band_width = 5.0f;
        rasterNode->union_mode = true;
        u32 rid = graph.AddNode(std::move(rasterNode));

        auto scatterNode = std::make_unique<FieldScatterNode>();
        scatterNode->target_count = 2000;
        scatterNode->bounds_min = primal::math::v3{-24, 0.5f, -14};
        scatterNode->bounds_max = primal::math::v3{24, 3, 14};
        scatterNode->seed = 88;
        scatterNode->points_per_unit_area = 1.0f;
        u32 sid = graph.AddNode(std::move(scatterNode));

        auto alignNode = std::make_unique<CurveAlignNode>();
        alignNode->geometry_handles.push_back(curveGround);
        u32 aid = graph.AddNode(std::move(alignNode));

        auto transformNode = std::make_unique<TransformNode>();
        transformNode->scale_min = primal::math::v3{0.15f, 0.2f, 0.15f};
        transformNode->scale_max = primal::math::v3{0.4f, 0.8f, 0.4f};
        transformNode->position_jitter = 0.0f;
        transformNode->rotation_range = 0.0f;
        transformNode->seed = 33;
        u32 tid = graph.AddNode(std::move(transformNode));

        auto meshNode = std::make_unique<MeshAssignNode>();
        meshNode->weights = {0.6f, 0.4f}; // cylinder + cone
        u32 mid = graph.AddNode(std::move(meshNode));

        graph.Connect(rid, 0, sid, 0);
        graph.Connect(sid, 0, aid, 0);
        graph.Connect(aid, 0, tid, 0);
        graph.Connect(tid, 0, mid, 0);
        graph.Execute();

        auto* points = graph.GetOutputPoints(mid);
        if (points && points->count > 0) {
            std::cout << "[FieldScatter+Align] " << points->count << " points" << std::endl;
            auto result = pcg::PCGEntityFactory::CreateEntities(*points);
            // Offset mesh slots to actual procedural mesh slots
            for (auto& slot : result.mesh_slot_indices) {
                slot += procedural_slot_base_;
            }
            for (auto eid : result.entity_ids) all_geometry_entities.push_back(eid);
            for (auto slot : result.mesh_slot_indices) all_mesh_slots.push_back(slot);
        }
    }

    // ========================================================================
    // Path 3: Surface scatter on procedural mesh
    // ========================================================================
    {
        PCGGraph graph;

        auto surfNode = std::make_unique<SurfaceScatterNode>();
        // Use cylinder mesh (slot_cylinder_ was registered from geometry_content_id)
        // We need the geometry_content_id, not the slot. Use the content ID.
        // The slot is slot_cylinder_, but SurfaceScatterNode needs geometry_content_id.
        // We registered cylinder via create_cylinder_mesh() which returned a content ID.
        // But we didn't save it. Let's just use slot_cylinder_ to find the content ID.
        // Actually, we can use the first Sponza mesh for surface scatter (floor-like).
        // For now, skip if no mesh available — the sampler will use whatever geometry_content_id is set.
        // Let's scatter on the cone mesh for a visually interesting result.
        surfNode->target_count = 200;
        surfNode->normal_offset = 0.5f;
        surfNode->seed = 55;
        // geometry_content_id will be 0 by default (invalid). We'd need to save it.
        // For now, just add the node to verify compilation.
        u32 ssid = graph.AddNode(std::move(surfNode));

        auto transformNode = std::make_unique<TransformNode>();
        transformNode->scale_min = primal::math::v3{0.05f, 0.05f, 0.05f};
        transformNode->scale_max = primal::math::v3{0.15f, 0.15f, 0.15f};
        transformNode->rotation_range = 6.28f;
        transformNode->position_jitter = 0.0f;
        transformNode->seed = 44;
        u32 tid = graph.AddNode(std::move(transformNode));

        auto meshNode = std::make_unique<MeshAssignNode>();
        meshNode->weights = {1.0f}; // single mesh (box = rock)
        u32 mid = graph.AddNode(std::move(meshNode));

        graph.Connect(ssid, 0, tid, 0);
        graph.Connect(tid, 0, mid, 0);

        // Note: SurfaceScatterNode with invalid geometry_content_id will produce 0 points
        // This path is a skeleton for future use when mesh content IDs are tracked
        graph.Execute();

        auto* points = graph.GetOutputPoints(mid);
        if (points && points->count > 0) {
            std::cout << "[SurfaceScatter] " << points->count << " points on mesh surface" << std::endl;
            auto result = pcg::PCGEntityFactory::CreateEntities(*points);
            for (auto& slot : result.mesh_slot_indices) {
                slot += procedural_slot_base_;
            }
            for (auto eid : result.entity_ids) all_geometry_entities.push_back(eid);
            for (auto slot : result.mesh_slot_indices) all_mesh_slots.push_back(slot);
        } else {
            std::cout << "[SurfaceScatter] Skipped (no geometry_content_id set)" << std::endl;
        }
    }

    // Pass all scatter entities to pipeline for rendering
    pipeline->SetPCGEntities(std::move(all_geometry_entities),
                              std::move(all_mesh_slots));
}

// ============================================================================
// PCGScatterTestCase::ReScatterPCG (hot-reload)
// ============================================================================

void PCGScatterTestCase::ReScatterPCG() {
    using namespace primal::graphics::pcg;
    if (!pcg_graph_ || !pipeline) return;

    // Remove old proxies from RenderScene before destroying entities
    if (!pcg_entity_ids_.empty() && scene) {
        for (auto eid : pcg_entity_ids_) {
            scene->RemoveProxy(eid);
        }
    }

    // Destroy old entities
    if (!pcg_entity_ids_.empty()) {
        PCGEntityFactory::DestroyEntities(pcg_entity_ids_);
        pcg_entity_ids_.clear();
        pcg_mesh_slots_.clear();
    }

    // Update scatter count
    auto& nodes = pcg_graph_->GetNodes();
    if (pcg_scatter_node_id_ < nodes.size()) {
        auto* scatter = static_cast<FieldScatterNode*>(nodes[pcg_scatter_node_id_].get());
        scatter->target_count = pcg_target_count_;
    }

    // Re-execute
    pcg_graph_->Execute();

    auto* points = pcg_graph_->GetOutputPoints(pcg_mesh_node_id_);
    if (!points || points->count == 0) {
        std::cerr << "[ReScatterPCG] 0 points!" << std::endl;
        pipeline->SetPCGEntities({}, {});
        return;
    }

    auto result = PCGEntityFactory::CreateEntities(*points);
    for (auto& slot : result.mesh_slot_indices) {
        slot += procedural_slot_base_;
    }

    pcg_entity_ids_ = result.entity_ids;
    pcg_mesh_slots_ = result.mesh_slot_indices;

    pipeline->SetPCGEntities(std::move(result.entity_ids),
                              std::move(result.mesh_slot_indices));
    std::cout << "[ReScatterPCG] " << points->count << " instances (target=" << pcg_target_count_ << ")" << std::endl;
}

// ============================================================================
// PCGScatterTestCase::CreateGeometryDemo
// ============================================================================

void PCGScatterTestCase::CreateGeometryDemo() {
    using namespace primal::geometry;
    using namespace primal::game_entity;

    std::vector<primal::id::id_type> geom_entity_ids;

    // 0. Scatter curve line (green) at Y=15
    if (curve_handle_.is_valid()) {
        entity e = create();
        component::init_info gi{};
        gi.handle = curve_handle_;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // All demo geometries placed at Y=5, spread along X and Z
    auto mk = [](f32 x, f32 y, f32 z) -> primal::math::v3 {
        return {x, y, z};
    };

    // 1. Line: horizontal segment
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(0, 5, 0));
        pts.push_back(mk(3, 5, 0));
        auto handle = create(GeometryType::Line, pts);

        entity e = create();
        component::init_info gi{};
        gi.handle = handle;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // 2. Arc: three-point semicircle
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(0, 5, 2));
        pts.push_back(mk(1.5f, 6.5f, 2));
        pts.push_back(mk(3, 5, 2));
        auto handle = create_arc_three_point(pts);

        entity e = create();
        component::init_info gi{};
        gi.handle = handle;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // 3. Spline: wavy curve
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(0, 5, 4));
        pts.push_back(mk(1, 6, 4));
        pts.push_back(mk(2, 4, 4));
        pts.push_back(mk(3, 5, 4));
        auto handle = create_spline(pts, false);

        entity e = create();
        component::init_info gi{};
        gi.handle = handle;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // 4. Polyline: staircase
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(0, 5, 6));
        pts.push_back(mk(0, 6, 6));
        pts.push_back(mk(1, 6, 6));
        pts.push_back(mk(1, 7, 6));
        pts.push_back(mk(2, 7, 6));
        std::vector<SegmentType> segs(4, SegmentType::Line);
        auto handle = create_polyline(pts, segs);

        entity e = create();
        component::init_info gi{};
        gi.handle = handle;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // 5. Larger spline arc
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(0, 5, 8));
        pts.push_back(mk(0.5f, 7, 8));
        pts.push_back(mk(2.5f, 7, 8));
        pts.push_back(mk(3, 5, 8));
        auto handle = create_spline(pts, false);

        entity e = create();
        component::init_info gi{};
        gi.handle = handle;
        e.Add<primal::component::Geometry>(gi);
        geom_entity_ids.push_back(e.get_id());
    }

    // 6. Offset demo: original spline at Y=5, Z=12 + parallel offset
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(-5, 5, 12));
        pts.push_back(mk(-2, 6, 12));
        pts.push_back(mk(2, 6, 12));
        pts.push_back(mk(5, 5, 12));
        auto src = create_spline(pts, false);
        auto off = offset(src, 1.0f);

        // Original
        { entity e = create(); component::init_info gi{}; gi.handle = src;
          e.Add<primal::component::Geometry>(gi); geom_entity_ids.push_back(e.get_id()); }
        // Offset (parallel line)
        { entity e = create(); component::init_info gi{}; gi.handle = off;
          e.Add<primal::component::Geometry>(gi); geom_entity_ids.push_back(e.get_id()); }
    }

    // 7. Trim demo: full spline at Y=5, Z=14 + trimmed [0.2, 0.8]
    {
        std::vector<primal::math::v3> pts;
        pts.push_back(mk(-5, 5, 14));
        pts.push_back(mk(-2, 7, 14));
        pts.push_back(mk(2, 7, 14));
        pts.push_back(mk(5, 5, 14));
        auto src = create_spline(pts, false);
        auto trimmed = trim(src, 0.2f, 0.8f);

        // Full curve
        { entity e = create(); component::init_info gi{}; gi.handle = src;
          e.Add<primal::component::Geometry>(gi); geom_entity_ids.push_back(e.get_id()); }
        // Trimmed portion
        { entity e = create(); component::init_info gi{}; gi.handle = trimmed;
          e.Add<primal::component::Geometry>(gi); geom_entity_ids.push_back(e.get_id()); }
    }

    pipeline->SetGeometryEntities(std::move(geom_entity_ids));
    std::cout << "[GeometryDemo] Created " << geom_entity_ids.size()
              << " geometry entities (Line/Arc/Spline/Polyline/Spline2)" << std::endl;
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

    // PCG hot-reload keys
    get(input_source::keyboard, input_code::key_r, val);
    if (val.current.x > 0.0f) {
        if (!key_r_pressed_) { key_r_pressed_ = true; ReScatterPCG(); }
    } else { key_r_pressed_ = false; }

    get(input_source::keyboard, input_code::key_t, val);
    if (val.current.x > 0.0f) {
        if (!key_t_pressed_) { key_t_pressed_ = true; pcg_target_count_ = std::min(pcg_target_count_ + 500, 10000u); ReScatterPCG(); }
    } else { key_t_pressed_ = false; }

    get(input_source::keyboard, input_code::key_g, val);
    if (val.current.x > 0.0f) {
        if (!key_g_pressed_) { key_g_pressed_ = true; pcg_target_count_ = (pcg_target_count_ > 500) ? pcg_target_count_ - 500 : 100u; ReScatterPCG(); }
    } else { key_g_pressed_ = false; }

    get(input_source::keyboard, input_code::key_n, val);
    if (val.current.x > 0.0f) {
        if (!key_n_pressed_) {
            key_n_pressed_ = true;
            if (pcg_graph_) {
                auto& nodes = pcg_graph_->GetNodes();
                if (pcg_noise_node_id_ < nodes.size()) {
                    auto* noise = static_cast<pcg::NoiseFieldNode*>(nodes[pcg_noise_node_id_].get());
                    noise->seed = std::rand() % 9999;
                    ReScatterPCG();
                }
            }
        }
    } else { key_n_pressed_ = false; }
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
