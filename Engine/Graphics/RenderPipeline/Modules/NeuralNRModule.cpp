/**
 * @file NeuralNRModule.cpp
 * @brief NR 引擎推理实现（pack 复用 NeuralDenoisePack；间隔门控 + shim 推理）
 */
#include "NeuralNRModule.h"

#include <algorithm>
#include <chrono>
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

// 与 NeuralDenoisePack.comp/.metal 的 CB 逐字节镜像（复用其字节码）。
struct NeuralPackCB {
    math::v4 res_misc;   // x=w y=h z=depth_norm_inv w=unused
};
static_assert(sizeof(NeuralPackCB) == 16, "NeuralPackCB layout mismatch");

constexpr float kDepthNorm = 1.0f / 16.0f;

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

// f16 编解码（与 NeuralDenoiseModule 同实现；就近偶数舍入）。
u16 F32ToF16(f32 v) {
    u32 x;
    std::memcpy(&x, &v, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    u32 exp = (x >> 23) & 0xFFu;
    u32 man = x & 0x7FFFFFu;
    if (exp == 0xFF) {
        const u16 nan = man ? 0x7E00u : 0x7C00u;
        return static_cast<u16>(sign | nan);
    }
    int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 0x1F) return static_cast<u16>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<u16>(sign);
        man = (man | 0x800000u) >> (1 - e);
        return static_cast<u16>(sign + ((man + 0x1000) >> 13));
    }
    u32 h = sign | (static_cast<u32>(e) << 10) + ((man + 0x1000) >> 13);
    return static_cast<u16>(h);
}

f32 F16ToF32(u16 h) {
    const u32 sign = (h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 man = h & 0x3FFu;
    u32 out;
    if (exp == 0) {
        if (man == 0) {
            out = sign;
        } else {
            int e = -1;
            u32 m = man;
            do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
            m &= 0x3FFu;
            out = sign | (static_cast<u32>(127 - 15 + e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (man << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    f32 v;
    std::memcpy(&v, &out, 4);
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool NeuralNRModule::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    if (!device) return false;

    if (const char* e = std::getenv("PRIMAL_NEURAL_NR_INTERVAL")) {
        const int v = std::atoi(e);
        if (v >= 1) interval_ = static_cast<u32>(v);
    }
    if (const char* e = std::getenv("PRIMAL_NEURAL_NR_DUMP")) {
        if (*e) dump_path_ = e;
    }
    dump_frame_ = 120;
    if (const char* e = std::getenv("PRIMAL_NEURAL_NR_DUMP_FRAME")) {
        const int v = std::atoi(e);
        if (v > 0) dump_frame_ = static_cast<u32>(v);
    }

    std::string err;
    if (!backend_.Initialize({}, {}, &err)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralNR] backend init failed ("
                  << err << ")——模块停用" << std::endl;
        return false;
    }
    stats_.backend_ready = true;

    if (!EnsurePackPipeline()) return false;
    // 资源（pack 纹理/回读环/上传/staging/CB/推理缓冲）延迟到首次 AddPass
    // 按"实际帧分辨率"创建——Initialize 分辨率是窗口尺寸（HiDPI 2560×1440），
    // 逐帧 blit 输入是渲染目标分辨率（1280×720），错配会让内容只占纹理
    // 左上角四分之一（2026-09-23 截图实证，与 NeuralFrameGenModule 同修）。

    initialized_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralNR] initialized (resources lazy) interval=" << interval_
              << " package=" << backend_.LoadedPackagePath() << std::endl;
    return true;
}

bool NeuralNRModule::EnsurePackPipeline() {
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
    std::vector<u8> bytes;
    if (isVk) bytes = LoadWithFallback("Engine/Graphics/Vulkan/shaders/Neural/NeuralDenoisePack.comp.spv");
    else bytes = LoadWithFallback("Engine/Graphics/Metal/shaders/Neural/NeuralDenoise.metal");
    if (bytes.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralNR] pack shader missing" << std::endl;
        return false;
    }
    ShaderHandle pack_cs;
    if (isVk) pack_cs = device_->CreateShader(bytes.data(), bytes.size(), ShaderStage::Compute, "main");
    else pack_cs = device_->CreateShader(bytes.data(), bytes.size(), ShaderStage::Compute, "neural_denoise_pack");
    if (pack_cs == handles::INVALID_SHADER) return false;

    if (isVk) {
        DescriptorSetLayoutBinding pb[6] = {};
        pb[0] = {0, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr};
        pb[1] = {1, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr};
        pb[2] = {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr};
        pb[3] = {3, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr};
        pb[4] = {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr};
        pb[5] = {5, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr};
        // 注意 gi 在 GLSL 是 binding 0（SampledImage）、depth binding 1（同降噪模块坑）。
        pack_set_layout_ = device_->CreateDescriptorSetLayout({6, pb});
    } else {
        DescriptorSetLayoutBinding pb[6] = {};
        pb[0] = {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // gi
        pb[1] = {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // depth
        pb[2] = {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // normal
        pb[3] = {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr};  // P0
        pb[4] = {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr};  // P1
        pb[5] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};
        pack_set_layout_ = device_->CreateDescriptorSetLayout({6, pb});
    }
    pack_layout_ = device_->CreatePipelineLayout({1, &pack_set_layout_});
    ComputePipelineDesc ppd{};
    ppd.computeShader = pack_cs;
    ppd.layout = pack_layout_;
    ppd.threadGroupSize = {8, 8, 1};
    pack_pipeline_ = device_->CreateComputePipeline(ppd);
    if (pack_pipeline_ == handles::INVALID_PIPELINE) return false;
    for (u32 i = 0; i < 3; ++i) {
        pack_sets_[i] = device_->CreateDescriptorSet({pack_set_layout_});
    }
    return true;
}

void NeuralNRModule::Shutdown() {
    if (device_ == nullptr) return;
    if (pack_p0_ != handles::INVALID_RESOURCE) device_->DestroyTexture(pack_p0_);
    if (pack_p1_ != handles::INVALID_RESOURCE) device_->DestroyTexture(pack_p1_);
    if (nr_output_ != handles::INVALID_RESOURCE) device_->DestroyTexture(nr_output_);
    pack_p0_ = pack_p1_ = nr_output_ = handles::INVALID_RESOURCE;
    if (staging_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(staging_);
    staging_ = handles::INVALID_RESOURCE;
    if (upload_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(upload_);
    upload_ = handles::INVALID_RESOURCE;
    upload_pending_ = false;
    for (u32 i = 0; i < kSlots; ++i) {
        if (readback_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(readback_[i]);
        readback_[i] = handles::INVALID_RESOURCE;
        slot_generation_[i] = 0;
    }
    for (u32 i = 0; i < 3; ++i) {
        if (pack_cb_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(pack_cb_[i]);
        pack_cb_[i] = handles::INVALID_RESOURCE;
    }
    backend_.Shutdown();
    initialized_ = false;
    device_ = nullptr;
}

// ---------------------------------------------------------------------------
// AddPass
// ---------------------------------------------------------------------------
bool NeuralNRModule::EnsureResources(u32 w, u32 h) {
    if (resources_ready_) return true;
    frame_w_ = w;
    frame_h_ = h;

    TextureDesc td{};
    td.size = {w, h, 1};
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
               TextureUsage::CopySource;
    td.format = DataFormat::RGBA16_Float;
    td.name = "NeuralNR_Pack";
    pack_p0_ = device_->CreateTexture(td);
    pack_p1_ = device_->CreateTexture(td);
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
               TextureUsage::CopyDest | TextureUsage::CopySource;
    td.name = "NeuralNR_Out";
    nr_output_ = device_->CreateTexture(td);
    if (pack_p0_ == handles::INVALID_RESOURCE || pack_p1_ == handles::INVALID_RESOURCE ||
        nr_output_ == handles::INVALID_RESOURCE) {
        return false;
    }

    const u64 tex_bytes = static_cast<u64>(w) * h * 8ull;
    for (u32 i = 0; i < kSlots; ++i) {
        BufferDesc rb{};
        rb.size = tex_bytes * 2ull;
        rb.usage = GPUMemoryUsage::Readback;
        rb.memoryUsage = GPUMemoryUsage::Readback;
        rb.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        rb.name = "NeuralNR_Readback";
        readback_[i] = device_->CreateBuffer(rb);
        if (readback_[i] == handles::INVALID_RESOURCE) return false;
    }
    BufferDesc up{};
    up.size = tex_bytes;
    up.usage = GPUMemoryUsage::Dynamic;
    up.memoryUsage = GPUMemoryUsage::Dynamic;
    up.bindFlags = static_cast<u32>(BufferUsageFlags::TransferSrc);
    up.name = "NeuralNR_Upload";
    upload_ = device_->CreateBuffer(up);
    if (upload_ == handles::INVALID_RESOURCE) return false;

    if (!dump_path_.empty()) {
        staging_size_ = tex_bytes;
        BufferDesc d{};
        d.size = staging_size_;
        d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        d.usage = GPUMemoryUsage::Readback;
        d.memoryUsage = GPUMemoryUsage::Readback;
        d.name = "NeuralNR_Staging";
        staging_ = device_->CreateBuffer(d);
    }

    for (u32 i = 0; i < 3; ++i) {
        BufferDesc cbDesc{};
        cbDesc.size = 256;
        cbDesc.type = BufferType::Constant;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        cbDesc.usage = GPUMemoryUsage::Dynamic;
        cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
        cbDesc.name = "NeuralNR_CB";
        pack_cb_[i] = device_->CreateBuffer(cbDesc);
        if (auto* cb = static_cast<NeuralPackCB*>(device_->MapBuffer(pack_cb_[i]))) {
            cb->res_misc = math::v4{float(w), float(h), kDepthNorm, 0.0f};
            device_->UnmapBuffer(pack_cb_[i]);
        }
    }

    infer_in_.resize(static_cast<size_t>(7) * w * h);
    infer_out_.resize(static_cast<size_t>(3) * w * h);
    resources_ready_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralNR] resources ready: "
              << w << "x" << h << std::endl;
    return true;
}

void NeuralNRModule::AddPass(RenderGraph& graph, const NeuralNRInputs& inputs) {
    if (!initialized_) return;
    ++tick_;
    const u32 w = inputs.render_width ? inputs.render_width : render_width_;
    const u32 h = inputs.render_height ? inputs.render_height : render_height_;
    if (!EnsureResources(w, h)) return;
    const bool this_interval = (tick_ % interval_) == 0;

    auto resolvePhys = [&graph](RGResourceHandle handle) -> ResourceHandle {
        auto* r = graph.GetResource(handle);
        return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
    };
    const ResourceHandle gi = resolvePhys(inputs.spgi_gi_output);
    const ResourceHandle depth = resolvePhys(inputs.gbuffer_depth);
    const ResourceHandle normal = resolvePhys(inputs.gbuffer_normal);

    graph.AddPass<int>(
        "NeuralNR",
        RGPassType::Compute,
        RGPassCategory::Lighting,
        [&](int&, RenderGraphBuilder& builder) {
            builder.SideEffect();
            if (this_interval) {
                if (gi != handles::INVALID_RESOURCE)
                    builder.Read(inputs.spgi_gi_output, ResourceState::ShaderResource);
                if (depth != handles::INVALID_RESOURCE)
                    builder.Read(inputs.gbuffer_depth, ResourceState::ShaderResource);
                if (normal != handles::INVALID_RESOURCE)
                    builder.Read(inputs.gbuffer_normal, ResourceState::ShaderResource);
            }
        },
        [this, w, h, gi, depth, normal, this_interval, cbIdx = inputs.current_buffer_index % 3,
         interval_index = interval_index_](const int&, RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;

            // ---- 上传：上一 interval 的推理结果 → nr_output_ ----
            if (upload_pending_) {
                InsertStateBarrier(cmd, nr_output_, ResourceState::ShaderResource,
                                   ResourceState::CopyDest);
                BufferTextureCopyRegion up{};
                up.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                up.imageExtent = Extent3D{w, h, 1};
                cmd->CopyBufferToTexture(upload_, nr_output_, &up, 1);
                InsertStateBarrier(cmd, nr_output_, ResourceState::CopyDest,
                                   ResourceState::ShaderResource);
                upload_pending_ = false;
                ++stats_.frames_uploaded;
            }

            if (!this_interval) return;
            if (gi == handles::INVALID_RESOURCE || depth == handles::INVALID_RESOURCE ||
                normal == handles::INVALID_RESOURCE) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralNR] resolve fail" << std::endl;
                }
                return;
            }

            // ---- pack（复用降噪 pack：7ch = log1p×3 + normal×3 + depth/16）----
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            DescriptorImageInfo giInfo; giInfo.imageView = gi; giInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo dInfo; dInfo.imageView = depth; dInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo nInfo; nInfo.imageView = normal; nInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo p0Info; p0Info.imageView = pack_p0_; p0Info.imageLayout = ResourceState::UnorderedAccess;
            DescriptorImageInfo p1Info; p1Info.imageView = pack_p1_; p1Info.imageLayout = ResourceState::UnorderedAccess;
            DescriptorBufferInfo pcInfo; pcInfo.buffer = pack_cb_[cbIdx]; pcInfo.offset = 0;
            pcInfo.range = sizeof(NeuralPackCB);
            WriteDescriptorSet pw[6];
            const DescriptorType sampled = DescriptorType::SampledImage;
            pw[0].dstSet = pack_sets_[cbIdx]; pw[0].dstBinding = 0;
            pw[0].descriptorCount = 1; pw[0].descriptorType = sampled; pw[0].imageInfo = &giInfo;
            pw[1].dstSet = pack_sets_[cbIdx]; pw[1].dstBinding = 1;
            pw[1].descriptorCount = 1;
            pw[1].descriptorType = isVk ? DescriptorType::SampledDepthImage : sampled;
            pw[1].imageInfo = &dInfo;
            pw[2].dstSet = pack_sets_[cbIdx]; pw[2].dstBinding = 2;
            pw[2].descriptorCount = 1; pw[2].descriptorType = sampled; pw[2].imageInfo = &nInfo;
            pw[3].dstSet = pack_sets_[cbIdx]; pw[3].dstBinding = 3;
            pw[3].descriptorCount = 1; pw[3].descriptorType = DescriptorType::StorageImage;
            pw[3].imageInfo = &p0Info;
            pw[4].dstSet = pack_sets_[cbIdx]; pw[4].dstBinding = 4;
            pw[4].descriptorCount = 1; pw[4].descriptorType = DescriptorType::StorageImage;
            pw[4].imageInfo = &p1Info;
            pw[5].dstSet = pack_sets_[cbIdx];
            pw[5].dstBinding = isVk ? 5u : 0u;
            pw[5].descriptorCount = 1; pw[5].descriptorType = DescriptorType::UniformBuffer;
            pw[5].bufferInfo = &pcInfo;
            device_->UpdateDescriptorSets(6, pw);

            InsertStateBarrier(cmd, gi, ResourceState::UnorderedAccess, ResourceState::ShaderResource);
            // pack 纹理首用布局转换（Vulkan 校验：UNDEFINED → GENERAL；
            // Unknown = 未声明初始态，RHI 按纹理跟踪 layout 发转换）。
            if (interval_index == 0) {
                InsertStateBarrier(cmd, pack_p0_, ResourceState::Unknown,
                                   ResourceState::UnorderedAccess);
                InsertStateBarrier(cmd, pack_p1_, ResourceState::Unknown,
                                   ResourceState::UnorderedAccess);
            }
            cmd->BindComputePipeline(pack_pipeline_);
            const DescriptorSetHandle psets[] = {pack_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, pack_layout_, 0, 1, psets, 0, nullptr);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

            // ---- 回读 P0/P1 → 环槽 ----
            const u32 slot = interval_index % kSlots;
            InsertStateBarrier(cmd, pack_p0_, ResourceState::UnorderedAccess, ResourceState::CopySource);
            InsertStateBarrier(cmd, pack_p1_, ResourceState::UnorderedAccess, ResourceState::CopySource);
            BufferTextureCopyRegion r{};
            r.imageSubresource = TextureSubresourceLayers{0, 0, 1};
            r.imageExtent = Extent3D{w, h, 1};
            r.bufferOffset = 0;
            cmd->CopyTextureToBuffer(pack_p0_, readback_[slot], &r, 1);
            r.bufferOffset = static_cast<u64>(w) * h * 8ull;
            cmd->CopyTextureToBuffer(pack_p1_, readback_[slot], &r, 1);
            InsertStateBarrier(cmd, pack_p0_, ResourceState::CopySource, ResourceState::UnorderedAccess);
            InsertStateBarrier(cmd, pack_p1_, ResourceState::CopySource, ResourceState::UnorderedAccess);

            // ---- dump 快照（nr_output_ → staging，图内排序安全）----
            if (!dump_path_.empty() && tick_ >= dump_frame_ && !dump_copied_ &&
                stats_.frames_uploaded > 0) {
                InsertStateBarrier(cmd, nr_output_, ResourceState::ShaderResource,
                                   ResourceState::CopySource);
                BufferTextureCopyRegion rd{};
                rd.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                rd.imageExtent = Extent3D{w, h, 1};
                cmd->CopyTextureToBuffer(nr_output_, staging_, &rd, 1);
                InsertStateBarrier(cmd, nr_output_, ResourceState::CopySource,
                                   ResourceState::ShaderResource);
                dump_copied_ = true;
                dump_map_tick_ = tick_ + 5;
            }
        });

    // ---- CPU 推理：处理最老的已回读槽（代计数防陈旧）----
    if (this_interval) {
        // 刚提交的回读在本帧图内；处理上一 interval 的槽（≥ interval 帧旧）。
        const u32 oldest = (interval_index_ + kSlots - 1) % kSlots;
        if (interval_index_ >= 1 && slot_generation_[oldest] != 0 &&
            !upload_pending_) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = ProcessSlot(oldest, w, h);
            const f32 ms = std::chrono::duration<f32, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                upload_pending_ = true;
                ++stats_.frames_inferred;
                stats_.last_infer_ms = ms;
            }
            if (tick_ % (interval_ * 20) == 0) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[NeuralNR] infer " << w << "x" << h << " ms=" << ms
                    << " inferred=" << stats_.frames_inferred
                    << " uploaded=" << stats_.frames_uploaded
                    << " nz=" << stats_.last_nonzero_ratio << std::endl;
            }
        }
        slot_generation_[interval_index_ % kSlots] = ++interval_index_;   // 本帧槽标记
    }

    // dump 落盘（图内拷贝 +5 帧后 CPU 落盘）
    if (dump_copied_ && !dump_done_ && tick_ >= dump_map_tick_ && !dump_path_.empty()) {
        if (WriteDumpFile()) {
            PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralNR] dump written: " << dump_path_
                      << std::endl;
        }
        dump_done_ = true;
    }
}

bool NeuralNRModule::ProcessSlot(u32 slot, u32 w, u32 h) {
    const size_t plane = static_cast<size_t>(w) * h;
    auto* raw = static_cast<const u16*>(device_->MapBuffer(readback_[slot]));
    if (!raw) return false;
    // P0 平铺在前，P1 在 w*h*8 偏移；7ch = P0.rgba(log.xyz,normal.x) +
    // P1.rgb(normal.yz, depth16)——与降噪 MlxProcessSlot 同解码。
    for (size_t i = 0; i < plane; ++i) {
        const u16* p0 = raw + i * 4;
        const u16* p1 = raw + plane * 4 + i * 4;
        infer_in_[0 * plane + i] = F16ToF32(p0[0]);
        infer_in_[1 * plane + i] = F16ToF32(p0[1]);
        infer_in_[2 * plane + i] = F16ToF32(p0[2]);
        infer_in_[3 * plane + i] = F16ToF32(p0[3]);
        infer_in_[4 * plane + i] = F16ToF32(p1[0]);
        infer_in_[5 * plane + i] = F16ToF32(p1[1]);
        infer_in_[6 * plane + i] = F16ToF32(p1[2]);
    }
    device_->UnmapBuffer(readback_[slot]);

    std::string err;
    if (!backend_.Infer(infer_in_.data(), w, h, static_cast<u32>(tick_),
                        infer_out_.data(), &err)) {
        if (infer_err_.empty()) {
            infer_err_ = err;
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralNR] infer failed: "
                      << err << "——停止推理" << std::endl;
        }
        return false;
    }

    // 门禁统计 + f16 编码
    u32 nz = 0, nan = 0;
    const size_t total = plane * 3;
    for (size_t i = 0; i < total; ++i) {
        const f32 v = infer_out_[i];
        if (v > 1e-4f || v < -1e-4f) ++nz;
        if (v != v) ++nan;
    }
    stats_.last_nonzero_ratio = static_cast<f32>(nz) / static_cast<f32>(total);
    stats_.last_nan_free = nan == 0;

    auto* up = static_cast<u16*>(device_->MapBuffer(upload_));
    if (!up) return false;
    for (size_t i = 0; i < plane; ++i) {
        up[i * 4 + 0] = F32ToF16(infer_out_[0 * plane + i]);
        up[i * 4 + 1] = F32ToF16(infer_out_[1 * plane + i]);
        up[i * 4 + 2] = F32ToF16(infer_out_[2 * plane + i]);
        up[i * 4 + 3] = F32ToF16(1.0f);
    }
    device_->UnmapBuffer(upload_);
    return true;
}

/// dump 落盘（staging 已由图内拷贝填充）。
bool NeuralNRModule::WriteDumpFile() {
    if (staging_ == handles::INVALID_RESOURCE) return false;
    if (auto* texels = static_cast<const u16*>(device_->MapBuffer(staging_))) {
        std::ofstream out(dump_path_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(texels),
                  (std::streamsize)staging_size_);
        device_->UnmapBuffer(staging_);
        return bool(out);
    }
    return false;
}

}  // namespace primal::graphics
