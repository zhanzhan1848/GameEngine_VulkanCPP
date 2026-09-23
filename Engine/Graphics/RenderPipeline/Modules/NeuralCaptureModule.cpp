/**
 * @file NeuralCaptureModule.cpp
 * @brief M1 训练数据采集实现
 * @details 范式来源：shader 自加载仿 DebugProbeSphereModule，持久纹理每帧
 *          ImportResource/直连物理句柄仿 Lumen 三缓冲消费，惰性 staging +
 *          延迟回读仿 GIGatherModule SC_DUMP_CARDS，手工 UA→SRV 屏障仿
 *          SDFVisualizationModule。导出走 5 张 rgba32f 过渡纹理逐张
 *          CopyTextureToBuffer 进单个 readback staging。
 */
#include "NeuralCaptureModule.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "Engine/Common/CoreLogStream.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;
using namespace rendergraph;

namespace {

// 与 NeuralCaptureUpdate.comp / NeuralCapturePack.comp / NeuralCapture.metal
// 逐字节镜像（std140 / MSL 对齐一致）。
struct NeuralCaptureCB {
    math::v4 res_frame;   // 0: x=w y=h z=frame_in_take w=frames_total
    math::v4 misc;        // 16: x=near y=far z=take_idx w=reserved
};
static_assert(sizeof(NeuralCaptureCB) == 32, "NeuralCaptureCB layout mismatch");

std::vector<u8> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

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

std::vector<u8> LoadNeuralBytecode(rhi::RHIDeviceBase* device, std::string name) {
    const auto platform = device ? device->GetPlatform() : RHIPlatform::Metal;
    if (platform == RHIPlatform::Vulkan) {
        return LoadWithFallback("Engine/Graphics/Vulkan/shaders/Neural/" + name);
    }
    if (platform == RHIPlatform::Metal) {
        return LoadWithFallback("Engine/Graphics/Metal/shaders/Neural/" + name + ".metal");
    }
    return {};
}

u32 EnvU32(const char* name, u32 def) {
    if (const char* e = std::getenv(name)) {
        const int v = std::atoi(e);
        if (v > 0) return static_cast<u32>(v);
    }
    return def;
}

std::string EnvStr(const char* name, std::string def) {
    if (const char* e = std::getenv(name)) {
        if (*e) return e;
    }
    return def;
}

void InsertStateBarrier(RHICommandBuffer* cmd, ResourceHandle res,
                        ResourceState before, ResourceState after) {
    ResourceBarrier b{};
    b.resource = res;
    b.beforeState = before;
    b.afterState = after;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    cmd->InsertBarrier(&b, 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool NeuralCaptureModule::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    if (!device) return false;

    frames_per_take_ = EnvU32("PRIMAL_NEURAL_CAPTURE_FRAMES", 256);
    total_takes_ = EnvU32("PRIMAL_NEURAL_CAPTURE_TAKES", 1);
    warmup_ = EnvU32("PRIMAL_NEURAL_CAPTURE_WARMUP", 240);
    // 多机位："x,y,z;x,y,z;..."（世界空间偏移，首条约定 0,0,0）。take 数不足
    // 时 clamp 到末条。
    if (const char* e = std::getenv("PRIMAL_NEURAL_CAPTURE_CAM_PATH")) {
        std::vector<math::v3> path;
        std::istringstream ss(e);
        std::string item;
        while (std::getline(ss, item, ';')) {
            float x = 0, y = 0, z = 0;
            if (std::sscanf(item.c_str(), "%f,%f,%f", &x, &y, &z) == 3)
                path.push_back(math::v3{x, y, z});
        }
        if (!path.empty()) {
            cam_path_ = path;
            if (total_takes_ < (u32)path.size()) total_takes_ = (u32)path.size();
        }
    }
    out_dir_ = EnvStr("PRIMAL_NEURAL_CAPTURE_DIR", "/tmp/neural_capture");
    tag_ = EnvStr("PRIMAL_NEURAL_CAPTURE_TAG", "scene");
    // 目录不存在时 ofstream 静默失败（此前"dumped"日志在 open 失败时仍会
    // 打印）——落盘前主动建目录。
    std::filesystem::create_directories(out_dir_);
    if (const char* e = std::getenv("PRIMAL_NEURAL_CAPTURE_NEAR")) z_near_ = static_cast<float>(std::atof(e));
    if (const char* e = std::getenv("PRIMAL_NEURAL_CAPTURE_FAR")) z_far_ = static_cast<float>(std::atof(e));

    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;

    // ---- 字节码（Vulkan 两个独立 .comp.spv entry=main；Metal 单文件双入口）----
    auto make_shader = [&](const char* vk_name, const char* mtl_stem,
                           const char* entry) -> ShaderHandle {
        std::vector<u8> bytes;
        if (isVk) bytes = LoadNeuralBytecode(device, vk_name);
        else bytes = LoadNeuralBytecode(device, mtl_stem);
        if (bytes.empty()) return handles::INVALID_SHADER;
        return device->CreateShader(bytes.data(), bytes.size(), ShaderStage::Compute, entry);
    };
    ShaderHandle update_cs, pack_cs;
    if (isVk) {
        update_cs = make_shader("NeuralCaptureUpdate.comp.spv", "", "main");
        pack_cs = make_shader("NeuralCapturePack.comp.spv", "", "main");
    } else {
        update_cs = make_shader("", "NeuralCapture", "neural_capture_update");
        pack_cs = make_shader("", "NeuralCapture", "neural_capture_pack");
    }
    if (update_cs == handles::INVALID_SHADER || pack_cs == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] shader bytecode missing — disabled"
                  << std::endl;
        return false;
    }

    // ---- 描述符布局（Vulkan 平面 / Metal 纹理-缓冲重叠命名空间）----
    if (isVk) {
        DescriptorSetLayoutBinding ub[] = {
            {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},
        };
        update_set_layout_ = device->CreateDescriptorSetLayout({7, ub});
        DescriptorSetLayoutBinding pb[] = {
            {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {7, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {8, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {9, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},
            {10, DescriptorType::UniformBuffer,    1, ShaderStage::Compute, nullptr},
        };
        pack_set_layout_ = device->CreateDescriptorSetLayout({11, pb});
    } else {
        DescriptorSetLayoutBinding ub[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        update_set_layout_ = device->CreateDescriptorSetLayout({7, ub});
        DescriptorSetLayoutBinding pb[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {7, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {8, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {9, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        pack_set_layout_ = device->CreateDescriptorSetLayout({11, pb});
    }
    update_ubo_binding_ = isVk ? 6 : 0;
    pack_ubo_binding_ = isVk ? 10 : 0;
    update_layout_ = device->CreatePipelineLayout({1, &update_set_layout_});
    pack_layout_ = device->CreatePipelineLayout({1, &pack_set_layout_});
    ComputePipelineDesc upd{};
    upd.computeShader = update_cs;
    upd.layout = update_layout_;
    upd.threadGroupSize = {8, 8, 1};
    update_pipeline_ = device->CreateComputePipeline(upd);
    ComputePipelineDesc pkd{};
    pkd.computeShader = pack_cs;
    pkd.layout = pack_layout_;
    pkd.threadGroupSize = {8, 8, 1};
    pack_pipeline_ = device->CreateComputePipeline(pkd);
    if (update_pipeline_ == handles::INVALID_PIPELINE ||
        pack_pipeline_ == handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] pipeline creation failed" << std::endl;
        return false;
    }
    for (u32 i = 0; i < 3; ++i) {
        update_sets_[i] = device->CreateDescriptorSet({update_set_layout_});
        pack_sets_[i] = device->CreateDescriptorSet({pack_set_layout_});
    }

    // ---- 持久资源 ----
    TextureDesc td{};
    td.size = {render_width_, render_height_, 1};
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    td.format = DataFormat::RGBA16_Float;
    td.name = "NeuralCap_Acc";
    for (u32 s = 0; s < 2; ++s) {
        acc_mean_[s] = device->CreateTexture(td);
        acc_m2_[s] = device->CreateTexture(td);
    }
    noisy_sample_ = device->CreateTexture(td);
    td.format = DataFormat::RGBA32_Float;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
               TextureUsage::CopySource;
    for (u32 p = 0; p < 5; ++p) {
        export_tex_[p] = device->CreateTexture(td);
    }
    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    cbDesc.name = "NeuralCapture_CB";
    for (u32 i = 0; i < 3; ++i) cb_[i] = device->CreateBuffer(cbDesc);

    active_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralCapture] initialized: frames/take="
              << frames_per_take_ << " takes=" << total_takes_ << " dir=" << out_dir_
              << " tag=" << tag_ << " (" << render_width_ << "x" << render_height_ << ")"
              << std::endl;
    return true;
}

void NeuralCaptureModule::Shutdown() {
    if (device_ == nullptr) return;
    for (u32 s = 0; s < 2; ++s) {
        if (acc_mean_[s] != handles::INVALID_RESOURCE) device_->DestroyTexture(acc_mean_[s]);
        if (acc_m2_[s] != handles::INVALID_RESOURCE) device_->DestroyTexture(acc_m2_[s]);
        acc_mean_[s] = acc_m2_[s] = handles::INVALID_RESOURCE;
    }
    if (noisy_sample_ != handles::INVALID_RESOURCE) device_->DestroyTexture(noisy_sample_);
    noisy_sample_ = handles::INVALID_RESOURCE;
    for (u32 p = 0; p < 5; ++p) {
        if (export_tex_[p] != handles::INVALID_RESOURCE) device_->DestroyTexture(export_tex_[p]);
        export_tex_[p] = handles::INVALID_RESOURCE;
    }
    for (u32 p = 0; p < 5; ++p) {
        if (staging_[p] != handles::INVALID_RESOURCE)
            device_->DestroyBuffer(staging_[p]);
        staging_[p] = handles::INVALID_RESOURCE;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (cb_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(cb_[i]);
        cb_[i] = handles::INVALID_RESOURCE;
    }
    update_pipeline_ = handles::INVALID_PIPELINE;
    pack_pipeline_ = handles::INVALID_PIPELINE;
    device_ = nullptr;
}

math::v3 NeuralCaptureModule::GetTakeCameraOffset() const {
    if (cam_path_.empty()) return math::v3{0.0f, 0.0f, 0.0f};
    const u32 i = take_idx_ < (u32)cam_path_.size() ? take_idx_
                                                    : (u32)cam_path_.size() - 1;
    return cam_path_[i];
}

bool NeuralCaptureModule::EnsureStaging() {
    if (staging_[0] != handles::INVALID_RESOURCE) return true;
    staging_plane_bytes_ = static_cast<u64>(render_width_) * render_height_ * 16ull;
    BufferDesc d{};
    d.size = staging_plane_bytes_;
    d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
    d.usage = GPUMemoryUsage::Readback;
    d.memoryUsage = GPUMemoryUsage::Readback;
    d.name = "NeuralCapture_Staging";
    for (u32 p = 0; p < 5; ++p) {
        staging_[p] = device_->CreateBuffer(d);
        if (staging_[p] == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] staging[" << p
                      << "] create FAILED (" << staging_plane_bytes_ << " B)" << std::endl;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// AddPass：update（每帧）→ pack（take 满帧）→ 延迟回读落盘
// ---------------------------------------------------------------------------
void NeuralCaptureModule::AddPass(RenderGraph& graph, const NeuralCaptureInputs& inputs) {
    if (!IsInitialized() || !active_) return;
    ++tick_;
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
    // GI 链暖机期（SC/DDGI/SPGI 时序历史从冷启动收敛）：不采。
    if (tick_ <= warmup_) return;
    const u32 cbIdx = inputs.current_buffer_index % 3;
    if (tick_ == 1 || tick_ == 30) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] AddPass tick=" << tick_
                  << " spgi_rg_valid=" << inputs.spgi_gi_output.IsValid()
                  << " depth_valid=" << inputs.gbuffer_depth.IsValid()
                  << " normal_valid=" << inputs.gbuffer_normal.IsValid() << std::endl;
    }

    // 2026-09-22 修复：dispatch/CB/拷贝一律用模块纹理自己的尺寸。
    // 此前用 inputs.render_width/height（管线 target 逻辑分辨率 1280×720），
    // 而纹理按 render 分辨率（2560×1440）创建——kernel 只覆盖左上 1/4，
    // 其余平面行全零（"noisy 平面全零" 的第二根因）。
    const u32 w = render_width_;
    const u32 h = render_height_;
    const u32 dst = ping_ ^ 1;

    // 相机切镜 → 当前 take 作废重采。
    if (inputs.camera_cut && frame_in_take_ > 0) {
        ++camera_resets_;
        frame_in_take_ = 0;
        ping_ = 0;
        dump_pending_ = false;
        dump_copied_ = false;
        PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralCapture] camera cut — take "
                  << take_idx_ << " reset (#" << camera_resets_ << ")" << std::endl;
    }

    if (auto* cb = static_cast<NeuralCaptureCB*>(device_->MapBuffer(cb_[cbIdx]))) {
        cb->res_frame = math::v4{float(w), float(h), float(frame_in_take_),
                                 float(frames_per_take_)};
        cb->misc = math::v4{z_near_, z_far_, float(take_idx_), 0.0f};
        device_->UnmapBuffer(cb_[cbIdx]);
    }

    // ================================================================
    // Pass 1: Welford 更新（读 ping_ 槽 + 本帧 SPGI 输出，写 dst 槽）
    // ================================================================
    graph.AddPass<int>(
        "NeuralCapture_Update",
        RGPassType::Compute,
        RGPassCategory::PostProcess,
        [](int&, RenderGraphBuilder& builder) { builder.SideEffect(); },
        [this, inputs, cbIdx, w, h, dst, isVk](const int&, RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;
            auto phys = [&](RGResourceHandle rg) -> ResourceHandle {
                auto* r = ctx.graph->GetResource(rg);
                return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
            };
            const ResourceHandle gi = phys(inputs.spgi_gi_output);
            const ResourceHandle depth = phys(inputs.gbuffer_depth);
            const ResourceHandle normal = phys(inputs.gbuffer_normal);
            if (gi == handles::INVALID_RESOURCE || depth == handles::INVALID_RESOURCE ||
                normal == handles::INVALID_RESOURCE) {
                static bool logged_fail = false;
                if (!logged_fail) {
                    logged_fail = true;
                    PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] exec resolve fail: gi="
                              << (gi == handles::INVALID_RESOURCE) << " depth="
                              << (depth == handles::INVALID_RESOURCE) << " normal="
                              << (normal == handles::INVALID_RESOURCE) << std::endl;
                }
                return;
            }
            {
                static bool logged_exec = false;
                if (!logged_exec) {
                    logged_exec = true;
                    PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] update exec OK" << std::endl;
                }
            }
            const u32 ping = ping_;

            // 描述符（0..2 sampled，3..5 storage image，6 UBO）
            DescriptorImageInfo giInfo;   giInfo.imageView = gi;   giInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo meanPrev; meanPrev.imageView = acc_mean_[ping]; meanPrev.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo m2Prev;   m2Prev.imageView = acc_m2_[ping];   m2Prev.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo meanDst;  meanDst.imageView = acc_mean_[dst];  meanDst.imageLayout = ResourceState::UnorderedAccess;
            DescriptorImageInfo m2Dst;    m2Dst.imageView = acc_m2_[dst];    m2Dst.imageLayout = ResourceState::UnorderedAccess;
            DescriptorImageInfo noisyInfo; noisyInfo.imageView = noisy_sample_; noisyInfo.imageLayout = ResourceState::UnorderedAccess;
            DescriptorBufferInfo uboInfo; uboInfo.buffer = cb_[cbIdx]; uboInfo.offset = 0; uboInfo.range = sizeof(NeuralCaptureCB);

            WriteDescriptorSet writes[7] = {};
            writes[0].dstSet = update_sets_[cbIdx]; writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::SampledImage;
            writes[0].imageInfo = &giInfo;
            writes[1].dstSet = update_sets_[cbIdx]; writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::SampledImage;
            writes[1].imageInfo = &meanPrev;
            writes[2].dstSet = update_sets_[cbIdx]; writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::SampledImage;
            writes[2].imageInfo = &m2Prev;
            writes[3].dstSet = update_sets_[cbIdx]; writes[3].dstBinding = 3;
            writes[3].descriptorCount = 1; writes[3].descriptorType = DescriptorType::StorageImage;
            writes[3].imageInfo = &meanDst;
            writes[4].dstSet = update_sets_[cbIdx]; writes[4].dstBinding = 4;
            writes[4].descriptorCount = 1; writes[4].descriptorType = DescriptorType::StorageImage;
            writes[4].imageInfo = &m2Dst;
            writes[5].dstSet = update_sets_[cbIdx]; writes[5].dstBinding = 5;
            writes[5].descriptorCount = 1; writes[5].descriptorType = DescriptorType::StorageImage;
            writes[5].imageInfo = &noisyInfo;
            writes[6].dstSet = update_sets_[cbIdx]; writes[6].dstBinding = update_ubo_binding_;
            writes[6].descriptorCount = 1; writes[6].descriptorType = DescriptorType::UniformBuffer;
            writes[6].bufferInfo = &uboInfo;
            device_->UpdateDescriptorSets(7, writes);

            // prev 统计槽与 SPGI 输出上一阶段都是 UA 写出 → 手工 UA→SRV。
            InsertStateBarrier(cmd, acc_mean_[ping], ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            InsertStateBarrier(cmd, acc_m2_[ping], ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            InsertStateBarrier(cmd, gi, ResourceState::UnorderedAccess, ResourceState::ShaderResource);

            // 2026-09-22 Vulkan 布局契约修复：本帧写目标（dst 槽 + noisy 样本）
            // 先收敛回 GENERAL——Vulkan InsertBarrier 以跟踪状态为 oldLayout，
            // 只需声明 afterState=UA；此后描述符以 UA(GENERAL) 承诺写访问。
            // 缺此屏障时写目标停在 SRO/UNDEFINED，存储写入被吞（VUID-09600）。
            if (isVk) {
                InsertStateBarrier(cmd, acc_mean_[dst], ResourceState::ShaderResource, ResourceState::UnorderedAccess);
                InsertStateBarrier(cmd, acc_m2_[dst], ResourceState::ShaderResource, ResourceState::UnorderedAccess);
                InsertStateBarrier(cmd, noisy_sample_, ResourceState::ShaderResource, ResourceState::UnorderedAccess);
            }

            cmd->BindComputePipeline(update_pipeline_);
            const DescriptorSetHandle sets[] = {update_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, update_layout_, 0, 1, sets, 0, nullptr);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

            // dst 槽恢复 UA（下一帧作为 prev 再降 SRV）。
            InsertStateBarrier(cmd, acc_mean_[dst], ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);

            // 翻转与计数（渲染线程串行执行，安全）。
            ping_ = dst;
            ++frame_in_take_;
        });

    // ================================================================
    // take 满帧 → pack + 落盘
    // ================================================================
    if (!dump_pending_ && frame_in_take_ >= frames_per_take_) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralCapture] take full (" << frame_in_take_
                  << " frames) — packing" << std::endl;
        if (EnsureStaging()) {
            dump_pending_ = true;
            map_tick_ = tick_ + 3;
        } else {
            active_ = false;
            return;
        }
    }

    if (dump_pending_ && !dump_copied_) {
        const u32 fin = ping_;   // 最新写完的槽
        graph.AddPass<int>(
            "NeuralCapture_Pack",
            RGPassType::Compute,
            RGPassCategory::PostProcess,
            [](int&, RenderGraphBuilder& builder) { builder.SideEffect(); },
            [this, inputs, cbIdx, w, h, fin, isVk](const int&, RenderGraphContext& ctx) {
                auto cmd = ctx.cmdBuffer;
                auto phys = [&](RGResourceHandle rg) -> ResourceHandle {
                    auto* r = ctx.graph->GetResource(rg);
                    return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
                };
                const ResourceHandle depth = phys(inputs.gbuffer_depth);
                const ResourceHandle normal = phys(inputs.gbuffer_normal);
                if (depth == handles::INVALID_RESOURCE || normal == handles::INVALID_RESOURCE) {
                    PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] pack resolve fail"
                              << std::endl;
                    return;
                }

                DescriptorImageInfo meanInfo;  meanInfo.imageView = acc_mean_[fin];  meanInfo.imageLayout = ResourceState::ShaderResource;
                DescriptorImageInfo m2Info;    m2Info.imageView = acc_m2_[fin];    m2Info.imageLayout = ResourceState::ShaderResource;
                DescriptorImageInfo noisyInfo; noisyInfo.imageView = noisy_sample_; noisyInfo.imageLayout = ResourceState::ShaderResource;
                DescriptorImageInfo depthInfo; depthInfo.imageView = depth;  depthInfo.imageLayout = ResourceState::ShaderResource;
                DescriptorImageInfo normInfo;  normInfo.imageView = normal;  normInfo.imageLayout = ResourceState::ShaderResource;
                DescriptorImageInfo expInfo[5];
                DescriptorBufferInfo uboInfo; uboInfo.buffer = cb_[cbIdx]; uboInfo.offset = 0; uboInfo.range = sizeof(NeuralCaptureCB);

                WriteDescriptorSet writes[11] = {};
                const DescriptorType sampled = DescriptorType::SampledImage;
                const DescriptorType storage = DescriptorType::StorageImage;
                writes[0] = {}; writes[0].dstSet = pack_sets_[cbIdx]; writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1; writes[0].descriptorType = sampled; writes[0].imageInfo = &meanInfo;
                writes[1] = {}; writes[1].dstSet = pack_sets_[cbIdx]; writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1; writes[1].descriptorType = sampled; writes[1].imageInfo = &m2Info;
                writes[2] = {}; writes[2].dstSet = pack_sets_[cbIdx]; writes[2].dstBinding = 2;
                writes[2].descriptorCount = 1; writes[2].descriptorType = sampled; writes[2].imageInfo = &noisyInfo;
                writes[3] = {}; writes[3].dstSet = pack_sets_[cbIdx]; writes[3].dstBinding = 3;
                writes[3].descriptorCount = 1; writes[3].descriptorType = sampled; writes[3].imageInfo = &depthInfo;
                writes[4] = {}; writes[4].dstSet = pack_sets_[cbIdx]; writes[4].dstBinding = 4;
                writes[4].descriptorCount = 1; writes[4].descriptorType = sampled; writes[4].imageInfo = &normInfo;
                for (u32 p = 0; p < 5; ++p) {
                    expInfo[p].imageView = export_tex_[p];
                    expInfo[p].imageLayout = ResourceState::UnorderedAccess;
                    writes[5 + p] = {};
                    writes[5 + p].dstSet = pack_sets_[cbIdx];
                    writes[5 + p].dstBinding = 5 + p;
                    writes[5 + p].descriptorCount = 1;
                    writes[5 + p].descriptorType = storage;
                    writes[5 + p].imageInfo = &expInfo[p];
                }
                writes[10] = {}; writes[10].dstSet = pack_sets_[cbIdx]; writes[10].dstBinding = pack_ubo_binding_;
                writes[10].descriptorCount = 1; writes[10].descriptorType = DescriptorType::UniformBuffer;
                writes[10].bufferInfo = &uboInfo;
                device_->UpdateDescriptorSets(11, writes);

                InsertStateBarrier(cmd, acc_mean_[fin], ResourceState::UnorderedAccess, ResourceState::ShaderResource);
                InsertStateBarrier(cmd, acc_m2_[fin], ResourceState::UnorderedAccess, ResourceState::ShaderResource);
                InsertStateBarrier(cmd, noisy_sample_, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
                InsertStateBarrier(cmd, depth, ResourceState::ShaderResource, ResourceState::ShaderResource);

                // 2026-09-22 Vulkan 布局契约修复：5 张导出平面是本 pass 的
                // storage image 写目标，首用前必须转到 GENERAL（同 update
                // pass 的注释；缺此转换 → rgba32f 平面写入被吞 → 全零）。
                if (isVk) {
                    for (u32 p = 0; p < 5; ++p) {
                        InsertStateBarrier(cmd, export_tex_[p], ResourceState::ShaderResource,
                                           ResourceState::UnorderedAccess);
                    }
                }

                cmd->BindComputePipeline(pack_pipeline_);
                const DescriptorSetHandle sets[] = {pack_sets_[cbIdx]};
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, pack_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                {
                    static bool logged = false;
                    if (!logged) { logged = true;
                        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] pack dispatched" << std::endl; }
                }

                // 导出平面拷入 staging。Vulkan：纹理全周期 GENERAL，由
                // CopyTextureToBuffer 内部自行转 TRANSFER_SRC/回 SRO——不,
                // 该内部转换会把跟踪状态推向 SRO,故仍显式 UA→CopySource→UA。
                for (u32 p = 0; p < 5; ++p) {
                    InsertStateBarrier(cmd, export_tex_[p], ResourceState::UnorderedAccess, ResourceState::CopySource);
                }
                for (u32 p = 0; p < 5; ++p) {
                    BufferTextureCopyRegion region{};
                    region.bufferOffset = 0;   // 每平面独立 readback buffer
                    region.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                    region.imageExtent = Extent3D{w, h, 1};
                    cmd->CopyTextureToBuffer(export_tex_[p], staging_[p], &region, 1);
                }
                for (u32 p = 0; p < 5; ++p) {
                    InsertStateBarrier(cmd, export_tex_[p], ResourceState::CopySource, ResourceState::UnorderedAccess);
                }
                {
                    static bool logged = false;
                    if (!logged) { logged = true;
                        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] plane copies recorded" << std::endl; }
                }
                dump_copied_ = true;
            });
    }

    // ================================================================
    // 延迟回读落盘（copy 后 +3 帧，渲染线程同步写文件）
    // ================================================================
    if (dump_copied_ && tick_ >= map_tick_) {
        std::ofstream out(out_dir_ + "/take_" + tag_ + "_" + std::to_string(take_idx_) + ".bin",
                          std::ios::binary);
        bool all_mapped = out.is_open();
        for (u32 p = 0; p < 5 && all_mapped; ++p) {
            if (auto* raw = static_cast<const u8*>(device_->MapBuffer(staging_[p]))) {
                out.write(reinterpret_cast<const char*>(raw),
                          static_cast<std::streamsize>(staging_plane_bytes_));
                device_->UnmapBuffer(staging_[p]);
            } else {
                all_mapped = false;
                PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralCapture] staging[" << p
                          << "] map FAILED" << std::endl;
            }
        }

        if (all_mapped) {
            std::ostringstream meta;
            meta << "{\n"
                 << "  \"w\": " << render_width_ << ",\n"
                 << "  \"h\": " << render_height_ << ",\n"
                 << "  \"frames\": " << frames_per_take_ << ",\n"
                 << "  \"take\": " << take_idx_ << ",\n"
                 << "  \"tag\": \"" << tag_ << "\",\n"
                 << "  \"plane_texel_bytes\": 16,\n"
                 << "  \"plane_format\": \"rgba32f\",\n"
                 << "  \"planes\": [\"gt_mean\", \"gt_var\", \"depth_raw\", \"normal\", \"noisy_1spp\"],\n"
                 << "  \"normal_encoding\": \"world_xyz\",\n"
                 << "  \"near\": " << z_near_ << ",\n"
                 << "  \"far\": " << z_far_ << ",\n"
                 << "  \"camera_resets\": " << camera_resets_ << "\n"
                 << "}\n";
            std::ofstream mout(out_dir_ + "/take_" + tag_ + "_" + std::to_string(take_idx_) + ".json");
            mout << meta.str();

            PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralCapture] take " << take_idx_ << " dumped: "
                      << (staging_plane_bytes_ * 5ull) << " B (" << frames_per_take_ << " frames, "
                      << camera_resets_ << " camera resets)" << std::endl;
        }
        ++take_idx_;
        if (take_idx_ >= total_takes_) {
            active_ = false;
            PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralCapture] all " << total_takes_
                      << " take(s) complete — module idle" << std::endl;
        } else {
            frame_in_take_ = 0;
            ping_ = 0;
            dump_pending_ = false;
            dump_copied_ = false;
        }
    }
}

}  // namespace primal::graphics
