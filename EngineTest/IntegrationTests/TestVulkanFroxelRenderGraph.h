#pragma once

// TestVulkanFroxelRenderGraph — Froxel 体积雾集成门禁（2026-09-30 计划 §6）。
//
// 照 DLSS 门禁模式：复制 Sponza 脚手架 + Initialize 内 setenv 自带配置（不
// 依赖外部 env）+ gate frame 进程内断言 + [FroxelGate] PASS/FAIL 逐项打印 +
// RESULT PASS/FAIL + std::_Exit(0/1)。与 Sponza/DLSS 完全独立。
//
// 门禁项（计划 §6.4，阈值 Phase 0 标定后回填 Docs 计划文档）：
//   G1 积分健康      — 3D scatter transmittance 沿 z 单调不增、RGB 无 NaN/Inf、
//                      远端 scatter > 0（uniform 相位读回）
//   G2 常量介质覆盖  — FROXEL_UNIFORM 下密度非零占比 ≥ 阈值、无全零 slice
//   G3 体积阴影生效  — 双相位（shadows on vs FROXEL_NO_SHADOW）逐列 scatter
//                      对比：强阴影列占比 ≥ 阈值且均值比 ≤ 阈值
//   G4 SDF carve     — noise 相位：固体参考层（地面下）密度 ≈ 0、空气带 > 0
//   G5 march 自阴影  — WP1 后启用（FROXEL_GATE_AB，march0 vs march4 双相位）
//   G6 时域稳定性    — WP3 后可选（连续帧差分能量）
//
// 相位时间线（帧号 = frameCount_，FROXEL_GATE_FRAME 默认 300）：
//   0..F        uniform + shadows on（F = FROXEL_GATE_FRAME）→ 读回 A 相位
//   F..F+45     SetFroxelShadowsEnabled(false)            → 读回 B 相位（G3）
//   F+45..F+47  UpdateSettings enable_froxel_fog off→on +
//               density_uniform_debug=-1                  → noise 相位重建
//   F+47..F+95  noise 收敛                                 → 读回密度（G4）
//
// 运行：Darwin/Debug/TestVulkanFroxelRenderGraph; echo $?
// FROXEL_GATE=0 退回窗口长跑（观景模式，相机可动）。

#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RHI/Components/RHICamera.h"
#include <chrono>
#include "Engine/Graphics/RHI/Utils/IBLPrecomputer.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/Nanite/OfflineSDFMerger.h"
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
#include "Engine/Graphics/DebugDraw/DebugDrawQueue.h"

#include <memory>
#include <vector>
#include <string>

class TestVulkanFroxelRenderGraph;

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};

class TestVulkanFroxelRenderGraph : public primal::test::RenderTestCase {
public:
    TestVulkanFroxelRenderGraph() = default;
    ~TestVulkanFroxelRenderGraph() override;

    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

private:
    bool InitializeDevice();
    bool InitializeWindowAndRenderSystem();
    bool InitializePipeline();
    bool LoadSponzaScene();
    bool InitializeIBL();

    // ---- Froxel 门禁（计划 §6.4）----
    // 相位机：UniformShadow → UniformNoShadow → Noise（→WP1: March0/March4）。
    enum class GatePhase { Off, UniformShadow, UniformNoShadow, UniformShadowM0, UniformMarch4, Noise, Done };
    // gate frame 到点后的相位推进 + 读回断言。frameNo = 已完成渲染的帧号。
    void RunFroxelGate(u64 frameNo);
    // 3D 纹理读回（staging copy + half-float 解码，复用 FROXEL_DUMP 已验证
    // 路径：CopyTextureToBuffer 直拷、独立 cmd buffer、WaitForCompletion）。
    bool ReadGridRGBA16F(primal::graphics::rhi::ResourceHandle tex, u32 w, u32 h, u32 d,
                         std::vector<float>& out);
    bool ReadGridR16F(primal::graphics::rhi::ResourceHandle tex, u32 w, u32 h, u32 d,
                      std::vector<float>& out);
    // 半分辨率 resolve 散射图读回（RGB=scatter, A=transmittance），存 PPM。
    bool CaptureFinalPPM(const char* tag);
    // CPU 侧 froxelToWorld 镜像（与 GLSL FroxelCommon 逐式一致）——G4 的
    // 世界坐标分层判据用。返回 false 表示相机矩阵不可用。
    bool BuildFroxelWorldPos(u32 w, u32 h, u32 d, std::vector<float>& worldXyz);

    void FinishGate();
    static std::vector<float> ColumnMeans(const std::vector<float>& scatter,
                                          u32 w, u32 h, u32 d);
    GatePhase gatePhase_{GatePhase::Off};
    bool gateEnabled_{true};
    u32 gateFrame_{300};          // FROXEL_GATE_FRAME（相位 A 读回帧）
    u32 gateFrameB_{345};         // F + 45：no-shadow 相位读回
    u32 gateFrameC_{390};         // F + 90：G5 A'（uniform+march0）读回
    u32 gateFrameD_{435};         // F + 135：march=4 读回 + G5 断言
    u32 gateFrameNoiseOn_{437};   // F + 137：noise 重建帧
    u32 gateFrameE_{480};         // F + 180：noise 相位密度读回（G4）+ RESULT
    std::vector<float> gateColMeanMarch0_;   // G5 A'（uniform+march0）列均值
    bool gateG5Skipped_{false};
    bool gateDone_{false};
    int  gatePassCount_{0};
    int  gateFailCount_{0};
    // 相位 A（shadows on）逐列 scatter 均值（G3 的 A 侧），及 A 相位密度覆盖。
    std::vector<float> gateColMeanA_;
    u32 gateGW_{0}, gateGH_{0}, gateGD_{0};
    // §5 基线帧时间：uniform 相位稳定窗口（100..F-1）的每帧墙钟均值。
    std::chrono::steady_clock::time_point frameStartTime_;
    double gateFrameTimeAccumMs_{0.0};
    u32 gateFrameTimeFrames_{0};

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

    // Offline SDF merger — loads per-mesh SDF from pipeline model for visualization.
    primal::graphics::nanite::OfflineSDFMerger* offlineSDFMerger_{nullptr};

    primal::graphics::rhi::ResourceHandle fallbackDiffuse_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle fallbackNormal_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle fallbackORM_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::SamplerHandle materialSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};
    primal::graphics::rhi::SamplerHandle texSampler_{primal::graphics::rhi::handles::INVALID_SAMPLER};

    primal::math::m4x4 viewMatrix_{primal::graphics::rhi::math::MatrixIdentity()};
    primal::math::m4x4 projMatrix_{primal::graphics::rhi::math::MatrixIdentity()};

    // IBL resources (Tier 5 visual fidelity) — sunset.hdr → cubes + brdfLUT.
    std::unique_ptr<primal::graphics::rhi::IBLPrecomputer> iblPrecomputer_;
    primal::graphics::rhi::ResourceHandle iblIrradiance_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblPrefilter_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle iblBrdfLUT_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle envCube_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle envCubeArrayView_{primal::graphics::rhi::handles::INVALID_RESOURCE};
    primal::graphics::rhi::ResourceHandle equirectTex_{primal::graphics::rhi::handles::INVALID_RESOURCE};

    // WASD + right-mouse-look camera。门禁模式（默认）相机锁定参考机位；
    // FROXEL_GATE=0 观景模式恢复自由相机。
    primal::graphics::rhi::RHICamera camera_;
    bool cameraInitialized_{false};
    std::chrono::steady_clock::time_point lastFrameTime_;

    static constexpr u32 kMaxSwapchainImages = 3;
    primal::graphics::rhi::SyncHandle renderDoneSemaphores_[kMaxSwapchainImages] = {
        primal::graphics::rhi::handles::INVALID_SYNC,
        primal::graphics::rhi::handles::INVALID_SYNC,
        primal::graphics::rhi::handles::INVALID_SYNC
    };

    u64 frameCount_{0};
    bool subsystemsInitialized_{false};
    bool hasShutdown_{false};
};
