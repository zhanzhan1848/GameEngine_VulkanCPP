#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "Common/Id.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }
class RenderScene;

// Lightmap 旁路消费模块（2026-10-19 计划，M3 形态）。
//
// 隔离铁律：本模块是 FusionIndirect→FusionComposite 之间的独立改写级，
// 不修改任何 Lumen shader 与 DDGI/SPGI/SSGI 资源；场景未注册 lightmap 时
// 零 pass 执行、渲染图与既有路径零差异。激活条件 = 加载了 LMAP 页
// 且 PRIMAL_LIGHTMAP != "0"。
//
// 运行时链路：
//   LoadLightmap(path)      —— LMAP 容器 → RGBA16F 页上传 + per-mesh 清单
//   UpdateFrame(scene)      —— RenderScene proxy 按收集器命名约定
//                              （mesh_<contentId>_e<entityId>）匹配清单，
//                              为命中实体构建世界空间顶点/UV/索引缓冲
//   AddUVPrePass(graph, in) —— 静态几何按实体直绘（深度 LessEqual 只读），
//                              输出逐像素 atlas UV RT
//   AddCompositePass(...)   —— fusion 插针：UV RT 有效像素以烘焙页替换
//                              间接信号，无效像素透传
class LightmapModule {
public:
    struct FrameInputs {
        u32 frame_index = 0;
        u32 render_width = 0;
        u32 render_height = 0;
        // FusionIndirect 本帧输出的物理句柄（FusionCompositeModule 侧
        // indirect_output_[cbIdx]），composite binding 0 直连。
        rhi::ResourceHandle indirect_tex{rhi::handles::INVALID_RESOURCE};
        // UV pre-pass 帧输入。
        math::m4x4 view_projection{};
        rendergraph::RGResourceHandle gbuffer_depth_rg{};
    };

    // full_screen_vs：与 fusion 共用的全屏三角形 VS（blit_vs_）。
    // shader 自装（ShaderRegistry 解析 + 正确 entry 名）。装配失败（如
    // Metal 侧尚未移植）返回 false，调用方视为模块不存在。
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle full_screen_vs,
                    u32 render_width, u32 render_height);
    void Shutdown();

    // M0 骨架入口：注册过程生成的渐变页（全屏 debug 覆盖）；M4 起 Editor
    // 走 LoadLightmap 正式链路。模块未初始化时安全空操作。
    void RegisterDebugLightmap();
    void ClearLightmap();

    // M3 正式链路：加载 LMAP 容器（页纹理上传 + 清单）。失败返回 false
    // 并保持原状。加载后与 UpdateFrame 配合逐步点亮实体。
    bool LoadLightmap(const std::string& path);

    // 每帧（AddUVPrePass 前）调用：按 RenderScene proxy 匹配清单、
    // 增量登记实体并重建 GPU 缓冲。清单为空时安全空操作。
    void UpdateFrame(const RenderScene& scene);

    bool HasActiveLightmap() const { return lightmap_tex_ != rhi::handles::INVALID_RESOURCE; }
    // 有真实实体注册（UV pre-pass 需要跑）。
    bool HasEntities() const { return !entities_.empty(); }

    // UV pre-pass：读 GBuffer 深度（只读绑定源由调用方解析），清 UV RT
    // 后按实体直绘。无实体时不加 pass（composite 绑 z=0 占位页）。
    void AddUVPrePass(rendergraph::RenderGraph& graph, const FrameInputs& inputs);

    // 由 FusionCompositeModule::AddPasses 在两个 fusion pass 之间调用。
    // 返回改写后间接信号的 RG 句柄；未激活（无页/环境关闭）返回无效句柄。
    rendergraph::RGResourceHandle AddCompositePass(rendergraph::RenderGraph& graph,
                                                   const FrameInputs& inputs,
                                                   rendergraph::RGResourceHandle indirect_rg);

    // Pass 2 绑定切换用的物理句柄（与 AddCompositePass 输出同源）。
    rhi::ResourceHandle GetOutputTexture() const { return output_tex_; }

    // 测试/工具旁路：显式 scale/bias 直登记（绕开场景匹配；见管线同名注）。
    bool RegisterEntityDirect(id::id_type mesh_content_id, u32 entity_id,
                              const float scale_bias[4]);

private:
    rhi::ResourceHandle CreateGradientLightmapTexture(u32 size);
    struct EntitySpec {
        id::id_type mesh = 0;   // hierarchy id（两步解析）
        u32 entity = 0;
        float scale_bias[4] = {1, 1, 0, 0};
        math::m4x4 transform{};  // 世界变换（直登记 = 恒等）
    };
    void RebuildEntityBuffers(const std::vector<EntitySpec>& specs);
    std::vector<EntitySpec> directEntities_;  // 旁路登记（长期存在）

    struct EntityRecord {
        std::uint32_t index_count = 0;
        std::uint32_t first_index = 0;      // combined 索引缓冲内偏移（元素）
        std::uint32_t position_offset = 0;  // vec4 单位
        std::uint32_t uv_offset = 0;        // vec2 单位
        float scale_bias[4] = {1, 1, 0, 0};
    };

    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    rhi::ShaderHandle full_screen_vs_{rhi::handles::INVALID_SHADER};

    rhi::PipelineHandle composite_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle composite_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle composite_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle composite_ds_[3]{rhi::handles::INVALID_DESCRIPTOR_SET,
                                          rhi::handles::INVALID_DESCRIPTOR_SET,
                                          rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::PipelineHandle uv_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle uv_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle uv_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle uv_ds_{rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::SamplerHandle sampler_{rhi::handles::INVALID_SAMPLER};
    rhi::ShaderHandle composite_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle uv_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle uv_fs_{rhi::handles::INVALID_SHADER};

    rhi::ResourceHandle lightmap_tex_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle output_tex_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle uv_rt_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle uv_black_tex_{rhi::handles::INVALID_RESOURCE};  // 1×1 z=0
    // 本帧 UV RT 的 RG 身份（AddUVPrePass 导入；composite 声明读以获得
    // 颜色附件→着色读屏障）。
    rendergraph::RGResourceHandle uv_rt_rg_{};

    // 实体 GPU 数据（模块自管，与 GPUDrivenDraw 全局缓冲零耦合）。
    rhi::ResourceHandle positions_sb_{rhi::handles::INVALID_RESOURCE};   // vec4[]
    rhi::ResourceHandle uvs_sb_{rhi::handles::INVALID_RESOURCE};         // vec2[]
    rhi::ResourceHandle indices_ib_{rhi::handles::INVALID_RESOURCE};     // u32[]
    std::vector<EntityRecord> entities_;
    // LMAP 清单（mesh 名 → scale/bias）。收集器命名约定匹配。
    std::unordered_map<std::string, std::array<float, 4>> entryScaleBias_;
    std::vector<std::uint64_t> matchedEntities_;  // 已建实体的 (meshId<<32|entityId)
};

} // namespace primal::graphics
