/**
 * @file NeuralNRModule.h
 * @brief NR transformer（DLSS-dlssnr）引擎推理模块：SPGI GI → shim → LDR 输出
 *
 * 结构 = NeuralDenoiseModule 的 MLX 路径裁剪（pack → 回读环 → shim → 上传），
 * 无 kernel fallback（NR 无裸 kernel 版本）。差异：
 *   - 间隔门控：NR 前向 100ms+ 量级（附录 B.1），每 PRIMAL_NEURAL_NR_INTERVAL
 *     帧（默认 6）才 pack+回读+推理一次；上传环带代计数，陈旧槽直接跳过
 *     （推理慢于间隔时宁可丢帧也不解码错帧）。
 *   - 输出为 LDR [0,1]（NR 残差语义，shim 内 tonemap+compose），非 log 域 GI。
 *   - 门禁统计（frames_inferred / nonzero_ratio / infer_ms / nan_free）供
 *     TestVulkanDLSSRenderGraph 自动断言。
 *
 * 触发：PRIMAL_NEURAL_NR=1（默认关，零足迹）。包：PRIMAL_NEURAL_NR_PACKAGE
 *   （默认 weights/nr/NeuralRendering.dlssmodel）。调试：PRIMAL_NEURAL_NR_DUMP。
 */
#pragma once

#include <string>
#include <vector>

#include "Graphics/Neural/NeuralNRBackend.h"
#include "Graphics/Neural/NeuralWeights.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct NeuralNRInputs {
    rendergraph::RGResourceHandle spgi_gi_output;
    rendergraph::RGResourceHandle gbuffer_depth;
    rendergraph::RGResourceHandle gbuffer_normal;
    u32 render_width = 0;
    u32 render_height = 0;
    u32 current_buffer_index = 0;
};

/// 自动化门禁快照（TestVulkanDLSSRenderGraph 断言用；纯读）。
struct NeuralNRGateStats {
    u32 frames_inferred = 0;       ///< 成功完成的 shim 推理次数
    u32 frames_uploaded = 0;       ///< 已上传到输出纹理的结果数
    f32 last_nonzero_ratio = 0.0f; ///< 最近一帧输出 |v|>1e-4 的通道占比
    f32 last_infer_ms = 0.0f;      ///< 最近一次 shim 推理耗时
    bool last_nan_free = true;     ///< 最近一帧输出无 NaN
    bool backend_ready = false;    ///< shim/包加载成功
};

class NeuralNRModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const NeuralNRInputs& inputs);

    bool IsInitialized() const { return initialized_; }
    const NeuralNRGateStats& GateStats() const { return stats_; }
    /// 插值输出纹理（rgba16f LDR，物理句柄）——测试/演示用直接读回。
    rhi::ResourceHandle OutputTexture() const { return nr_output_; }

private:
    bool EnsurePackPipeline();
    /// 按首次 AddPass 的实际帧分辨率惰性建资源（窗口/渲染目标分辨率错配修复）。
    bool EnsureResources(u32 w, u32 h);
    u32 frame_w_ = 0;
    u32 frame_h_ = 0;
    bool resources_ready_ = false;
    /// CPU 侧：回读槽 f16 解包（7ch）→ shim 推理 → LDR f16 编码上传缓冲。
    bool ProcessSlot(u32 slot, u32 w, u32 h);
    /// dump 落盘（staging 已由图内拷贝填充）。
    bool WriteDumpFile();

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    bool initialized_ = false;

    neural::NeuralNRBackend backend_;
    u32 interval_ = 6;

    // pack（复用 NeuralDenoisePack 字节码，通道布局一致：7ch = log1p×3 +
    // normal×3 + depth/16）
    rhi::ResourceHandle pack_p0_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle pack_p1_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle pack_cb_[3]{};
    rhi::PipelineHandle pack_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle pack_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle pack_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle pack_sets_[3]{};

    static constexpr u32 kSlots = 3;
    rhi::ResourceHandle readback_[kSlots]{};
    rhi::ResourceHandle upload_{rhi::handles::INVALID_RESOURCE};
    bool upload_pending_ = false;
    u32 interval_index_ = 0;            ///< 已发起 pack+回读的 interval 计数
    u32 slot_generation_[kSlots]{};     ///< 各槽记录的 interval 计数（0=无效）

    std::vector<f32> infer_in_;
    std::vector<f32> infer_out_;
    std::string infer_err_;

    NeuralNRGateStats stats_{};

    rhi::ResourceHandle nr_output_{rhi::handles::INVALID_RESOURCE};
    u64 tick_ = 0;

    // dump（PRIMAL_NEURAL_NR_DUMP）
    std::string dump_path_;
    rhi::ResourceHandle staging_{rhi::handles::INVALID_RESOURCE};
    u64 staging_size_ = 0;
    u32 dump_frame_ = 120;
    bool dump_copied_ = false;
    bool dump_done_ = false;
    u64 dump_map_tick_ = 0;
};

}  // namespace primal::graphics
