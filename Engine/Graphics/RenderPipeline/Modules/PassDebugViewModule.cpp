#include "PassDebugViewModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Engine/Common/CoreLogStream.h"
#include <fstream>
#include <cstring>
#include <vector>

namespace primal::graphics {

using namespace rhi;

namespace {
// UBO 布局（std140 / Metal constant 逐字段对齐）：
//   mat4 inv_view_proj;  mat4 proj;  vec4 misc;  vec4 misc2;
// misc  = { near_plane, far_plane, view(mode), cluster_count }
// misc2 = { render_width, render_height, 0, 0 }
constexpr u32 kParamBufferSize = sizeof(math::m4x4) * 2 + sizeof(math::v4) * 2;

// Metal: EngineTest/shaders/<name>.metal 源码直编（单文件双入口）。
// Vulkan: Engine/Graphics/Vulkan/shaders/Debug/<name>.<stage>.spv 预编译字节码。
std::vector<u8> LoadPassDebugBytecode(rhi::RHIDeviceBase* device, const char* name) {
    const auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;
    std::vector<std::string> candidates;
    if (platform == rhi::RHIPlatform::Vulkan) {
        std::string file = std::string(name) + ".spv";
        candidates.push_back("Engine/Graphics/Vulkan/shaders/Debug/" + file);
        candidates.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Vulkan/shaders/Debug/" + file);
    } else if (platform == rhi::RHIPlatform::Metal) {
        // MSL 单文件双入口：剥掉 .vert/.frag 后缀。
        std::string stem(name);
        for (const char* suffix : {".vert", ".frag"}) {
            auto pos = stem.rfind(suffix);
            if (pos != std::string::npos && pos + std::strlen(suffix) == stem.size())
                stem.erase(pos);
        }
        std::string file = stem + ".metal";
        candidates.push_back("EngineTest/shaders/" + file);
        candidates.push_back("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/" + file);
    } else {
        return {};
    }

    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) continue;
        if (platform == rhi::RHIPlatform::Metal) return bytes;           // MSL 源
        if (size % 4 != 0) continue;                                      // SPIR-V 4B 对齐
        return bytes;
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[PassDebugView] shader bytecode not found for '"
              << name << "' (tried " << candidates.size() << " paths)" << std::endl;
    return {};
}

struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    SamplerHandle sampler;
    u32 count = 1;
};

void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);
    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;
        const bool isBuffer = (params[i].type == DescriptorType::UniformBuffer ||
                               params[i].type == DescriptorType::StorageBuffer);
        if (isBuffer) {
            // 缓冲描述符走 bufferInfo；UBO 用精确 range（Metal 后端对 ~0
            // 长度的动态 UBO 绑定不可靠——对齐 DebugProbeSphereModule 惯例）。
            const u64 range = (params[i].type == DescriptorType::UniformBuffer)
                                  ? kParamBufferSize : ~0ULL;
            bufferInfos[i] = DescriptorBufferInfo{params[i].resource, 0, range};
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = params[i].sampler;
            imageInfos[i].imageView = handles::INVALID_RESOURCE;
            imageInfos[i].imageLayout = ResourceState::Unknown;
            writes[i].imageInfo = &imageInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            // CombinedImageSampler（binding 5 usampler2D 契约）需要
            // sampler + imageView 一起写入（VisibilityBufferResolve 同款）。
            if (params[i].type == DescriptorType::CombinedImageSampler)
                imageInfos[i].sampler = params[i].sampler;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// 跨编译单元共享的 UBO 数据（AddPass Setup 阶段缓存，exec 写入 param buffer）。
struct alignas(16) PassDebugParamsGpu {
    math::m4x4 inv_view_proj;
    math::m4x4 proj;
    math::v4 misc;   // x=near, y=far, z=view, w=cluster_count
    math::v4 misc2;  // x=width, y=height
};
static_assert(sizeof(PassDebugParamsGpu) == 160, "PassDebugParamsGpu must mirror the shader UBO layout");

PassDebugParamsGpu s_pending_params[3]{};

} // namespace

bool PassDebugViewModule::Initialize(RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;
    const auto platform = device->GetPlatform();

    // 描述符布局（三端一致的 binding 面，Metal 未用 sampler/UBO 面向 Vulkan）：
    //   0 SampledImage      — float 源（颜色/法线/SSAO/HDR 系）
    //   1 Sampler           — Vulkan 组合采样（Metal 忽略，用 constexpr sampler）
    //   2 UniformBuffer     — PassDebugParams
    //   3 SampledImage      — 深度纹理（线性化 + 背景判定）
    //   4 StorageBuffer     — cluster map（ClusterID/MeshID 反查；像素着色器只读）
    //   5 CombinedImageSampler — uint 源（可见性缓冲；GLSL usampler2D 的
    //     SPIR-V 契约是组合图像采样器，对齐 VisibilityBufferResolve 先例）
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},
        {1, DescriptorType::Sampler,        1, ShaderStage::Pixel, nullptr},
        {2, DescriptorType::UniformBuffer,  1, ShaderStage::Pixel, nullptr},
        {3, DescriptorType::SampledImage,   1, ShaderStage::Pixel, nullptr},
        {4, DescriptorType::StorageBuffer,  1, ShaderStage::Pixel, nullptr},
        {5, DescriptorType::CombinedImageSampler, 1, ShaderStage::Pixel, nullptr,
            DescriptorBindingFlags::None, DataFormat::R32_UInt},
        // [F3 扩展 2026-09-20] binding 6 = SC 逐 texel 阴影可见性
        //（sc_shadow 视图；恒绑 fallback 兜底，同 binding 4 惯例）。
        {6, DescriptorType::StorageBuffer,  1, ShaderStage::Pixel, nullptr},
    };
    set_layout_ = device->CreateDescriptorSetLayout({7, bindings});
    if (set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[PassDebugView] CreateDescriptorSetLayout failed" << std::endl;
        return false;
    }
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    for (int i = 0; i < 3; ++i)
        descriptor_sets_[i] = device->CreateDescriptorSet({set_layout_});

    BufferDesc ubDesc{};
    ubDesc.size = kParamBufferSize;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.bindFlags = (u32)BufferUsageFlags::Uniform;
    for (int i = 0; i < 3; ++i) param_buffer_[i] = device->CreateBuffer(ubDesc);

    // 哑 cluster map 兜底（64B 零填 storage）：流式场景 cluster map 可能
    // 暂缺，binding 4 仍需合法绑定；FS 反查不到退化为 meshlet 哈希色。
    // Dynamic（Metal shared storage，创建必成功）——Immutable 在部分驱动
    // 组合下创建失败会让绑定变 INVALID，Metal 丢弃整个 draw 输出黑屏。
    BufferDesc fbDesc{};
    fbDesc.size = 64;
    fbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    fbDesc.bindFlags = (u32)BufferUsageFlags::Storage;
    fallback_buffer_ = device->CreateBuffer(fbDesc);
    if (fallback_buffer_ != handles::INVALID_RESOURCE) {
        if (void* m = device_->MapBuffer(fallback_buffer_)) {
            memset(m, 0, 64);
            device_->UnmapBuffer(fallback_buffer_);
        }
    } else {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[PassDebugView] fallback buffer create failed — ID views may render black" << std::endl;
    }

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Nearest;
    samplerDesc.magFilter = FilterMode::Nearest;
    samplerDesc.mipFilter = FilterMode::Nearest;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    default_sampler_ = device->CreateSampler(samplerDesc);

    // 着色器自加载。Metal 单文件双入口（pass_debug_vs / pass_debug_fs）；
    // Vulkan SPIR-V entry 约定 "main"。
    const bool metal = (platform == rhi::RHIPlatform::Metal);
    const char* vsName = metal ? "PassDebugView.vert" : "PassDebugView.vert";
    auto vsCode = LoadPassDebugBytecode(device, vsName);
    auto fsCode = LoadPassDebugBytecode(device, "PassDebugView.frag");
    if (vsCode.empty() || fsCode.empty()) return false;

    ShaderHandle vs = device->CreateShader(vsCode.data(), vsCode.size(), ShaderStage::Vertex,
                                           metal ? "pass_debug_vs" : "main");
    ShaderHandle fs = device->CreateShader(fsCode.data(), fsCode.size(), ShaderStage::Pixel,
                                           metal ? "pass_debug_fs" : "main");
    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[PassDebugView] CreateShader failed (vs=" << (vs == handles::INVALID_SHADER ? "invalid" : "ok")
            << " fs=" << (fs == handles::INVALID_SHADER ? "invalid" : "ok")
            << " vsBytes=" << vsCode.size() << " fsBytes=" << fsCode.size() << ")" << std::endl;
        return false;
    }

    GraphicsPipelineDesc pd{};
    pd.layout = layout_;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pd.renderTargetCount = 1;
    pd.depthStencilFormat = DataFormat::Unknown;
    pd.enableDepthTest = false;
    pd.enableDepthWrite = false;
    pd.cullMode = CullMode::None;
    pd.vertexAttributes.clear();
    pd.vertexBindings.clear();
    vs_ = vs;   // 变体懒建用（RGBA16F 编辑器离屏 RT）
    fs_ = fs;
    pipeline_ = device->CreateGraphicsPipeline(pd);

    if (pipeline_ == handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[PassDebugView] CreateGraphicsPipeline failed" << std::endl;
    }
    // 二分实验开关（框架问题定位用；不影响生产路径）。
    if (const char* e = std::getenv("PRIMAL_PASS_DEBUG_SIMPLE"); e && e[0] == '1') {
        InitializeSimple(device);
    }
    return pipeline_ != handles::INVALID_PIPELINE;
}

// ============================================================================
// 二分实验：极简管线（FinalBlit 同款双绑定）——PRIMAL_PASS_DEBUG_SIMPLE=1 启用
// ============================================================================
bool PassDebugViewModule::InitializeSimple(RHIDeviceBase* device) {
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {1, DescriptorType::Sampler,      1, ShaderStage::Pixel, nullptr}
    };
    simple_set_layout_ = device->CreateDescriptorSetLayout({2, bindings});
    if (simple_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;
    simple_layout_ = device->CreatePipelineLayout({1, &simple_set_layout_});

    const bool metal = device->GetPlatform() == rhi::RHIPlatform::Metal;
    auto vsCode = LoadPassDebugBytecode(device, "PassDebugView.vert");
    // Metal：主文件第二入口 pass_debug_simple_fs；Vulkan：独立 spv（entry main）。
    auto fsCode = LoadPassDebugBytecode(device, metal ? "PassDebugView" : "PassDebugViewSimple.frag");
    if (vsCode.empty() || fsCode.empty()) return false;
    ShaderHandle vs = device->CreateShader(vsCode.data(), vsCode.size(), ShaderStage::Vertex,
                                           metal ? "pass_debug_vs" : "main");
    ShaderHandle fs = device->CreateShader(fsCode.data(), fsCode.size(), ShaderStage::Pixel,
                                           metal ? "pass_debug_simple_fs" : "main");
    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[PassDebugView] simple shaders failed" << std::endl;
        return false;
    }
    simple_fs_ = fs;
    GraphicsPipelineDesc pd{};
    pd.layout = simple_layout_;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pd.renderTargetCount = 1;
    pd.depthStencilFormat = DataFormat::Unknown;
    pd.enableDepthTest = false;
    pd.enableDepthWrite = false;
    pd.cullMode = CullMode::None;
    pd.vertexAttributes.clear();
    pd.vertexBindings.clear();
    simple_pipeline_ = device->CreateGraphicsPipeline(pd);
    for (int i = 0; i < 3; ++i)
        simple_sets_[i] = device->CreateDescriptorSet({simple_set_layout_});
    simple_mode_ = simple_pipeline_ != handles::INVALID_PIPELINE;
    PRIMAL_LOG_STREAM(Error, "Render")
        << "[PassDebugView] simple_mode pipeline=" << simple_pipeline_ << std::endl;
    return simple_mode_;
}

// 按 BackBuffer 实际格式取/建管线变体。Metal 管线 pixel format 与 render
// pass attach 格式不一致会静默丢 draw（编辑器离屏 RT 是 RGBA16F——
// ForwardSceneRenderer 把 targetDesc.format 一路传入管线创建即为此因）。
rhi::PipelineHandle PassDebugViewModule::PipelineForFormat(DataFormat fmt) {
    if (fmt == DataFormat::RGBA16_Float) {
        if (pipeline_rgba16f_ == handles::INVALID_PIPELINE && vs_ != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = simple_mode_ ? simple_layout_ : layout_;
            pd.vertexShader = vs_;
            pd.pixelShader = simple_mode_ ? simple_fs_ : fs_;
            pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
            pd.renderTargetCount = 1;
            pd.depthStencilFormat = DataFormat::Unknown;
            pd.enableDepthTest = false;
            pd.enableDepthWrite = false;
            pd.cullMode = CullMode::None;
            pd.vertexAttributes.clear();
            pd.vertexBindings.clear();
            pipeline_rgba16f_ = device_->CreateGraphicsPipeline(pd);
            if (pipeline_rgba16f_ == handles::INVALID_PIPELINE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[PassDebugView] RGBA16F pipeline variant failed" << std::endl;
            }
        }
        return pipeline_rgba16f_ != handles::INVALID_PIPELINE ? pipeline_rgba16f_ : pipeline_;
    }
    return pipeline_;
}

void PassDebugViewModule::Shutdown() {
    if (device_ == nullptr) return;
    for (int i = 0; i < 3; ++i) {
        if (param_buffer_[i] != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(param_buffer_[i]);
            param_buffer_[i] = handles::INVALID_RESOURCE;
        }
    }
    if (fallback_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(fallback_buffer_);
        fallback_buffer_ = handles::INVALID_RESOURCE;
    }
    if (default_sampler_ != handles::INVALID_SAMPLER) {
        device_->DestroySampler(default_sampler_);
        default_sampler_ = handles::INVALID_SAMPLER;
    }
    pipeline_ = handles::INVALID_PIPELINE;
}

void PassDebugViewModule::AddPass(rendergraph::RenderGraph& graph, const PassDebugViewInputs& inputs) {
    if (pipeline_ == handles::INVALID_PIPELINE) return;

    struct DebugViewData {
        rendergraph::RGResourceHandle output;
    };

    // Setup 阶段：UBO 按槽缓存（exec 阶段写入 param buffer）。
    {
        PassDebugParamsGpu& p = s_pending_params[inputs.current_buffer_index % 3];
        p.inv_view_proj = inputs.inv_view_proj;
        p.proj = inputs.proj;
        p.misc = math::v4{inputs.near_plane, inputs.far_plane,
                          static_cast<f32>(static_cast<u32>(inputs.view)),
                          static_cast<f32>(inputs.cluster_count)};
        p.misc2 = math::v4{static_cast<f32>(inputs.render_width),
                           static_cast<f32>(inputs.render_height),
                           // [F3-V2 2026-09-20] z = GI HDR 视图（spgi/spgi_raw/
                           // sc_emissive/sc_indirect/sc_tile）显示曝光——
                           // gather/irradiance 类输出（sky 2.0 档下 ~9-12），
                           // ACES 直读全白。env 可调（0.01..1）。
                           [] {
                               // 0.05：sky 2.0 档下 SPGI irradiance 场 ~0.3-12
                               //（露天中庭 6-12），×0.05 后落在 ACES 线性-肩部
                               // 区；0.15 在露天场景仍大面积饱和白。
                               f32 k = 0.05f;
                               if (const char* e = std::getenv("PRIMAL_PASS_DEBUG_GI_SCALE")) {
                                   const f32 v = static_cast<f32>(std::atof(e));
                                   if (v > 0.0f && v <= 1.0f) k = v;
                               }
                               return k;
                           }(),
                           // w = SC atlas 边长（sc_shadow 的 buffer 索引用）。
                           static_cast<f32>(inputs.aux_count)};
    }

    graph.AddPass<DebugViewData>("PassDebugView",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [inputs](DebugViewData& data, rendergraph::RenderGraphBuilder& builder) {
            if (inputs.source_rg.IsValid())
                builder.Read(inputs.source_rg, ResourceState::ShaderResource);
            if (inputs.id_rg.IsValid())
                builder.Read(inputs.id_rg, ResourceState::ShaderResource);
            if (inputs.depth_rg.IsValid())
                builder.Read(inputs.depth_rg, ResourceState::ShaderResource);
            // 写 import 的 BackBuffer 且无下游消费者——不标 SideEffect 会被
            // RG 死代码消除整 pass 裁掉（NeuralDebug/GIGather 同款坑）。
            builder.SideEffect();
            data.output = builder.Write(inputs.backbuffer_rg, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = LoadAction::DontCare,
                .storeOp = StoreAction::Store,
                .clearColor = { math::v4{0, 0, 0, 1} }
            });
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs](const DebugViewData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            const u32 cbIdx = inputs.current_buffer_index % 3;

            // 源物理句柄 exec 时解析（RG 瞬态纹理在 Compile/Allocate 之后才有
            // physical handle —— 照抄 FinalBlitModule 的 exec-resolve 注释契约）。
            ResourceHandle srcTex = inputs.source_tex;
            if (inputs.source_rg.IsValid()) {
                auto* res = context.graph->GetResource(inputs.source_rg);
                if (res) srcTex = res->GetPhysicalHandle();
            }
            ResourceHandle idTexHandle = inputs.id_tex;
            if (inputs.id_rg.IsValid()) {
                auto* res = context.graph->GetResource(inputs.id_rg);
                if (res) idTexHandle = res->GetPhysicalHandle();
            }
            ResourceHandle depthHandle = inputs.depth_tex;
            if (inputs.depth_rg.IsValid()) {
                auto* res = context.graph->GetResource(inputs.depth_rg);
                if (res) depthHandle = res->GetPhysicalHandle();
            }

            // UBO 更新（Map/Unmap 动态缓冲）。
            if (auto* m = device_->MapBuffer(param_buffer_[cbIdx])) {
                memcpy(m, &s_pending_params[cbIdx], sizeof(PassDebugParamsGpu));
                device_->UnmapBuffer(param_buffer_[cbIdx]);
            }

            DescData params[] = {
                {0, DescriptorType::SampledImage,   srcTex,                        handles::INVALID_SAMPLER},
                {1, DescriptorType::Sampler,        handles::INVALID_RESOURCE,     default_sampler_},
                {2, DescriptorType::UniformBuffer,  param_buffer_[cbIdx],          handles::INVALID_SAMPLER},
                {3, DescriptorType::SampledImage,   depthHandle,                   handles::INVALID_SAMPLER},
                {4, DescriptorType::StorageBuffer,  inputs.cluster_map_buffer != handles::INVALID_RESOURCE
                                                        ? inputs.cluster_map_buffer : fallback_buffer_,
                                                                                    handles::INVALID_SAMPLER},
                {5, DescriptorType::CombinedImageSampler, idTexHandle,             default_sampler_},
                {6, DescriptorType::StorageBuffer,  inputs.aux_buffer != handles::INVALID_RESOURCE
                                                        ? inputs.aux_buffer : fallback_buffer_,
                                                                                    handles::INVALID_SAMPLER},
            };
            UpdateDesc(device_, descriptor_sets_[cbIdx], params, 7);

            // 极简模式：只更新 binding0（直出 source），绕开 UBO/SSBO/组合采样。
            if (simple_mode_) {
                DescData sp[] = {
                    {0, DescriptorType::SampledImage, srcTex, handles::INVALID_SAMPLER},
                    {1, DescriptorType::Sampler,      handles::INVALID_RESOURCE, default_sampler_},
                };
                UpdateDesc(device_, simple_sets_[cbIdx], sp, 2);
            }

            cmd->SetViewport({{0, 0}, {static_cast<float>(inputs.render_width), static_cast<float>(inputs.render_height)}, 0, 1});
            cmd->SetScissor({{0, 0}, {inputs.render_width, inputs.render_height}});

            // BackBuffer 实际格式选择管线变体（RGBA16F 离屏 RT vs BGRA8 drawable）。
            const bool useSimple = simple_mode_;
            const rhi::PipelineHandle pipe =
                (inputs.output_format == DataFormat::RGBA16_Float)
                    ? PipelineForFormat(DataFormat::RGBA16_Float)
                    : (useSimple ? simple_pipeline_ : pipeline_);
            if (pipe == handles::INVALID_PIPELINE) return;

            cmd->SetViewport({{0, 0}, {static_cast<float>(inputs.render_width), static_cast<float>(inputs.render_height)}, 0, 1});
            cmd->SetScissor({{0, 0}, {inputs.render_width, inputs.render_height}});

            cmd->BindGraphicsPipeline(pipe);
            const DescriptorSetHandle sets[] = { useSimple ? simple_sets_[cbIdx]
                                                           : descriptor_sets_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                    useSimple ? simple_layout_ : layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );
}

// ============================================================================
// DrawInline — 直绑路径（测试壳/外部宿主在已声明 render pass 内调用）
// ============================================================================
// 绑定面与 AddPass exec 完全一致（含并发扩展的 binding 6 aux 兜底），
// 物理句柄由调用方直接给（无 RG 解析）。
void PassDebugViewModule::DrawInline(rhi::RHICommandBuffer* cmd,
                                     rhi::ResourceHandle src,
                                     rhi::ResourceHandle depth,
                                     rhi::ResourceHandle idTex,
                                     rhi::ResourceHandle clusterMap,
                                     PassDebugView view,
                                     u32 width, u32 height,
                                     f32 near_p, f32 far_p,
                                     u32 cluster_count, u32 cbIdx) {
    if (pipeline_ == handles::INVALID_PIPELINE || cmd == nullptr) return;
    const u32 slot = cbIdx % 3;

    // UBO（布局对齐 SelfLoad 无关的 PassDebugParamsGpu——与 AddPass 共用缓冲组）。
    if (auto* m = device_->MapBuffer(param_buffer_[slot])) {
        PassDebugParamsGpu* p = static_cast<PassDebugParamsGpu*>(m);
        p->misc = math::v4{near_p, far_p, static_cast<f32>(static_cast<u32>(view)),
                           static_cast<f32>(cluster_count)};
        p->misc2 = math::v4{static_cast<f32>(width), static_cast<f32>(height), 0.f, 0.f};
        device_->UnmapBuffer(param_buffer_[slot]);
    }

    ResourceHandle cmap = clusterMap != handles::INVALID_RESOURCE ? clusterMap
                                                                  : fallback_buffer_;
    DescData params[] = {
        {0, DescriptorType::SampledImage,   src,              handles::INVALID_SAMPLER},
        {1, DescriptorType::Sampler,        handles::INVALID_RESOURCE, default_sampler_},
        {2, DescriptorType::UniformBuffer,  param_buffer_[slot], handles::INVALID_SAMPLER},
        {3, DescriptorType::SampledImage,   depth,            handles::INVALID_SAMPLER},
        {4, DescriptorType::StorageBuffer,  cmap,             handles::INVALID_SAMPLER},
        {5, DescriptorType::CombinedImageSampler, idTex,      default_sampler_},
    };
    UpdateDesc(device_, descriptor_sets_[slot], params, 6);

    cmd->SetViewport({{0, 0}, {static_cast<float>(width), static_cast<float>(height)}, 0, 1});
    cmd->SetScissor({{0, 0}, {width, height}});
    cmd->BindGraphicsPipeline(pipeline_);
    const DescriptorSetHandle sets[] = { descriptor_sets_[slot] };
    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout_, 0, 1, sets, 0, nullptr);
    cmd->Draw(3, 0, 1, 0);
}

// ============================================================================
// PassDebugView 名字表（单一事实源，ABI PipelineGetPassDebugViewName 直通）
// ============================================================================
namespace pass_debug_view_names {
static const char* kNames[] = {
    "off", "albedo", "normal", "orm", "depth", "velocity", "visibility",
    "meshlet", "cluster", "mesh", "ssao", "deferred", "gi_indirect", "spgi",
    "sc_atlas", "sc_albedo", "sc_normal", "sc_depth", "spgi_raw",
    "sc_emissive", "sc_shadow", "sc_indirect", "sc_tile", "sc_tile_aux",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<u32>(PassDebugView::Count),
              "name table must match PassDebugView enum");
}

const char* GetPassDebugViewName(u32 view) {
    if (view >= static_cast<u32>(PassDebugView::Count)) return nullptr;
    return pass_debug_view_names::kNames[view];
}

s32 ParsePassDebugViewName(const char* name) {
    if (!name || !name[0]) return -1;
    auto tolower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (u32 i = 0; i < static_cast<u32>(PassDebugView::Count); ++i) {
        const char* n = pass_debug_view_names::kNames[i];
        const char* p = name;
        bool match = true;
        for (; *n && *p; ++n, ++p) {
            if (tolower(*n) != tolower(*p)) { match = false; break; }
        }
        if (match && *n == '\0' && *p == '\0') return static_cast<s32>(i);
    }
    return -1;
}

} // namespace primal::graphics
