#pragma once

// TestPhysicsMultiBody.h — T1 多物体刚体可视化测试（M1 验收，设计文档 §8.2）。
//
// 三场景顺序执行，每场景独立重建物理世界（确定性基线前提），全部带渲染可视化：
//   Tower    10 层 ×10 错缝砖塔（100 盒）：安定 150 帧（60 帧哈希不变=全员安定）
//            → 底层侧向冲量推倒 300 帧 → 塔顶砖坠落断言；
//   Pyramid  15 层金字塔（120 盒）：安定 120 帧 → 底部冲量坍塌 240 帧 → 顶点坠落；
//   Ladder   8 球弹性阶梯（restitution 0.05..0.75）：反弹峰值单调性。
// 断言：安定确定性哈希、基线比对（Testing/Baselines/physics-multibody-hash.txt，
// PRIMAL_UPDATE_PHYS_BASELINE=1 重新生成）、行为带内、性能预算 <30s。
// 可视化：双管线（窗口 RWC→backbuffer 实时；捕获管线高层 Render→离屏 PNG），
// 物理驱动走引擎侧 owner→ECS transform 回写，全体 body 线框 proxy 进 debug_draw。

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderTexture.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Platform/Platform.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// 一条渲染通道（管线 + 场景 + 代理实体表）；定义在 TestPhysicsMultiBody.cpp。
struct multibody_channel;

class PhysicsMultiBodyTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::RenderView* view = nullptr;

    multibody_channel* main_ch{ nullptr };   // 窗口通道
    multibody_channel* cap_ch{ nullptr };    // 捕获通道

    // 离屏捕获 RT
    primal::graphics::rhi::ResourceHandle capture_target;
    primal::graphics::rhi::TextureDesc capture_desc{};
    std::unique_ptr<primal::graphics::RenderTexture> capture_rt_;

    // 场景推进状态
    u32 scenario_index_{ 0 };
    bool scenario_started_{ false };
    u32 sc_frame_{ 0 };
    const char* sc_name_{ "tower" };
    u32 sc_settle_frames_{ 150 };
    u32 sc_total_frames_{ 450 };
    u64 sc_apex_body_{ 0 };
    f32 sc_apex_initial_y_{ 0.f };
    f32 sc_min_apex_y_{ 1e9f };
    u64 sc_settled_hash_{ 0 };
    u64 sc_pre_settle_hash_{ 0 };
    u64 tower_ball_body_{ 0 };
    bool ball_spawned_{ false };
    f32 sc_apex_x0_{ 0.f };
    f32 sc_max_apex_dx_{ 0.f };
    std::vector<f32> ladder_restitutions_;
    std::vector<f32> ladder_peaks_;
    std::vector<bool> ladder_touched_;

    u32 window_width_{ 960 };
    u32 window_height_{ 540 };
    u64 global_frame_{ 0 };
    bool pass_{ true };
    bool completed_{ false };
    std::chrono::steady_clock::time_point run_start_{};

    void start_scenario(u32 index);
    void finish_scenario();
    void capture_png(const char* name);
    std::string baseline_path() const;
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
