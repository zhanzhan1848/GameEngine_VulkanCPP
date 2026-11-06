/**
 * @file NeuralDebugModule.cpp
 * @brief M0 神经渲染 debug 链路实现
 * @details 范式来源：管线创建/描述符写入仿 SDFVisualizationModule.cpp，
 *          字节码自加载仿 DebugProbeSphereModule.cpp，dump 仿 GIGatherModule
 *          的 SC_DUMP_CARDS 惰性 staging 模式。
 */
#include "NeuralDebugModule.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "Engine/Common/CoreLogStream.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;
using namespace rendergraph;

namespace {

// ---------------------------------------------------------------------------
// 常量缓冲（与 MLPInferDebug.metal / MLPInfer.comp 的 NeuralDebugCB 逐字节
// 镜像；std140 与 MSL 布局一致：v4 对齐 16B）。
// ---------------------------------------------------------------------------
struct NeuralDebugCB {
    math::v4 res_mode;       //  0: x=outW y=outH z=split(1)/NN-full(0) w=layer_count
    math::v4 enc;            // 16: x=levels y=features z=log2T w=table_count(f32 个数)
    math::v4 level_res[4];   // 32: 每级分辨率（f32 精确整数）
    math::v4 layers[8];      // 96: x=in y=out z=W 偏移(f32 idx) w=B 偏移
};
static_assert(sizeof(NeuralDebugCB) == 224, "NeuralDebugCB layout mismatch");
static_assert(offsetof(NeuralDebugCB, enc) == 16, "NeuralDebugCB enc offset");
static_assert(offsetof(NeuralDebugCB, level_res) == 32, "NeuralDebugCB level_res offset");
static_assert(offsetof(NeuralDebugCB, layers) == 96, "NeuralDebugCB layers offset");

std::vector<u8> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

// SDFViz 同款：相对路径 + "../" 上溯（构建子目录 CWD）+ 仓库绝对路径兜底。
std::vector<u8> LoadWithFallback(const std::string& rel) {
    std::vector<u8> bytes = ReadFileBytes(rel);
    for (int i = 1; i <= 6 && bytes.empty(); ++i) {
        std::string pfx;
        for (int j = 0; j < i; ++j) pfx += "../";
        bytes = ReadFileBytes(pfx + rel);
    }
    if (bytes.empty()) bytes = ReadFileBytes("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + rel);
    return bytes;
}

// DebugProbeSphere 同款：按平台读 MSL 源 / .spv，双候选路径。
// Metal: name = MSL stem（单文件多入口）；Vulkan: name 含扩展名（如 "X.comp.spv"）。
std::vector<u8> LoadNeuralBytecode(rhi::RHIDeviceBase* device, std::string name) {
    const auto platform = device ? device->GetPlatform() : RHIPlatform::Metal;
    if (platform == RHIPlatform::Vulkan) {
        return LoadWithFallback("Engine/Graphics/Vulkan/shaders/Neural/" + name);
    }
    if (platform == RHIPlatform::Metal) {
        // Metal 无扩展名约定 → 补 .metal（调用方传 stem）。
        return LoadWithFallback("Engine/Graphics/Metal/shaders/Neural/" + name + ".metal");
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool NeuralDebugModule::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    if (!device) return false;

    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;

    // ---- 1. 权重资产（PRIMAL_NEURAL_PNN 可覆盖路径） ----
    if (const char* e = std::getenv("PRIMAL_NEURAL_DUMP")) {
        if (*e) dump_path_ = e;
    }
    std::string pnn_err;
    std::string pnn_path;
    if (const char* e = std::getenv("PRIMAL_NEURAL_PNN")) {
        if (*e) pnn_path = e;
    }
    neural::NeuralNetwork net;
    bool loaded = false;
    if (!pnn_path.empty()) {
        loaded = neural::LoadPnnFile(pnn_path, net, &pnn_err);
    } else {
        const std::string candidates[] = {"EngineTest/assets/Neural/test_image.pnn",
                                          "ContentTools/neural/build/test_image.pnn"};
        for (const std::string& cand : candidates) {
            if ((loaded = neural::LoadPnnFile(cand, net, &pnn_err))) {
                pnn_path = cand;
                break;
            }
        }
    }
    if (!loaded) {
        pnn_path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/test_image.pnn";
        loaded = neural::LoadPnnFile(pnn_path, net, &pnn_err);
    }
    if (!loaded) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] .pnn not loadable ("
                  << pnn_err << ") — debug view disabled" << std::endl;
        return false;
    }
    if (!runtime_.Initialize(device, net)) return false;

    // 编码参数：level_res 装包 + log2T（entries 必须是 2 的幂）。
    const u32 levels = (u32)net.level_res.size();
    const u32 table_count = (u32)net.table.size();
    const u32 entries = table_count / (levels * 2u);
    const u32 log2t = (u32)std::log2((double)entries);
    if (entries == 0 || (entries & (entries - 1)) != 0 || (1u << log2t) != entries) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] hash table entries ("
                  << entries << ") not a power of two — disabled" << std::endl;
        return false;
    }

    // ---- 2. 字节码自加载 ----
    auto make_shader = [&](const char* vk_name, const char* mtl_stem,
                           ShaderStage stage, const char* entry) -> ShaderHandle {
        const std::vector<u8> bytes = isVk ? LoadNeuralBytecode(device, vk_name)
                                           : LoadNeuralBytecode(device, mtl_stem);
        if (bytes.empty()) return handles::INVALID_SHADER;
        return device->CreateShader(bytes.data(), bytes.size(), stage, entry);
    };
    ShaderHandle infer_cs = make_shader("MLPInfer.comp.spv", "MLPInferDebug",
                                        ShaderStage::Compute, "neural_mlp_infer");
    ShaderHandle blit_vs = make_shader("NeuralDebugView.vert.spv", "MLPInferDebug",
                                       ShaderStage::Vertex, "neural_debug_vs");
    ShaderHandle blit_ps = make_shader("NeuralDebugView.frag.spv", "MLPInferDebug",
                                       ShaderStage::Pixel, "neural_debug_fs");
    if (infer_cs == handles::INVALID_SHADER || blit_vs == handles::INVALID_SHADER ||
        blit_ps == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] shader bytecode missing — disabled"
                  << std::endl;
        return false;
    }

    // ---- 3. 描述符布局（Vulkan 平面 / Metal 纹理-缓冲重叠命名空间） ----
    if (isVk) {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        infer_set_layout_ = device->CreateDescriptorSetLayout({3, b});
        infer_ssbo_binding_ = 1;
        infer_ubo_binding_ = 2;
    } else {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // texture(0)
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // buffer(0)
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // buffer(1)
        };
        infer_set_layout_ = device->CreateDescriptorSetLayout({3, b});
        infer_ssbo_binding_ = 0;
        infer_ubo_binding_ = 1;
    }
    infer_layout_ = device->CreatePipelineLayout({1, &infer_set_layout_});
    ComputePipelineDesc cpd{};
    cpd.computeShader = infer_cs;
    cpd.layout = infer_layout_;
    cpd.threadGroupSize = {8, 8, 1};
    infer_pipeline_ = device->CreateComputePipeline(cpd);
    if (infer_pipeline_ == handles::INVALID_PIPELINE) return false;

    DescriptorSetLayoutBinding bb[] = {
        {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
    };
    blit_set_layout_ = device->CreateDescriptorSetLayout({1, bb});
    blit_layout_ = device->CreatePipelineLayout({1, &blit_set_layout_});
    GraphicsPipelineDesc gpd{};
    gpd.layout = blit_layout_;
    gpd.vertexShader = blit_vs;
    gpd.pixelShader = blit_ps;
    gpd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    gpd.renderTargetCount = 1;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    gpd.cullMode = CullMode::None;
    blit_pipeline_ = device->CreateGraphicsPipeline(gpd);
    if (blit_pipeline_ == handles::INVALID_PIPELINE) return false;

    for (u32 i = 0; i < 3; ++i) {
        infer_sets_[i] = device->CreateDescriptorSet({infer_set_layout_});
        blit_sets_[i] = device->CreateDescriptorSet({blit_set_layout_});
    }

    // ---- 4. 资源 ----
    layer_count_ = (u32)net.layers.size();
    levels_ = levels;
    log2t_ = log2t;
    table_float_count_ = table_count;

    BufferDesc cbDesc{};
    cbDesc.size = 256;   // 常规 256B 槽位（实际使用 224B）
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    cbDesc.name = "NeuralDebug_CB";
    for (u32 i = 0; i < 3; ++i) cb_[i] = device->CreateBuffer(cbDesc);

    // 常量部分（与帧无关）先填好；res_mode 每帧在 AddPass 里刷新。
    for (u32 i = 0; i < 3; ++i) {
        if (auto* cb = static_cast<NeuralDebugCB*>(device->MapBuffer(cb_[i]))) {
            std::memset(cb, 0, sizeof(NeuralDebugCB));
            cb->enc = {float(levels), 2.0f, float(log2t), float(table_count)};
            for (u32 l = 0; l < levels && l < 16; ++l)
                math::set_component(cb->level_res[l >> 2], l & 3, float(net.level_res[l]));
            for (u32 k = 0; k < net.layers.size() && k < neural::kMaxLayers; ++k) {
                const neural::LayerDesc& d = net.layers[k];
                cb->layers[k] = {float(d.in_dim), float(d.out_dim),
                                 float(table_count + d.w_offset),
                                 float(table_count + d.b_offset)};
            }
            device->UnmapBuffer(cb_[i]);
        }
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralDebug] initialized: pnn=" << pnn_path
              << " layers=" << net.layers.size() << " in=" << net.input_dim
              << " out=" << net.output_dim << " table=" << table_count << std::endl;
    return true;
}

void NeuralDebugModule::Shutdown() {
    if (device_ == nullptr) return;
    runtime_.Shutdown();
    for (u32 i = 0; i < 3; ++i) {
        if (cb_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(cb_[i]);
        cb_[i] = handles::INVALID_RESOURCE;
    }
    if (dump_staging_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(dump_staging_);
    dump_staging_ = handles::INVALID_RESOURCE;
    // pipeline/layout/set 的回收走 RHIGarbageCollector（句柄置空即可）。
    infer_pipeline_ = handles::INVALID_PIPELINE;
    blit_pipeline_ = handles::INVALID_PIPELINE;
    device_ = nullptr;
}

// ---------------------------------------------------------------------------
// AddPass：compute 推理 → blit 上屏（结构仿 SDFVisualizationModule::AddPass）
// ---------------------------------------------------------------------------
void NeuralDebugModule::AddPass(RenderGraph& graph, const NeuralDebugInputs& inputs) {
    if (!IsInitialized()) return;
    const u32 cbIdx = inputs.current_buffer_index % 3;
    ++tick_;
    if (tick_ == 1) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] AddPass #1: dump_path='"
                  << (dump_path_.empty() ? "<off>" : dump_path_) << "'" << std::endl;
    }

    // 刷新每帧部分（split 固定 1 = 左 NN 右 GT；其余常量已在 init 填好）。
    if (auto* cb = static_cast<NeuralDebugCB*>(device_->MapBuffer(cb_[cbIdx]))) {
        cb->res_mode = {float(inputs.render_width ? inputs.render_width : render_width_),
                        float(inputs.render_height ? inputs.render_height : render_height_),
                        1.0f, float(layer_count_)};
        device_->UnmapBuffer(cb_[cbIdx]);
    }

    struct InferData {
        RGResourceHandle outTex;
    };
    const InferData& io = graph.AddPass<InferData>(
        "NeuralDebug_Infer", RGPassType::Compute, RGPassCategory::PostProcess,
        [inputs](InferData& data, RenderGraphBuilder& builder) {
            builder.SideEffect();
            TextureDesc td{};
            td.size = {inputs.render_width ? inputs.render_width : 1,
                       inputs.render_height ? inputs.render_height : 1, 1};
            td.format = DataFormat::RGBA16_Float;
            td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                       TextureUsage::CopySource;
            data.outTex = builder.CreateTexture("NeuralDebug_InferOut", td,
                                                ResourceState::UnorderedAccess);
        },
        [this, inputs, cbIdx](const InferData& data, RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;
            auto* outRes = ctx.graph->GetResource(data.outTex);
            ResourceHandle outTex = outRes ? outRes->GetPhysicalHandle()
                                           : handles::INVALID_RESOURCE;
            if (outTex == handles::INVALID_RESOURCE) return;

            DescriptorImageInfo outInfo;
            outInfo.imageView = outTex;
            outInfo.imageLayout = ResourceState::UnorderedAccess;
            DescriptorBufferInfo ssboInfo;
            ssboInfo.buffer = runtime_.GetParamsBuffer();
            ssboInfo.offset = 0;
            ssboInfo.range = (u64)runtime_.GetTotalFloatCount() * 4ull;
            DescriptorBufferInfo uboInfo;
            uboInfo.buffer = cb_[cbIdx];
            uboInfo.offset = 0;
            uboInfo.range = sizeof(NeuralDebugCB);

            WriteDescriptorSet writes[3] = {};
            writes[0].dstSet = infer_sets_[cbIdx];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::StorageImage;
            writes[0].imageInfo = &outInfo;
            writes[1].dstSet = infer_sets_[cbIdx];
            writes[1].dstBinding = infer_ssbo_binding_;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::StorageBuffer;
            writes[1].bufferInfo = &ssboInfo;
            writes[2].dstSet = infer_sets_[cbIdx];
            writes[2].dstBinding = infer_ubo_binding_;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::UniformBuffer;
            writes[2].bufferInfo = &uboInfo;
            device_->UpdateDescriptorSets(3, writes);

            cmd->BindComputePipeline(infer_pipeline_);
            const DescriptorSetHandle sets[] = {infer_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, infer_layout_,
                                    0, 1, sets, 0, nullptr);
            const u32 w = inputs.render_width ? inputs.render_width : render_width_;
            const u32 h = inputs.render_height ? inputs.render_height : render_height_;
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

            // PRIMAL_NEURAL_DUMP：稳定帧后把推理输出拷入 readback staging，
            // 再过 3 帧回读落盘（GIGather SC_DUMP_CARDS 同款时序）。
            if (!dump_path_.empty() && !dump_done_) {
                if (dump_staging_ == handles::INVALID_RESOURCE && tick_ >= 90) {
                    BufferDesc d{};
                    d.size = (u64)w * h * 8ull;   // RGBA16F
                    d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
                    d.usage = GPUMemoryUsage::Readback;
                    d.memoryUsage = GPUMemoryUsage::Readback;
                    d.name = "NeuralDebug_Dump";
                    dump_staging_ = device_->CreateBuffer(d);
                    dump_staging_size_ = d.size;
                    if (dump_staging_ == handles::INVALID_RESOURCE)
                        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] staging create FAILED"
                                  << std::endl;
                }
                if (dump_staging_ != handles::INVALID_RESOURCE && tick_ >= 93 &&
                    !dump_copied_) {
                    ResourceBarrier b{};
                    b.resource = outTex;
                    b.beforeState = ResourceState::UnorderedAccess;
                    b.afterState = ResourceState::CopySource;
                    b.subresource = 0xFFFFFFFF;
                    b.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                    BufferTextureCopyRegion region{};
                    region.imageSubresource = {0, 0, 1};
                    region.imageExtent = {w, h, 1};
                    cmd->CopyTextureToBuffer(outTex, dump_staging_, &region, 1);
                    ResourceBarrier back{};
                    back.resource = outTex;
                    back.beforeState = ResourceState::CopySource;
                    back.afterState = ResourceState::UnorderedAccess;
                    back.subresource = 0xFFFFFFFF;
                    back.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&back, 1);
                    dump_copied_ = true;
                }
                if (dump_copied_ && tick_ >= 96) {
                    if (auto* texels = static_cast<const u16*>(
                            device_->MapBuffer(dump_staging_))) {
                        std::ofstream out(dump_path_, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(texels),
                                  (std::streamsize)dump_staging_size_);
                        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] dump written: "
                                  << dump_path_ << " (" << dump_staging_size_
                                  << " B, RGBA16F " << w << "x" << h << ")" << std::endl;
                        device_->UnmapBuffer(dump_staging_);
                    } else {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralDebug] staging map FAILED"
                                  << std::endl;
                    }
                    dump_done_ = true;
                }
            }
        });

    // ---- blit 上屏 ----
    struct BlitData {
        RGResourceHandle output;
    };
    graph.AddPass<BlitData>(
        "NeuralDebug_Blit", RGPassType::Graphics, RGPassCategory::PostProcess,
        [inputs, loTex = io.outTex](BlitData& data, RenderGraphBuilder& builder) {
            if (loTex.IsValid()) builder.Read(loTex, ResourceState::ShaderResource);
            data.output = builder.Write(inputs.backbuffer_rg, ResourceState::RenderTarget);
            RGRenderPassDesc rp;
            rp.colors.push_back({.texture = data.output,
                                 .loadOp = LoadAction::DontCare,
                                 .storeOp = StoreAction::Store,
                                 .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rp);
        },
        [this, inputs, loTex = io.outTex, cbIdx](const BlitData&, RenderGraphContext& ctx) {
            if (blit_pipeline_ == handles::INVALID_PIPELINE) return;
            auto cmd = ctx.cmdBuffer;
            ResourceHandle loPhys = handles::INVALID_RESOURCE;
            if (loTex.IsValid()) {
                auto* res = ctx.graph->GetResource(loTex);
                if (res) loPhys = res->GetPhysicalHandle();
            }
            if (loPhys == handles::INVALID_RESOURCE) return;

            // UAV→SRV 手工屏障（compute 用 SideEffect，RG 不自动补）。
            ResourceBarrier b{};
            b.resource = loPhys;
            b.beforeState = ResourceState::UnorderedAccess;
            b.afterState = ResourceState::ShaderResource;
            b.subresource = 0xFFFFFFFF;
            b.queueFamily = 0xFFFFFFFF;
            cmd->InsertBarrier(&b, 1);

            DescriptorImageInfo texInfo;
            texInfo.imageView = loPhys;
            texInfo.imageLayout = ResourceState::ShaderResource;
            WriteDescriptorSet writes[1] = {};
            writes[0].dstSet = blit_sets_[cbIdx];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage;
            writes[0].imageInfo = &texInfo;
            device_->UpdateDescriptorSets(1, writes);

            const u32 w = inputs.render_width ? inputs.render_width : render_width_;
            const u32 h = inputs.render_height ? inputs.render_height : render_height_;
            cmd->SetViewport({{0, 0}, {(float)w, (float)h}, 0, 1});
            cmd->SetScissor({{0, 0}, {w, h}});
            cmd->BindGraphicsPipeline(blit_pipeline_);
            const DescriptorSetHandle sets[] = {blit_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blit_layout_,
                                    0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        });
}

}  // namespace primal::graphics
