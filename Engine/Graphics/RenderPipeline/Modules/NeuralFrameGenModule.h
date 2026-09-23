/**
 * @file NeuralFrameGenModule.h
 * @brief R2.1 神经帧生成（FG）模块：前帧/当前帧 → shim neural_fg_interpolate → 插值帧
 *
 * 数据流（参照 NeuralDenoiseModule 的 MLX 3 槽延迟回读范式，扩为 4 槽）：
 *   AddPass 头部：上一处理轮回的插值结果 CopyBufferToTexture → fg_result_；
 *   pass 内：当帧 FinalBlit 输入纹理 CopyTextureToBuffer 进回读环（4 槽）；
 *   AddPass 尾部（CPU）：处理 (t-3, t-2) 帧对（≥2 帧裕量免 stall）→
 *   shim interpolate → f16 编码进上传缓冲 → 下帧头部上传。
 * 输出滞后显示 ~3 帧；插值帧经 SRP 的 FinalBlit 输入选择展示
 * （PRIMAL_FRAMEGEN_PRESENT=1 时 blitIn.input_rg 换为本模块输出纹理）。
 *
 * 触发：PRIMAL_FRAMEGEN=1（默认关，零足迹）。权重：
 *   PRIMAL_FRAMEGEN_WEIGHTS（默认 weights/fg/framegen.safetensors）。
 * 相位：PRIMAL_FRAMEGEN_PHASE（默认 0.5）。
 * 调试：PRIMAL_FRAMEGEN_DUMP=<path>（+ _DUMP_FRAME，默认 300）输出插值帧
 *   raw RGBA16F——非全零验证用。
 */
#pragma once

#include <string>
#include <vector>

#include "Graphics/Neural/NeuralFrameGenBackend.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

struct NeuralFrameGenInputs {
    rendergraph::RGResourceHandle blit_input;   ///< 当帧 FinalBlit 的输入（LDR [0,1]）
    u32 render_width = 0;
    u32 render_height = 0;
    u32 current_buffer_index = 0;
    bool input_is_hdr = false;                  ///< 输入是 HDR rgba16f（需 tonemap 解码）
    bool present_interpolated = false;          ///< 本帧 FinalBlit 展示插值帧
};

class NeuralFrameGenModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const NeuralFrameGenInputs& inputs);

    bool IsInitialized() const { return initialized_; }
    /// 插值输出纹理（rgba16f，物理句柄）——SRP 以 ImportTexture 供 FinalBlit 读。
    rhi::ResourceHandle OutputTexture() const { return fg_result_; }
    const rhi::TextureDesc& OutputDesc() const { return output_desc_; }
    /// 至少上传过一帧插值结果（FinalBlit 换源的前置条件）。
    /// 注意用"已上传"而非"已生产"：生产到上传隔一帧，若用生产计数，
    /// FinalBlit 会在首帧上传前消费 UNDEFINED 布局的纹理（Vulkan 校验错）。
    bool PresentReady() const { return frames_uploaded_ > 0; }

private:
    /// 按首次 AddPass 的实际帧分辨率惰性建资源（窗口/渲染目标分辨率错配修复）。
    bool EnsureResources(u32 w, u32 h);
    /// CPU 侧：回读槽解码 → shim interpolate → f16 编码上传缓冲。
    bool ProcessPair(u32 slot_a, u32 slot_b, u32 upload_idx, u32 w, u32 h);
    u32 frame_w_ = 0;
    u32 frame_h_ = 0;
    bool resources_ready_ = false;

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    bool initialized_ = false;

    neural::NeuralFrameGenBackend backend_;
    f32 phase_ = 0.5f;

    // 回读环：4 槽（帧对处理需 2+2 帧裕量）。尺寸按 rgba16f 预算（w*h*8），
    // rgba8 输入只用到前半。
    static constexpr u32 kSlots = 4;
    rhi::ResourceHandle readback_[kSlots]{};
    static constexpr u32 kUploads = 3;   /// 深度 3：present 每帧消费时留 2 帧裕量
    rhi::ResourceHandle upload_[kUploads]{};
    bool upload_pending_[kUploads] = {false, false, false};
    u32 pairs_done_ = 0;          ///< 成功处理的帧对数（producer 计数）
    u32 frames_uploaded_ = 0;     ///< 已上传到 fg_result_ 的插值帧数（consumer 计数）
    u32 upload_idx_for_present_ = 2;   ///< 本帧 pass 头要上传的缓冲（2 = 无）
    u64 tick_ = 0;
    bool last_present_ = false;

    std::vector<f32> frame_a_, frame_b_, frame_out_;
    std::string infer_err_;   // 首次失败只报一次

    rhi::ResourceHandle fg_result_{rhi::handles::INVALID_RESOURCE};
    rhi::TextureDesc output_desc_{};

    // dump（PRIMAL_FRAMEGEN_DUMP）
    std::string dump_path_;
    rhi::ResourceHandle staging_{rhi::handles::INVALID_RESOURCE};
    u64 staging_size_ = 0;
    u32 dump_frame_ = 300;
    bool dump_copied_ = false;
    bool dump_done_ = false;
    u64 dump_map_tick_ = 0;

    // 计时（R2.2 预算证据；每 120 tick 输出一次，decode/infer/encode 拆解）
    f32 last_cpu_ms_ = 0.0f;
    f32 cpu_ms_peak_ = 0.0f;
    f32 decode_ms_ = 0.0f;
    f32 infer_ms_ = 0.0f;
    f32 encode_ms_ = 0.0f;
};

}  // namespace primal::graphics
