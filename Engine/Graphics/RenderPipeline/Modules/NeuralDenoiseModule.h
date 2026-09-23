/**
 * @file NeuralDenoiseModule.h
 * @brief M1 神经降噪推理模块：SPGI 1spp 输出 → conv 网络 → 降噪 GI
 *
 * 推理网 = train_denoiser.py --channels 7 训练的 4 层 3×3 conv 残差网
 * （.pnn v2，7→24→24→16→3，末层 expm1 + log 域残差），离线留出视角
 * PSNR +3.4~3.6 dB。执行结构：
 *   pack pass（GI/depth/normal → 7ch 双纹理）+ 4 次 conv dispatch
 *   （双 bank rgba16f ping-pong，描述符 init 期写死，每帧零更新）。
 *
 * 触发：PRIMAL_NEURAL_DENOISE=1（默认关，零足迹）。权重：
 *   PRIMAL_NEURAL_DENOISE_PNN（默认 EngineTest/assets/Neural/denoiser.pnn）。
 * 调试：PRIMAL_NEURAL_DENOISE_DUMP=<path> 输出降噪结果 raw RGBA16F。
 */
#pragma once

#include <string>
#include <vector>

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/Neural/NeuralMLXBackend.h"   // MLX 后端（可选依赖）
#include "Graphics/Neural/NeuralRuntime.h"      // NeuralWeights 解析复用
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct NeuralDenoiseInputs {
    rendergraph::RGResourceHandle spgi_gi_output;
    rendergraph::RGResourceHandle gbuffer_depth;
    rendergraph::RGResourceHandle gbuffer_normal;
    u32 render_width = 0;
    u32 render_height = 0;
    u32 current_buffer_index = 0;
};

class NeuralDenoiseModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const NeuralDenoiseInputs& inputs);

    bool IsInitialized() const { return conv_pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    bool EnsureStaging();
    /// MLX 路径：映射回读槽 → f16 解包 → shim 推理 → f16 编码回传槽。
    bool MlxProcessSlot(u32 slot, u32 w, u32 h);

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    u32 layer_count_ = 0;

    // MLX 后端（PRIMAL_NEURAL_DENOISE_BACKEND=mlx；仅 Metal 平台，缺库/失败
    // 自动降级 kernel 路径）。数据流：pack → CopyTextureToBuffer 进
    // mlx_readback_ 环（3 槽，2 帧裕量）→ CPU 解包/shim 推理 → mlx_upload_
    // 环 → 下一次该槽 pass 头部 CopyBufferToTexture 进 denoised_（输出
    // 滞后输入 2 帧，对 GI 降噪可接受）。
    bool use_mlx_ = false;
    neural::NeuralMLXBackend mlx_backend_;
    rhi::ResourceHandle mlx_readback_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle mlx_upload_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    bool mlx_pending_[3] = {false, false, false};
    bool mlx_slot_done_[3] = {false, false, false};
    std::vector<f32> mlx_in_;
    std::vector<f32> mlx_out_;
    std::string mlx_infer_err_;   // 首次推理失败只报一次

    // 激活双 bank（A/B ping-pong）+ 专用 pack 输出 P（残差源，不被覆写）
    rhi::ResourceHandle bank_a_[6];
    rhi::ResourceHandle bank_b_[6];
    rhi::ResourceHandle pack_p0_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle pack_p1_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle denoised_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_tex_{rhi::handles::INVALID_RESOURCE};

    rhi::ResourceHandle weights_buf_{rhi::handles::INVALID_RESOURCE};
    u32 weights_float_count_ = 0;
    rhi::ResourceHandle layer_cb_[4];
    rhi::ResourceHandle pack_cb_[3];

    rhi::PipelineHandle pack_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle pack_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle pack_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle pack_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::PipelineHandle conv_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle conv_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle conv_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle conv_sets_[4]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    // dump
    std::string dump_path_;
    rhi::ResourceHandle staging_{rhi::handles::INVALID_RESOURCE};
    u64 staging_size_ = 0;
    u32 tick_ = 0;
    u32 dump_frame_ = 300;
    u32 map_tick_ = 0;
    bool dump_copied_ = false;
    bool dump_done_ = false;
};

}  // namespace primal::graphics
