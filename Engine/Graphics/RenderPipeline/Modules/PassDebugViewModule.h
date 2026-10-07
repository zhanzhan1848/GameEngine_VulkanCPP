#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/RenderPipeline/PassDebugView.h"
#include "Utilities/MathTypes.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

// ============================================================================
// PassDebugViewModule — 把单个中间 pass 资源整屏画进 BackBuffer（调试视图）
// ============================================================================
// 模式对齐 FinalBlitModule（同一全屏三角 + BackBuffer 渲染目标），但绑定面
// 更宽：float 源纹理 + uint 源纹理（可见性缓冲）+ 深度纹理 + cluster map
// storage buffer + 参数 UBO，一套管线按 UBO.mode 分派解码逻辑。
//
// 着色器自加载（编辑器路径无 SetShaderHandles 引导，对齐 DebugProbeSphereModule）：
//   Metal  — EngineTest/shaders/PassDebugView.metal（源码直编，双入口）
//   Vulkan — Engine/Graphics/Vulkan/shaders/Debug/PassDebugView.vert/.frag.spv
struct PassDebugViewInputs {
    // 源纹理：transient RG 资源走 *_rg（exec 时解析物理句柄，照抄
    // FinalBlit 的 exec-resolve 模式）；持久资源也可走 ImportResource 后
    // 传 *_rg（推荐——RG 会插入正确的读 barrier）。
    rendergraph::RGResourceHandle source_rg;
    rhi::ResourceHandle source_tex{ rhi::handles::INVALID_RESOURCE };
    // uint 源（可见性缓冲 R32_UINT，ID 类视图专用；INVALID 时 ID 视图黑屏）
    rendergraph::RGResourceHandle id_rg;
    rhi::ResourceHandle id_tex{ rhi::handles::INVALID_RESOURCE };
    // cluster map storage buffer（ClusterID/MeshID 反查；binding 4 的正确
    // 数据源——着色器静态引用，必须始终有效且是缓冲句柄）。
    rhi::ResourceHandle cluster_map_buffer{ rhi::handles::INVALID_RESOURCE };
    // [F3 扩展 2026-09-20] sc_shadow 视图：SC 逐 texel 阴影可见性 storage
    // buffer（binding 6；INVALID 时恒绑 fallback 兜底）。aux_count =
    // atlas_size（frag 一维索引 uv.x + uv.y×atlas 用）。
    rhi::ResourceHandle aux_buffer{ rhi::handles::INVALID_RESOURCE };
    u32 aux_count = 0;
    // GBuffer 深度（Depth 视图线性化 + ID 视图背景判定）
    rendergraph::RGResourceHandle depth_rg;
    rhi::ResourceHandle depth_tex{ rhi::handles::INVALID_RESOURCE };
    rendergraph::RGResourceHandle backbuffer_rg;
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
    PassDebugView view = PassDebugView::Off;
    math::m4x4 inv_view_proj{};   // 深度反投影（背景检测）
    math::m4x4 proj{};            // 线性化用
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    u32 cluster_count = 0;        // cluster map 条目数（ClusterID/MeshID 扫描上限）
    // BackBuffer 实际格式（编辑器离屏 RT 是 RGBA16F、drawable 是 BGRA8）。
    // Metal 管线 pixel format 与 attach 格式不一致会静默丢 draw（黑屏）。
    rhi::DataFormat output_format = rhi::DataFormat::BGRA8_UNorm;
};

class PassDebugViewModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();
    void AddPass(rendergraph::RenderGraph& graph, const PassDebugViewInputs& inputs);

    // 直绑路径：在调用方已声明的 render pass 内绑定管线/描述符并绘制
    // （测试壳/外部宿主不经过 AddPass 图声明时使用；源/深度为物理句柄，
    //  cluster map 传 INVALID 时用哑缓冲兜底）。
    void DrawInline(rhi::RHICommandBuffer* cmd, rhi::ResourceHandle src,
                    rhi::ResourceHandle depth, rhi::ResourceHandle idTex,
                    rhi::ResourceHandle clusterMap, PassDebugView view,
                    u32 width, u32 height, f32 near_p, f32 far_p,
                    u32 cluster_count, u32 cbIdx);

    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    bool InitializeSimple(rhi::RHIDeviceBase* device);
    // 按 BackBuffer 格式取/建管线变体（BGRA8 主变体 + RGBA16F 编辑器离屏 RT）。
    rhi::PipelineHandle PipelineForFormat(rhi::DataFormat fmt);
    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::PipelineHandle pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::DataFormat pipeline_format_{ rhi::DataFormat::BGRA8_UNorm };  // pipeline_ 对应格式
    rhi::PipelineHandle pipeline_rgba16f_{ rhi::handles::INVALID_PIPELINE };  // 懒建变体
    rhi::PipelineHandle simple_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle simple_rgba16f_{ rhi::handles::INVALID_PIPELINE };    // 懒建变体
    rhi::ShaderHandle vs_{ rhi::handles::INVALID_SHADER };                    // 变体懒建用
    rhi::ShaderHandle fs_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle simple_fs_{ rhi::handles::INVALID_SHADER };
    rhi::PipelineLayoutHandle layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle param_buffer_[3]{ rhi::handles::INVALID_RESOURCE,
                                          rhi::handles::INVALID_RESOURCE,
                                          rhi::handles::INVALID_RESOURCE };
    // cluster map 缺席时的哑 storage buffer（16B 零填）——着色器静态引用
    // binding 4，必须始终绑定合法缓冲；反查不到时 FS 退化为 meshlet 哈希色。
    rhi::ResourceHandle fallback_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle default_sampler_{ rhi::handles::INVALID_SAMPLER };

    // 二分实验：PRIMAL_PASS_DEBUG_SIMPLE=1 时用极简双绑定管线（FinalBlit
    // 同款声明：SampledImage+Sampler，直接输出 source）隔离多绑定/UBO 因素。
    bool simple_mode_ = false;
    rhi::PipelineLayoutHandle simple_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle simple_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle simple_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
};

} // namespace primal::graphics
