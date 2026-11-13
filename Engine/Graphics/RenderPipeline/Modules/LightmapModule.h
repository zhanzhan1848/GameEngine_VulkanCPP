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

// Lightmap 旁路消费模块（2026-10-19 计划；v3 互斥形态 2026-10-20）。
//
// 隔离铁律：本模块不修改任何 Lumen shader 与 DDGI/SPGI/SSGI/SC 资源；
// 场景未注册 lightmap 时零 pass 执行、渲染图与既有路径零差异。激活条件 =
// 加载了 LMAP 页 且 PRIMAL_LIGHTMAP != "0" 且有实体。
//
// v3 互斥（用户定调："开 lightmap ⇒ Lumen GI 完全关闭"）：lightmap 激活时
// StandardRenderPipeline 两个渲染站点以 lmMutex 门控把整个 Lumen GI 区
// （SC/DDGI/WRC/SC-DDGI/SSGI/SSR/SPGI/GIGather/fusion/volume）整体不挂，
// composite 以 deferred 直射输出为底图——lightmap 像素叠加烘焙间接辐照度
// （v3 架构轮 2026-10-21：final = 直射底图 + albedo·E_ind/π·gain，直射/
// 纹理细节走实时链，间接为低频烘焙信号），其余像素 = 直射-only。
// PRIMAL_LIGHTMAP_COEXIST=1 保留旧并存形态作诊断。
//
// 运行时链路：
//   LoadLightmap(path)      —— LMAP 容器 → RGBA16F 页上传 + per-mesh 清单
//                              （2026-11-05：同名条目保留首个 + Warn 告警）
//   UpdateFrame(scene)      —— RenderScene proxy 按收集器同款实例键
//                              （ResolveLightmapInstanceKey：注册表稳定键
//                              "e"+entityGuid，未命中回退
//                              mesh_<contentId>_e<entityId>）匹配清单，
//                              为命中实体构建世界空间顶点/UV/索引缓冲
//   AddUVPrePass(graph, in) —— 静态几何按实体直绘（深度 LessEqual 只读），
//                              输出逐像素 atlas UV RT
//   AddCompositePass(...)   —— 独立消费级（v3 间接辐照度叠加）：UV RT
//                              有效像素 final = base + albedo·E_ind/π·gain
//                              （albedo = GBuffer 逐像素反照率，材质纹理
//                              细节保留；页 A 有效性不足/无 marker 像素纯
//                              透传底图）。与 Lumen 动态 GI 零耦合。
class LightmapModule {
public:
    struct FrameInputs {
        u32 frame_index = 0;
        u32 render_width = 0;
        u32 render_height = 0;
        // 底图物理句柄：v3 互斥态 = deferred 直射输出（fusion 缺席）；
        // 默认并存态 = fusion 最终输出。composite binding 0 直连。
        rhi::ResourceHandle scene_color_tex{rhi::handles::INVALID_RESOURCE};
        // 互斥态置 true：UV pre-pass 之后追加 16B dummy 拷贝 pass（非 render
        // 编码器切换）。MoltenVK tiler 上 render→render 直连时前一 pass 的
        // 附件写入对紧随采样不可见（COLOR→SRO 屏障不足，§6.5 取证）；
        // 并存态 false 保持 v2 图形状零影响。
        bool encoder_flush = false;
        // UV pre-pass 帧输入。
        math::m4x4 view_projection{};
        rendergraph::RGResourceHandle gbuffer_depth_rg{};
        // v3 间接辐照度合成（2026-10-21 架构轮）：GBuffer 反照率（逐像素
        // 真实材质纹理）调制间接项——final = base + albedo·E_ind/π·gain。
        // rg 句柄有效时 composite 声明 Read（屏障）；物理句柄无效时模块
        // 绑 1×1 白占位（albedo=1，间接项不失活）。
        rhi::ResourceHandle gbuffer_albedo_tex{rhi::handles::INVALID_RESOURCE};
        rendergraph::RGResourceHandle gbuffer_albedo_rg{};
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

    // v2（2026-10-20 用户定调）：激活 = env 开 + 页已载 + 有实体。
    // 管线据此整链关闭 Lumen GI（模式级互斥，非逐像素并存）。
    bool IsActive() const;
    // 有真实实体注册（UV pre-pass 需要跑）。
    bool HasEntities() const { return !entities_.empty(); }

    // UV pre-pass：读 GBuffer 深度（只读绑定源由调用方解析），清 UV RT
    // 后按实体直绘。无实体时不加 pass（composite 绑 z=0 占位页）。
    void AddUVPrePass(rendergraph::RenderGraph& graph, const FrameInputs& inputs);

    // 由 StandardRenderPipeline 在 Lumen 区之后调用（v3 互斥：lmMutex 时
    // Lumen 区整体不挂，底图 = deferred 直射输出）。返回替换后场景颜色的
    // RG 句柄；未激活（无页/环境关闭）返回无效句柄。
    rendergraph::RGResourceHandle AddCompositePass(rendergraph::RenderGraph& graph,
                                                   const FrameInputs& inputs,
                                                   rendergraph::RGResourceHandle scene_color_rg);

    // 后处理输入切换用的物理句柄（与 AddCompositePass 输出同源）。
    rhi::ResourceHandle GetOutputTexture() const { return output_tex_; }

    // 测试/工具旁路：显式 scale/bias 直登记（绕开场景匹配；见管线同名注）。
    bool RegisterEntityDirect(id::id_type mesh_content_id, u32 entity_id,
                              const float scale_bias[4]);

private:
    rhi::ResourceHandle CreateGradientLightmapTexture(u32 size);
    // 一次性 uv_rt_ 读回（PRIMAL_LIGHTMAP_UVDUMP；取证：UV pre-pass 写入
    // 是否存活到消费点——有效像素数 + 三个定点采样，线性 UV 值）。
    void DumpUvRT(const FrameInputs& inputs);
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
    // 当前页尺寸（LoadLightmap/RegisterDebugLightmap 记录；DumpUvRT 的
    // PAGEDUMP 纹素坐标换算用——2026-10-21 修 1023 硬编码遗留）。
    u32 page_w_ = 0;
    u32 page_h_ = 0;
    // 页 A 通道 CPU 镜像（§18：UVDUMP=2 的"marker 采样落空区"验收统计用；
    // LoadLightmap 填充，debug 页为空——语义见 LightmapContainer A 通道）。
    std::vector<f32> page_a_;
    rhi::ResourceHandle output_tex_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle uv_rt_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle uv_black_tex_{rhi::handles::INVALID_RESOURCE};  // 1×1 z=0
    // 1×1 白 albedo 占位（gbuffer_albedo_tex 缺失时 binding 4 兜底——
    // albedo=1 使间接项 ≈ 纯 lm 增益，不失活）。
    rhi::ResourceHandle albedo_white_tex_{rhi::handles::INVALID_RESOURCE};
    // §9 取证：本帧 GBuffer 深度物理句柄（AddUVPrePass 记录，DumpUvRT 差分）。
    rhi::ResourceHandle gbuffer_depth_phys_{rhi::handles::INVALID_RESOURCE};
    // 编码器切换 flush 用 16B 哑元对（互斥态 UV pass 后 16B 拷贝）。
    rhi::ResourceHandle flush_src_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle flush_dst_{rhi::handles::INVALID_RESOURCE};
    // 本帧 UV RT 的 RG 身份（AddUVPrePass 导入；composite 声明读以获得
    // 颜色附件→着色读屏障）。
    rendergraph::RGResourceHandle uv_rt_rg_{};

    // 实体 GPU 数据（模块自管，与 GPUDrivenDraw 全局缓冲零耦合）。
    rhi::ResourceHandle positions_sb_{rhi::handles::INVALID_RESOURCE};   // vec4[]
    rhi::ResourceHandle uvs_sb_{rhi::handles::INVALID_RESOURCE};         // vec2[]
    rhi::ResourceHandle indices_ib_{rhi::handles::INVALID_RESOURCE};     // u32[]
    std::vector<EntityRecord> entities_;
    // LMAP 清单（实例键 → scale/bias）。与收集器共用
    // ResolveLightmapInstanceKey 匹配（稳定键优先，回退老命名）。
    std::unordered_map<std::string, std::array<float, 4>> entryScaleBias_;
    std::vector<std::uint64_t> matchedEntities_;  // 已建实体的 (meshId<<32|entityId)
};

} // namespace primal::graphics
