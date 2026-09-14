#pragma once

// TestPhysicsSmoke.h — T0 物理可视化冒烟（Docs/2026-09-11-physics-simulation-design.md §8.1）。
//
// 场景：静态地面 + 动态盒（restitution 0.1，快速安定）+ 高弹球（restitution 0.7，
// 回弹-衰减演示，实测反弹峰值 y≈1.66m）。固定 1/60 步长 × 默认 120 帧
//（环境变量 PRIMAL_PHYS_SMOKE_FRAMES 可覆盖，便于人工观察）。
//
// 可视化：双管线（构造上隔离渲染状态）——
//   主管线   RWC → swapchain backbuffer（窗口实时可见）；
//   捕获管线 高层 Render → 离屏 RGBA8 RT，检查点帧 {1,30,60,90,120} ReadBack
//            写 PNG（test-output/physics-smoke/）+ 亮度/像素差断言。
// 退出码：_Exit(pass?0:1) 直传（terminate() 会吞掉非零退出码——门禁失效教训）。

#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderTexture.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Platform/Platform.h"

#include <memory>
#include <string>
#include <vector>

// 一条渲染通道的专属状态（管线 + 场景 + 代理实体）；定义在 TestPhysicsSmoke.cpp。
struct render_channel;

class PhysicsSmokeTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    primal::graphics::RenderView* view = nullptr;

    render_channel* main_ch{ nullptr };   // 窗口通道
    render_channel* cap_ch{ nullptr };    // 捕获通道

    // 物理 body id
    u64 ground_body{ 0 };
    u64 box_body{ 0 };
    u64 sphere_body{ 0 };
    // 回弹追踪：首次触地后的反弹最高点
    bool sphere_touched_{ false };
    f32  sphere_bounce_peak_{ 0.f };

    // 离屏捕获 RT
    primal::graphics::rhi::ResourceHandle capture_target;
    primal::graphics::rhi::TextureDesc capture_desc{};
    std::unique_ptr<primal::graphics::RenderTexture> capture_rt_;
    f32 luma_first_{ -1.f };
    f32 luma_last_{ -1.f };
    std::vector<u8> diff_ref_pixels_;   // 帧间逐像素对比基准（frame-30）
    u64 diff_changed_pixels_{ 0 };      // frame-90 vs frame-30 差异像素数

    u32 window_width_{ 960 };
    u32 window_height_{ 540 };
    u64 frame_count_{ 0 };
    u64 frame_cap_{ 120 };
    u32 steps_simulated_{ 0 };
    bool pass_{ true };
    bool completed_{ false };   // 断言段是否执行（窗口提前关闭 = 测试未完成，判失败）
    static constexpr u32 kPhysicsStepsPerFrame = 1;  // 固定 1/60，确定性优先
};

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
