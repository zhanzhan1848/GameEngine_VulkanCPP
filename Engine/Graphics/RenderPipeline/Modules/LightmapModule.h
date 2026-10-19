#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

// Lightmap 旁路消费模块（2026-10-19 计划）。
//
// 隔离铁律：本模块是 FusionIndirect→FusionComposite 之间的独立改写级，
// 不修改任何 Lumen shader 与 DDGI/SPGI/SSGI 资源；场景未注册 lightmap 时
// 零 pass 执行、渲染图与既有路径零差异。激活条件 = 注册了 lightmap 页
// 且 PRIMAL_LIGHTMAP != "0"。
//
// 当前形态（M0 骨架）：RegisterDebugLightmap 注册一张过程生成的渐变页，
// composite 仅 debugMode 全屏替换，用于验证插针/绑定/增益/RG 交接。
// M3 起替换为：LMAP 容器加载 + per-entity scale/bias 注册面 +
// LightmapUV pre-pass 产出的逐像素 UV RT。
class LightmapModule {
public:
    struct FrameInputs {
        u32 frame_index = 0;
        u32 render_width = 0;
        u32 render_height = 0;
        // FusionIndirect 本帧输出的物理句柄（FusionCompositeModule 侧
        // indirect_output_[cbIdx]），composite binding 0 直连。
        rhi::ResourceHandle indirect_tex{rhi::handles::INVALID_RESOURCE};
    };

    // full_screen_vs：与 fusion 共用的全屏三角形 VS（blit_vs_）。
    // shader 自装（ShaderRegistry 解析 + 正确 entry 名——管线 SelfLoad 的
    // Vulkan 分支硬编码 "main"，对本 spv 不适用）。装配失败（如 Metal 侧
    // 尚未移植）返回 false，调用方视为模块不存在。
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle full_screen_vs,
                    u32 render_width, u32 render_height);
    void Shutdown();

    // M0：注册过程生成渐变页（全屏 debug 覆盖）；M3 起由 LMAP 加载面替代。
    void RegisterDebugLightmap();
    void ClearLightmap();
    bool HasActiveLightmap() const { return lightmap_tex_ != rhi::handles::INVALID_RESOURCE; }

    // 由 FusionCompositeModule::AddPasses 在两个 fusion pass 之间调用。
    // 读 FusionIndirect 输出 RG 句柄，追加 "LightmapComposite" pass，
    // 返回改写后间接信号的 RG 句柄（本模块持久输出纹理，同帧写读）。
    rendergraph::RGResourceHandle AddCompositePass(rendergraph::RenderGraph& graph,
                                                   const FrameInputs& inputs,
                                                   rendergraph::RGResourceHandle indirect_rg);

    // Pass 2 绑定切换用的物理句柄（与 AddCompositePass 输出同源）。
    rhi::ResourceHandle GetOutputTexture() const { return output_tex_; }

private:
    rhi::ResourceHandle CreateGradientLightmapTexture(u32 size);

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    rhi::ShaderHandle full_screen_vs_{rhi::handles::INVALID_SHADER};

    rhi::PipelineHandle composite_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle composite_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle composite_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle composite_ds_{rhi::handles::INVALID_DESCRIPTOR_SET};
    rhi::ResourceHandle params_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::SamplerHandle sampler_{rhi::handles::INVALID_SAMPLER};
    rhi::ShaderHandle composite_ps_{rhi::handles::INVALID_SHADER};

    rhi::ResourceHandle lightmap_tex_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle output_tex_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
