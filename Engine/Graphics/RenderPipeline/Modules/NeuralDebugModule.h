/**
 * @file NeuralDebugModule.h
 * @brief M0 神经渲染 debug 链路模块：离线训练的 MLP → GPU 推理 → 上屏
 *
 * 链路证明（Layer C）：PyTorch 离线训练（train_image_fit.py）→ .pnn 权重资产
 * → NeuralRuntime 上传 → compute kernel（hash-encode + MLP 前向）→ blit 上屏。
 * 左半屏 = 网络重建，右半屏 = GT 参考函数直接求值，肉眼即可读出拟合质量。
 *
 * 范式对齐：
 *   - 模块形态仿 GIGatherModule / SDFVisualizationModule
 *     （Initialize / Shutdown / AddPass(RenderGraph)，descriptor 三缓冲轮换）；
 *   - shader 字节码自加载仿 DebugProbeSphereModule（编辑器路径无 SetShaderHandles
 *     引导，模块自己按平台读 MSL 源 / .spv）；
 *   - env 门控仿 SC_INDIRECT：PRIMAL_NEURAL_DEBUG=1 时才创建资源/进 render
 *     graph，未设置时零开销。
 *
 * 对拍：PRIMAL_NEURAL_DUMP=/path 时在稳定帧后把推理输出 raw RGBA16F 落盘，
 * 与 Python 端期望值比对（GPU 数值门禁）。
 */
#pragma once

#include <string>

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/Neural/NeuralRuntime.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

/// AddPass 输入（backbuffer 由调用方 import，仿 SDFVisualizationInputs）。
struct NeuralDebugInputs {
    rendergraph::RGResourceHandle backbuffer_rg;
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
};

class NeuralDebugModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const NeuralDebugInputs& inputs);

    bool IsInitialized() const { return infer_pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    neural::NeuralRuntime runtime_;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    u32 layer_count_ = 0;
    u32 levels_ = 0;
    u32 log2t_ = 0;
    u32 table_float_count_ = 0;

    // compute（hash-encode + MLP 前向 → RGBA16F StorageImage）
    rhi::PipelineHandle infer_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle infer_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle infer_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle infer_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};
    // Vulkan 平面命名空间与 Metal 重叠命名空间的 binding 槽位（init 时确定）。
    u32 infer_ssbo_binding_ = 1;
    u32 infer_ubo_binding_ = 2;

    // blit（RGBA16F → backbuffer，tonemap + gamma + 中缝线）
    rhi::PipelineHandle blit_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle blit_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle blit_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle blit_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};

    // PRIMAL_NEURAL_DUMP 对拍路径（空 = 关闭）。
    std::string dump_path_;
    rhi::ResourceHandle dump_staging_{rhi::handles::INVALID_RESOURCE};
    u64 dump_staging_size_ = 0;
    u32 tick_ = 0;
    bool dump_copied_ = false;
    bool dump_done_ = false;
};

}  // namespace primal::graphics
