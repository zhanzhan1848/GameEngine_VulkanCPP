/**
 * @file NeuralCaptureModule.h
 * @brief M1 神经降噪训练数据采集模块（env 门控，默认零足迹）
 *
 * 数据对的三端语义：
 *   - 噪声输入 = SPGI `ScreenProbeGather` 的输出（`GetOutputTexture()`，
 *     Denoise 之前的全分辨率 RGBA16F）。SPGI 每帧对 probe 摆放 + ray 方向
 *     做 jitter（F5/M7），是该 1spp 估计量的独立样本。
 *   - GT = 固定相机下同一信号的 Welford 收敛（均值 + 方差），累积
 *     `PRIMAL_NEURAL_CAPTURE_FRAMES` 帧。任意单帧 = 训练输入，收敛均值 =
 *     训练目标，方差 = 置信度/损失权重候选。
 *   - 特征 = GBuffer depth（原样导出，线性化在 python 侧）+ world normal
 *     （解码 N*0.5+0.5）。
 *
 * 落盘：pack kernel 把 5 张 rgba32f 平面渲染出来后逐张 CopyTextureToBuffer
 * 进一个 readback staging，+3 帧回读写文件（GIGather SC_DUMP_CARDS 同款
 * 时序）。每 take 两个文件：`take_<tag>_<i>.bin` + `.json`（自描述布局）。
 *
 * 触发（env，全部可选）：
 *   PRIMAL_NEURAL_CAPTURE=1            总开关（默认关，关 = 零资源零开销）
 *   PRIMAL_NEURAL_CAPTURE_FRAMES=128   每 take 累积帧数
 *   PRIMAL_NEURAL_CAPTURE_TAKES=1      take 总数（多 take 扩数据集）
 *   PRIMAL_NEURAL_CAPTURE_DIR=/tmp/neural_capture
 *   PRIMAL_NEURAL_CAPTURE_TAG=sponza   文件名标签
 * 相机切镜（复用 camera_cut_this_frame_）会重置当前 take 重采。
 */
#pragma once

#include <string>

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct NeuralCaptureInputs {
    rendergraph::RGResourceHandle spgi_gi_output;   // SPGI gather 输出（本帧）
    rendergraph::RGResourceHandle gbuffer_depth;    // GBuffer 深度（可采样）
    rendergraph::RGResourceHandle gbuffer_normal;   // GBuffer 法线（N*0.5+0.5）
    u32 render_width = 0;
    u32 render_height = 0;
    u32 current_buffer_index = 0;
    bool camera_cut = false;                        // 本帧发生切镜
};

class NeuralCaptureModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const NeuralCaptureInputs& inputs);

    bool IsInitialized() const { return update_pipeline_ != rhi::handles::INVALID_PIPELINE; }
    bool IsActive() const { return active_; }

    /// 当前 take 的相机偏移（世界空间）。PRIMAL_NEURAL_CAPTURE_CAM_PATH=
    /// "x,y,z;x,y,z;..."（分号分隔，第一条约定为 0,0,0 = 基准机位）；take i
    /// 用第 i 条（超出取末条）。管线在每帧相机矩阵就位后应用该偏移——瞬移
    /// 会触发切镜检测，采集与 SPGI 时序随之自然复位。
    math::v3 GetTakeCameraOffset() const;

private:
    bool EnsureStaging();

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    // env 配置
    u32 frames_per_take_ = 128;
    u32 total_takes_ = 1;
    u32 warmup_ = 240;   // GI 链（SC/DDGI/SPGI 时序）暖机帧数，之后才开始累积
    float z_near_ = 0.1f;
    float z_far_ = 1000.0f;
    std::vector<math::v3> cam_path_;   // 每 take 相机偏移（世界空间）
    std::string out_dir_;
    std::string tag_ = "scene";

    // 双槽 ping-pong 统计纹理 + 首帧噪声样本（rgba16f，UA|SR，持久）
    rhi::ResourceHandle acc_mean_[2]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle acc_m2_[2]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle noisy_sample_{rhi::handles::INVALID_RESOURCE};
    u32 ping_ = 0;   // 读 ping_，写 ping_^1，dispatch 后翻转
    u32 frame_in_take_ = 0;
    u32 take_idx_ = 0;
    u32 camera_resets_ = 0;
    bool active_ = true;

    // pack 导出平面（rgba32f UA|CopySource，持久）
    rhi::ResourceHandle export_tex_[5];
    // staging（readback，一次性 w*h*5*16B）
    // 2026-09-22：单一大 staging 在 Vulkan 上 bufferOffset ≥ ~2×plane 的
    // CopyTextureToBuffer 静默失效（大偏移回读平台问题，见计划 §7.2）——
    // 改为每平面独立 readback buffer（各 w*h*16 B）。
    rhi::ResourceHandle staging_[5]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 staging_plane_bytes_ = 0;
    u32 tick_ = 0;
    u32 map_tick_ = 0;
    bool dump_copied_ = false;
    bool dump_pending_ = false;

    // 管线（update + pack 各一套 descriptor/pipeline）
    rhi::PipelineHandle update_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle update_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle update_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle update_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    // Metal 重叠命名空间下 UBO 的实际槽位（update=0 / pack=0；Vulkan 6 / 10）。
    u32 update_ubo_binding_ = 6;
    u32 pack_ubo_binding_ = 10;

    rhi::PipelineHandle pack_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle pack_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle pack_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle pack_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
};

}  // namespace primal::graphics
