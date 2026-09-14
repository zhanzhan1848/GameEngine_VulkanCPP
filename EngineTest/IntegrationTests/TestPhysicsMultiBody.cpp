// TestPhysicsMultiBody.cpp — T1 多物体刚体可视化测试（M1 验收）。
//
// 三场景顺序执行（每场景独立重建物理世界，保证确定性基线可复现）：
//   Tower    10 层 ×10 错缝砖塔：安定 150 帧（帧 90 与帧 150 哈希逐位一致 =
//            全员安定 60 帧）→ 底层侧向冲量推倒 300 帧 → 塔顶砖坠落断言；
//   Pyramid  15 层金字塔：安定 120 帧 → 底部冲量坍塌 240 帧 → 顶点坠落断言；
//   Ladder   8 球弹性阶梯（restitution 0.05..0.75）：首次触地后的反弹峰值
//            随弹性系数单调不减，且首尾差 > 0.5m。
//
// 可视化：双管线隔离（主管线 RWC→backbuffer 窗口实时；捕获管线高层 Render→
// 离屏 RGBA8 RT）。物理驱动走引擎侧 owner→ECS transform 回写
//（PhysicsWorld::sync_bound_transforms），网格实体由 SyncEntitiesToRenderScene
// 自动跟随；全体 body 线框 proxy 进 debug_draw 队列（线框路径稳定，§10-7
// 的网格缺失问题不影响物理断言）。
//
// 基线：各场景安定哈希写入/比对 Testing/Baselines/physics-multibody-hash.txt。
// 退出码 _Exit(pass?0:1)。

#include "TestPhysicsMultiBody.h"

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ProceduralMesh.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Entity.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/DebugDraw/DebugDrawQueue.h"
#include "Engine/Simulation/SimulationAPI.h"

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::math;
namespace simulation = primal::simulation;

namespace {

bool g_all_checks_pass{ true };

void report(bool pass, const char* name)
{
    if (!pass) g_all_checks_pass = false;
    std::cout << "[TestPhysicsMultiBody] " << (pass ? "PASS" : "FAIL") << ": " << name << std::endl;
}

constexpr u32 kTowerLayers = 10;
constexpr u32 kTowerPerLayer = 10;
constexpr u32 kPyramidLayers = 15;
constexpr u32 kLadderCount = 8;

} // anonymous namespace

// ============================================================================
// 渲染通道（管线 + 场景 + 代理实体表）
// ============================================================================

struct multibody_channel
{
    StandardRenderPipeline* pipeline = nullptr;
    RenderScene* scene = nullptr;
    std::vector<primal::id::id_type> proxies;   // 与 body specs 顺序一一对应

    bool init(rhi::RHIDeviceBase* device, u32 w, u32 h)
    {
        pipeline = new StandardRenderPipeline();
        if (!pipeline->Initialize(device)) return false;
        pipeline->SetViewportSize(w, h);
        lumen::LumenConfig cfg;
        cfg.quality = lumen::LumenQualityPreset::Low;
        pipeline->SetLumenConfig(cfg);
        pipeline->SetEditorMode(true);
        scene = new RenderScene();
        return true;
    }

    primal::id::id_type add_proxy(primal::id::id_type geometry_content_id, const v3& pos)
    {
        const primal::id::id_type noTex[3] = {
            primal::id::invalid_id, primal::id::invalid_id, primal::id::invalid_id
        };
        primal::id::id_type eid = pipeline->RegisterMeshEntity(geometry_content_id, noTex, 3);
        if (eid != primal::id::invalid_id) {
            primal::transform::component tc{ primal::transform::transform_id{ eid } };
            primal::transform::set_position(tc, pos);
            proxies.push_back(eid);
        }
        return eid;
    }

    void clear_proxies()
    {
        for (auto eid : proxies) {
            pipeline->UnregisterMeshEntity(eid);
        }
        proxies.clear();
    }

    void shutdown()
    {
        clear_proxies();
        if (pipeline) {
            pipeline->Shutdown();
            delete pipeline;
            pipeline = nullptr;
        }
        if (scene) { delete scene; scene = nullptr; }
    }
};

// ============================================================================
// 场景布局生成器（确定性：纯公式）
// ============================================================================

namespace {

struct body_spec
{
    simulation::body_desc desc;
    v3 proxy_pos;
};

body_spec make_ground()
{
    body_spec g{};
    g.desc.m = simulation::body_desc::mode::static_body;
    g.desc.half_extents = v3{ 20.f, 0.5f, 20.f };
    g.desc.position = v3{ 0.f, -0.5f, 0.f };
    g.desc.restitution = 0.05f;
    g.proxy_pos = g.desc.position;
    return g;
}

void make_tower(std::vector<body_spec>& out, u32& apex_spec_index)
{
    // 偶数层 10 砖（x=-4.5..4.5），奇数层 9 砖（x=-4..4，每砖被上下两层完整支撑，
    // 无悬臂端——悬臂会造成永久微抖动、无法安定）
    out.push_back(make_ground());
    for (u32 L = 0; L < kTowerLayers; ++L) {
        const f32 y = 0.25f + L * 0.5f;
        const u32 count = (L % 2 == 0) ? 10u : 9u;
        const f32 x0 = (L % 2 == 0) ? -4.5f : -4.0f;
        for (u32 i = 0; i < count; ++i) {
            body_spec b{};
            b.desc.m = simulation::body_desc::mode::dynamic;
            b.desc.half_extents = v3{ 0.5f, 0.25f, 0.5f };
            b.desc.position = v3{ x0 + i * 1.0f, y, 0.f };
            b.desc.restitution = 0.05f;
            b.proxy_pos = b.desc.position;
            out.push_back(b);
        }
    }
    // apex = 顶层（9 砖）中间砖 = 末尾倒数第 5 块
    apex_spec_index = static_cast<u32>(out.size()) - 5;
}

void make_pyramid(std::vector<body_spec>& out, u32& apex_spec_index)
{
    out.push_back(make_ground());
    for (u32 j = 0; j < kPyramidLayers; ++j) {          // j: 自底向上
        const u32 count = kPyramidLayers - j;
        const f32 y = 0.25f + j * 0.5f;
        for (u32 i = 0; i < count; ++i) {
            body_spec b{};
            b.desc.m = simulation::body_desc::mode::dynamic;
            b.desc.half_extents = v3{ 0.5f, 0.25f, 0.5f };
            b.desc.position = v3{ (static_cast<f32>(i) - static_cast<f32>(count - 1) * 0.5f), y, 0.f };
            b.desc.restitution = 0.05f;
            b.proxy_pos = b.desc.position;
            out.push_back(b);
        }
    }
    // apex = 最顶层唯一盒：ground(1) + Σ_{j=0..13}(15-j) = 1+119 = 120
    u32 total = 1;
    for (u32 j = 0; j < kPyramidLayers - 1; ++j) total += kPyramidLayers - j;
    apex_spec_index = total;
}

void make_ladder(std::vector<body_spec>& out, std::vector<f32>& restitutions,
                 u32& apex_spec_index)
{
    out.push_back(make_ground());
    for (u32 i = 0; i < kLadderCount; ++i) {
        body_spec b{};
        b.desc.s = simulation::body_desc::shape::sphere;
        b.desc.m = simulation::body_desc::mode::dynamic;
        b.desc.half_extents = v3{ 0.4f, 0.4f, 0.4f };   // x = 半径
        b.desc.position = v3{ -5.25f + i * 1.5f, 3.f, 0.f };
        b.desc.restitution = 0.05f + i * 0.1f;
        restitutions.push_back(b.desc.restitution);
        b.proxy_pos = b.desc.position;
        out.push_back(b);
    }
    apex_spec_index = out.size() - 1;
}

} // anonymous namespace

// ============================================================================
// 测试用例
// ============================================================================

Engine_Test::Engine_Test()
    : primal::test::RenderTestRunner(std::make_unique<PhysicsMultiBodyTestCase>())
{}

bool PhysicsMultiBodyTestCase::Initialize()
{
    std::cout << "[TestPhysicsMultiBody] Initializing..." << std::endl;

    // 1. 窗口
    primal::platform::window_init_info winInfo{};
    winInfo.caption = "TestPhysicsMultiBody - T1";
    winInfo.width = window_width_;
    winInfo.height = window_height_;
    window = primal::platform::create_window(&winInfo);
    if (!window.is_valid()) return false;

    // 2. Metal device
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    auto* metalDevice = new MetalDevice(desc);
    if (!metalDevice || !metalDevice->Initialize()) {
        delete metalDevice;
        return false;
    }
    device.reset(metalDevice);
    g_deviceManager.RegisterDevice(device.get());

    // 3. RenderSystem
    RenderSystemInitInfo sysInfo;
    sysInfo.device = device.get();
    sysInfo.window = window.handle();
    sysInfo.width = window_width_;
    sysInfo.height = window_height_;
    if (!renderSystem.Initialize(sysInfo)) return false;

    // 4. 双渲染通道
    main_ch = new multibody_channel();
    cap_ch = new multibody_channel();
    if (!main_ch->init(device.get(), window_width_, window_height_) ||
        !cap_ch->init(device.get(), window_width_, window_height_)) {
        return false;
    }

    // 5. 离屏捕获 RT
    capture_desc = TextureDesc{};
    capture_desc.size = { window_width_, window_height_, 1 };
    capture_desc.format = DataFormat::RGBA8_UNorm;
    capture_desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    capture_desc.mipLevels = 1;
    capture_desc.arraySize = 1;
    auto* capture_rt = new RenderTexture();
    if (!capture_rt->Create(device.get(), 0x80000101u, capture_desc)) {
        delete capture_rt;
        return false;
    }
    capture_target = capture_rt->GetHandle();
    capture_rt_.reset(capture_rt);

    // 6. 视图 + 相机（静态全景俯视）
    view = new RenderView();
    ViewportDesc viewport;
    viewport.size.x = static_cast<f32>(window_width_);
    viewport.size.y = static_cast<f32>(window_height_);
    view->SetViewport(viewport);
    {
        const v3 cam_pos{ 13.f, 7.f, 13.f };
        const f32 pitch = -0.32f;
        const f32 cp = std::cos(pitch), sp = std::sin(pitch);
        const v3 forward{ cp, sp, -cp };   // yaw=0：沿 -Z 看向原点
        const v3 target = cam_pos + simd_normalize(forward);
        const m4x4 viewMat = metal::CreateLookAtMatrix(cam_pos, target, v3{ 0.f, 1.f, 0.f });
        constexpr f32 fov = 60.f * (pi / 180.f);
        const f32 aspect = static_cast<f32>(window_width_) / static_cast<f32>(window_height_);
        view->SetViewMatrix(viewMat);
        view->SetProjectionMatrix(metal::CreatePerspectiveMatrix(fov, aspect, 0.1f, 1000.f));
    }

    run_start_ = std::chrono::steady_clock::now();
    std::cout << "[TestPhysicsMultiBody] Ready" << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// 场景构建
// ---------------------------------------------------------------------------

void PhysicsMultiBodyTestCase::start_scenario(u32 index)
{
    scenario_index_ = index;
    sc_frame_ = 0;
    sc_min_apex_y_ = 1e9f;
    sc_settled_hash_ = 0;
    sc_pre_settle_hash_ = 0;
    ladder_restitutions_.clear();
    ladder_peaks_.clear();
    ladder_touched_.clear();

    std::vector<body_spec> specs;
    u32 apex_spec_index = 0;

    switch (index) {
        case 0:
            sc_name_ = "tower"; sc_settle_frames_ = 240; sc_total_frames_ = 480;
            tower_ball_body_ = 0; ball_spawned_ = false;
            make_tower(specs, apex_spec_index);
            break;
        case 1:
            sc_name_ = "pyramid"; sc_settle_frames_ = 180; sc_total_frames_ = 420;
            sc_apex_x0_ = 0.f; sc_max_apex_dx_ = 0.f;
            make_pyramid(specs, apex_spec_index);
            break;
        case 2:
        default:
            sc_name_ = "ladder"; sc_settle_frames_ = 0; sc_total_frames_ = 200;
            make_ladder(specs, ladder_restitutions_, apex_spec_index);
            ladder_peaks_.assign(kLadderCount, 0.f);
            ladder_touched_.assign(kLadderCount, false);
            break;
    }

    // 物理世界重建（确定性基线前提：干净状态 + 固定创建顺序）
    simulation::settings phys{};
    phys.gravity = v3{ 0.f, -9.81f, 0.f };
    simulation::shutdown();
    if (!simulation::initialize(phys)) {
        std::cerr << "[TestPhysicsMultiBody] physics init failed" << std::endl;
        pass_ = false;
        return;
    }

    for (size_t i = 0; i < specs.size(); ++i) {
        const bool is_sphere = specs[i].desc.s == simulation::body_desc::shape::sphere;
        const primal::id::id_type geo = is_sphere
            ? primal::content::create_sphere_mesh(specs[i].desc.half_extents.x, 24, 12)
            : primal::content::create_box_mesh(specs[i].desc.half_extents.x * 2.f,
                                               specs[i].desc.half_extents.y * 2.f,
                                               specs[i].desc.half_extents.z * 2.f);
        const u64 main_eid = main_ch->add_proxy(geo, specs[i].proxy_pos);
        cap_ch->add_proxy(geo, specs[i].proxy_pos);

        simulation::body_desc d = specs[i].desc;
        if (i > 0 && main_eid != primal::id::invalid_id) {
            // owner 绑定：物理 tick 自动回写主通道代理的 ECS transform
            d.owner = primal::game_entity::entity{ primal::game_entity::entity_id{ static_cast<primal::id::id_type>(main_eid) } };
        }
        const u64 bid = simulation::body_create(d);
        if (i == static_cast<size_t>(apex_spec_index)) {
            sc_apex_body_ = bid;
            sc_apex_initial_y_ = specs[i].desc.position.y;
        }
    }
    simulation::set_running(true);

    std::cout << "[TestPhysicsMultiBody] scenario '" << sc_name_ << "' started: bodies="
              << specs.size() << " settle=" << sc_settle_frames_
              << " total=" << sc_total_frames_ << std::endl;
}

void PhysicsMultiBodyTestCase::finish_scenario()
{
    // 场景收尾断言
    if (sc_name_ == std::string("tower")) {
        // 重球轰击后塔身坍塌：apex（顶砖）最低点坠落至 2m 以下（初始 4.75）
        report(sc_min_apex_y_ < 2.0f, "tower collapsed under impact (apex min y < 2.0)");
        std::cout << "[TestPhysicsMultiBody] tower apex min y: " << sc_min_apex_y_ << std::endl;
    }
    if (sc_name_ == std::string("pyramid")) {
        // 底层整体飞出 → 上方 14 层失去支撑坠落：apex 最低点比初始低 ≥0.3m（一层 0.5m）
        const bool dropped = sc_min_apex_y_ < sc_apex_initial_y_ - 0.3f;
        report(dropped, "pyramid collapsed after base removed (apex drop >= 0.3)");
        std::cout << "[TestPhysicsMultiBody] pyramid apex: initial=" << sc_apex_initial_y_
                  << " min=" << sc_min_apex_y_ << " max_dx=" << sc_max_apex_dx_ << std::endl;
    }

    if (sc_name_ == std::string("ladder")) {
        // 弹性阶梯：峰值单调不减（容差 0.05）且首尾差 > 0.5m
        bool mono = true;
        for (u32 i = 1; i < ladder_peaks_.size(); ++i) {
            if (ladder_peaks_[i] < ladder_peaks_[i - 1] - 0.05f) mono = false;
        }
        const bool spread = (ladder_peaks_.back() - ladder_peaks_.front()) > 0.5f;
        report(mono && spread, "restitution ladder: rebound peaks monotonic");
        for (u32 i = 0; i < ladder_peaks_.size(); ++i) {
            std::cout << "[TestPhysicsMultiBody]   ball " << i
                      << " (CoR " << ladder_restitutions_[i] << ") peak y="
                      << ladder_peaks_[i] << std::endl;
        }
    }

    // 基线比对/更新
    const std::string bpath = baseline_path();
    const bool update = std::getenv("PRIMAL_UPDATE_PHYS_BASELINE") != nullptr &&
                        (sc_name_ == std::string("tower") || sc_name_ == std::string("pyramid"));
    if (update) {
        std::ofstream out(bpath, std::ios::app);
        out << sc_name_ << " " << sc_settled_hash_ << "\n";
        std::cout << "[TestPhysicsMultiBody] baseline appended: " << sc_name_
                  << " " << sc_settled_hash_ << std::endl;
    } else if (sc_name_ == std::string("tower") || sc_name_ == std::string("pyramid")) {
        std::ifstream in(bpath);
        bool found = false;
        u64 baseline_hash = 0;
        std::string line;
        while (std::getline(in, line)) {
            std::string n;
            u64 h = 0;
            std::istringstream iss(line);
            if (iss >> n >> h && n == sc_name_) {
                found = true;
                baseline_hash = h;   // 取最后一条（重复追加时以最新为准）
            }
        }
        if (found) {
            report(baseline_hash == sc_settled_hash_, "settled hash matches baseline");
            if (baseline_hash != sc_settled_hash_) {
                std::cout << "[TestPhysicsMultiBody] baseline=" << baseline_hash
                          << " actual=" << sc_settled_hash_ << std::endl;
            }
        } else {
            report(false, "settled hash matches baseline (baseline entry missing)");
        }
    }
}

void PhysicsMultiBodyTestCase::capture_png(const char* tag)
{
    std::filesystem::path dir = std::filesystem::path("test-output") / "physics-multibody";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / (std::string(sc_name_) + "-" + tag + ".png")).string();

    const u64 pixel_bytes = static_cast<u64>(window_width_) * window_height_ * 4;
    std::vector<u8> pixels(pixel_bytes);
    if (capture_rt_ && capture_rt_->ReadBack(device.get(), pixels.data(), pixel_bytes)) {
        stbi_write_png(path.c_str(),
                       static_cast<int>(window_width_), static_cast<int>(window_height_),
                       4, pixels.data(), static_cast<int>(window_width_) * 4);
        std::cout << "[TestPhysicsMultiBody] captured -> " << path << std::endl;
    }
}

std::string PhysicsMultiBodyTestCase::baseline_path() const
{
    // 工作目录随运行方式变化（手动 = Darwin/Debug；ctest = 构建子目录）——
    // 从当前目录向上找仓库根的 Testing/Baselines
    std::filesystem::path dir = std::filesystem::current_path();
    for (int i = 0; i < 6; ++i) {
        std::filesystem::path cand = dir / "Testing" / "Baselines";
        if (std::filesystem::exists(cand)) {
            return (cand / "physics-multibody-hash.txt").string();
        }
        dir = dir.parent_path();
        if (dir == dir.parent_path()) break;   // 到达根
    }
    return "Testing/Baselines/physics-multibody-hash.txt";
}

void PhysicsMultiBodyTestCase::Run()
{
    if (main_ch == nullptr || cap_ch == nullptr || !view) return;

    if (scenario_index_ >= 3) return;   // 全部完成（等待断言退出）
    if (!scenario_started_) {
        start_scenario(scenario_index_);
        scenario_started_ = true;
        if (!simulation::is_initialized()) return;
    }

    // 1. 物理步进（固定 1/60；owner 回写在 step 内部完成）
    simulation::step(1.f / 60.f);
    ++sc_frame_;
    ++global_frame_;

    // 线框 proxy 可视化（全体 body → debug_draw 队列 → Pass 6）
    simulation::debug_draw_bodies();

    // 2. 场景专属跟踪
    if (scenario_index_ == 2) {
        // ladder：各球首次触地后的反弹峰值
        for (u32 i = 0; i < kLadderCount; ++i) {
            simulation::body_transform t{};
            const u64 bid = 2 + i;   // body id: 1=地面，球=2..9
            if (!simulation::body_get_transform(bid, t)) continue;
            if (!ladder_touched_[i]) {
                if (sc_frame_ > 5 && t.position.y < 0.5f) ladder_touched_[i] = true;
            } else if (t.position.y > ladder_peaks_[i]) {
                ladder_peaks_[i] = t.position.y;
            }
        }
    } else {
        // tower/pyramid：冲量后 apex 位姿追踪
        if (sc_frame_ > sc_settle_frames_) {
            simulation::body_transform t{};
            if (simulation::body_get_transform(sc_apex_body_, t)) {
                if (t.position.y < sc_min_apex_y_) sc_min_apex_y_ = t.position.y;
                if (scenario_index_ == 1) {
                    const f32 dx = std::fabs(t.position.x - sc_apex_x0_);
                    if (dx > sc_max_apex_dx_) sc_max_apex_dx_ = dx;
                }
            }
        }
    }

    // 3. 安定哈希与冲量
    if (sc_frame_ == sc_settle_frames_ - 60 && sc_settle_frames_ >= 60) {
        sc_pre_settle_hash_ = simulation::determinism_hash();
    }
    if (sc_frame_ == sc_settle_frames_) {
        sc_settled_hash_ = simulation::determinism_hash();
        if (sc_settle_frames_ >= 60) {
            report(sc_settled_hash_ == sc_pre_settle_hash_,
                   "settled: hash static across 60 frames (all bodies asleep)");
        }
        if (scenario_index_ == 1) {
            simulation::body_transform t{};
            if (simulation::body_get_transform(sc_apex_body_, t)) sc_apex_x0_ = t.position.x;
        }
        // 施加冲量（body id 从 1 起：1=地面）。塔：中部两砖侧向踢出（制造倾覆矩）；
        // 金字塔：底部中间两块侧向踢出（掏空支撑）。
        // Tower：重球侧击（50kg @ 30 m/s）撞碎塔身；Pyramid：底层统一冲量（连贯平移）
        if (scenario_index_ == 0) {
            if (!ball_spawned_) {
                const primal::id::id_type ball_geo =
                    primal::content::create_sphere_mesh(1.0f, 24, 12);
                const v3 ball_pos{ -15.f, 4.5f, 0.f };
                const u64 ball_eid = main_ch->add_proxy(ball_geo, ball_pos);
                cap_ch->add_proxy(ball_geo, ball_pos);
                simulation::body_desc bd{};
                bd.s = simulation::body_desc::shape::sphere;
                bd.m = simulation::body_desc::mode::dynamic;
                bd.half_extents = v3{ 1.f, 1.f, 1.f };
                bd.position = ball_pos;
                bd.mass = 50.f;
                bd.restitution = 0.1f;
                if (ball_eid != primal::id::invalid_id) {
                    bd.owner = primal::game_entity::entity{
                        primal::game_entity::entity_id{ static_cast<primal::id::id_type>(ball_eid) } };
                }
                tower_ball_body_ = simulation::body_create(bd);
                simulation::body_set_linear_velocity(tower_ball_body_, v3{ 30.f, 0.f, 0.f });
                ball_spawned_ = true;
                std::cout << "[TestPhysicsMultiBody] impact ball spawned (id="
                          << tower_ball_body_ << ", 50kg @ 30m/s)" << std::endl;
            }
        } else if (scenario_index_ == 1) {
            for (u64 bid = 2; bid <= 16; ++bid) simulation::body_apply_impulse(bid, v3{ 100.f, 0.f, 0.f });
        }
        capture_png("settled");
    }

    // 4. 帧渲染（双管线）：主管线 → backbuffer（窗口实时）；捕获管线 → 离屏 RT
    ResourceHandle backBuffer;
    SyncHandle signalFence;
    if (!renderSystem.BeginFrame(backBuffer, signalFence)) return;

    auto* cmd = renderSystem.GetCurrentCommandBuffer();
    auto cmdHandle = renderSystem.GetCurrentCommandBufferHandle();
    u32 bufferIndex = renderSystem.GetCurrentFrameIndex();

    cmd->Reset();
    cmd->Begin();
    main_ch->pipeline->RenderWithCommandBuffer(
        *main_ch->scene, *view, backBuffer, renderSystem.GetBackBufferDesc(),
        cmd, bufferIndex, cmdHandle, signalFence);
    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = signalFence;
    device->Submit(submitInfo);
    renderSystem.EndFrame();

    cap_ch->pipeline->Render(*cap_ch->scene, *view, capture_target, capture_desc);

    // 冲量后速度探针（质量诊断：1kg 砖受 12 N·s 应得 ~12 m/s）
    if (scenario_index_ == 0 && (sc_frame_ == sc_settle_frames_ + 2 ||
                                 sc_frame_ == sc_settle_frames_ + 30)) {
        simulation::body_transform t{};
        if (simulation::body_get_transform(2, t)) {
            std::cout << "[TestPhysicsMultiBody] diag frame " << sc_frame_
                      << " body2 v=(" << t.linear_velocity.x << "," << t.linear_velocity.y
                      << "," << t.linear_velocity.z << ") y=" << t.position.y << std::endl;
        }
    }

    // 5. 场景收尾
    if (scenario_index_ == 2 && sc_frame_ == 30) {
        capture_png("air");
    }
    if (sc_frame_ >= sc_total_frames_) {
        if (sc_frame_ == sc_total_frames_) {
            capture_png("final");
        }
        finish_scenario();
        ++scenario_index_;
        scenario_started_ = false;

        if (scenario_index_ >= 3) {
            // 全场景完成：性能预算 + 线框计数
            const f32 elapsed = std::chrono::duration<f32>(
                std::chrono::steady_clock::now() - run_start_).count();
            report(elapsed < 30.f, "total wall clock < 30s budget");
            std::cout << "[TestPhysicsMultiBody] total wall clock: " << elapsed << "s" << std::endl;
            report(simulation::debug_lines_forwarded() > 0,
                   "debug wireframe lines forwarded");

            completed_ = true;
            const bool all_pass = pass_ && g_all_checks_pass;
            std::cout << "[TestPhysicsMultiBody] done: pass=" << (all_pass ? 1 : 0) << std::endl;
            std::cout.flush();
            std::cerr.flush();
            // 进程退出即回收全部资源；不做重拆除——462 次 UnregisterMeshEntity 的
            // 拆除路径存在引擎侧双重释放噪声（MetalDevice Double-free 警告 + SIGTRAP）。
            std::_Exit(all_pass ? 0 : 1);
        }
    }
}

void PhysicsMultiBodyTestCase::Shutdown()
{
    if (main_ch == nullptr || cap_ch == nullptr) return;   // 幂等
    std::cout << "[TestPhysicsMultiBody] Shutting down..." << std::endl;
    if (!completed_) {
        // 窗口被提前关闭 / runloop 提前终止：测试没跑完，不能让门禁空洞通过
        std::cout << "[TestPhysicsMultiBody] FAIL: interrupted before completion" << std::endl;
        std::cout.flush();
        simulation::shutdown();
        std::_Exit(1);
    }

    simulation::shutdown();

    main_ch->shutdown();
    delete main_ch;
    main_ch = nullptr;
    cap_ch->shutdown();
    delete cap_ch;
    cap_ch = nullptr;
    if (view) { delete view; view = nullptr; }
    if (window.is_valid()) {
        primal::platform::remove_window(window.get_id());
    }
    capture_rt_.reset();
    capture_target = handles::INVALID_RESOURCE;
    device.reset();
    primal::content::shutdown();
}
