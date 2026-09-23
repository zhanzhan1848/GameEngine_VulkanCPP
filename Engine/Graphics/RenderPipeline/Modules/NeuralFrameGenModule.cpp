/**
 * @file NeuralFrameGenModule.cpp
 * @brief R2.1 神经帧生成实现（延迟回读 + shim 插值 + 上传；范本 NeuralDenoiseModule）
 */
#include "NeuralFrameGenModule.h"

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

// IEEE-754 binary16 编解码（与 NeuralDenoiseModule 同实现；就近偶数舍入）。
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
bool NeuralFrameGenModule::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    Shutdown();
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    if (!device) return false;

    if (const char* e = std::getenv("PRIMAL_FRAMEGEN_PHASE")) {
        const f32 v = static_cast<f32>(std::atof(e));
        if (v > 0.0f && v < 1.0f) phase_ = v;
    }
    if (const char* e = std::getenv("PRIMAL_FRAMEGEN_DUMP")) {
        if (*e) dump_path_ = e;
    }
    dump_frame_ = 300;
    if (const char* e = std::getenv("PRIMAL_FRAMEGEN_DUMP_FRAME")) {
        const int v = std::atoi(e);
        if (v > 0) dump_frame_ = static_cast<u32>(v);
    }

    std::string err;
    if (!backend_.Initialize({}, {}, &err)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralFrameGen] backend init failed ("
                  << err << ")——模块停用" << std::endl;
        return false;
    }

    // 资源（输出纹理/回读环/上传环/staging）延迟到首次 AddPass 按"实际帧
    // 分辨率"创建——Initialize 拿到的是管线初始化分辨率（=窗口尺寸，
    // HiDPI 下 2560×1440），而逐帧 blit 输入是渲染目标分辨率（1280×720）。
    // 若按初始化分辨率建纹理，内容只占左上角四分之一（FinalBlit 按整纹理
    // 归一化 UV 采样，2026-09-23 截图实证）。


    initialized_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralFrameGen] initialized (resources lazy) phase=" << phase_
              << " weights=" << backend_.LoadedWeightsPath() << std::endl;
    return true;
}

void NeuralFrameGenModule::Shutdown() {
    if (device_ == nullptr) return;
    if (fg_result_ != handles::INVALID_RESOURCE) device_->DestroyTexture(fg_result_);
    fg_result_ = handles::INVALID_RESOURCE;
    if (staging_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(staging_);
    staging_ = handles::INVALID_RESOURCE;
    for (u32 i = 0; i < kSlots; ++i) {
        if (readback_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(readback_[i]);
        readback_[i] = handles::INVALID_RESOURCE;
    }
    for (u32 i = 0; i < kUploads; ++i) {
        if (upload_[i] != handles::INVALID_RESOURCE) device_->DestroyBuffer(upload_[i]);
        upload_[i] = handles::INVALID_RESOURCE;
        upload_pending_[i] = false;
    }
    backend_.Shutdown();
    initialized_ = false;
    device_ = nullptr;
}

// ---------------------------------------------------------------------------
// AddPass
// ---------------------------------------------------------------------------
bool NeuralFrameGenModule::EnsureResources(u32 w, u32 h) {
    if (resources_ready_ && frame_w_ == w && frame_h_ == h) return true;
    if (resources_ready_) return true;   // 分辨率变更不支持：报一次，沿用旧资源
    static bool res_mismatch_logged = false;
    if (resources_ready_) (void)res_mismatch_logged;

    frame_w_ = w;
    frame_h_ = h;

    TextureDesc td{};
    td.size = {w, h, 1};
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest |
               TextureUsage::CopySource;
    td.format = DataFormat::RGBA16_Float;
    td.name = "NeuralFG_Result";
    fg_result_ = device_->CreateTexture(td);
    if (fg_result_ == handles::INVALID_RESOURCE) return false;
    output_desc_ = td;

    const u64 plane_bytes = static_cast<u64>(w) * h * 8ull;
    for (u32 i = 0; i < kSlots; ++i) {
        BufferDesc rb{};
        rb.size = plane_bytes;
        rb.usage = GPUMemoryUsage::Readback;
        rb.memoryUsage = GPUMemoryUsage::Readback;
        rb.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        rb.name = "NeuralFG_Readback";
        readback_[i] = device_->CreateBuffer(rb);
        if (readback_[i] == handles::INVALID_RESOURCE) return false;
    }
    for (u32 i = 0; i < kUploads; ++i) {
        BufferDesc up{};
        up.size = plane_bytes;
        up.usage = GPUMemoryUsage::Dynamic;
        up.memoryUsage = GPUMemoryUsage::Dynamic;
        up.bindFlags = static_cast<u32>(BufferUsageFlags::TransferSrc);
        up.name = "NeuralFG_Upload";
        upload_[i] = device_->CreateBuffer(up);
        if (upload_[i] == handles::INVALID_RESOURCE) return false;
    }
    if (!dump_path_.empty()) {
        staging_size_ = plane_bytes;
        BufferDesc d{};
        d.size = staging_size_;
        d.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        d.usage = GPUMemoryUsage::Readback;
        d.memoryUsage = GPUMemoryUsage::Readback;
        d.name = "NeuralFG_Staging";
        staging_ = device_->CreateBuffer(d);
    }
    frame_a_.resize(static_cast<size_t>(w) * h * 3ull);
    frame_b_.resize(frame_a_.size());
    frame_out_.resize(frame_a_.size());
    resources_ready_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralFrameGen] resources ready: "
              << w << "x" << h << std::endl;
    return true;
}

void NeuralFrameGenModule::AddPass(RenderGraph& graph, const NeuralFrameGenInputs& inputs) {
    if (!initialized_) return;
    ++tick_;
    const u32 w = inputs.render_width ? inputs.render_width : render_width_;
    const u32 h = inputs.render_height ? inputs.render_height : render_height_;
    if (!EnsureResources(w, h)) return;
    last_present_ = inputs.present_interpolated;

    // 本帧 pass 头要上传的缓冲 = 最近一次成功处理的帧对（producer 在上一帧
    // AddPass 尾部写入）。上传与 present 解耦：无论是否换源显示，都消费
    // 生产结果——否则 PRESENT=0 时上传环塞满、生产停摆、门禁拿不到数据。
    // FinalBlit 是否换源显示由 SRP 按 framegen_present_ 单独决定。
    upload_idx_for_present_ =
        (pairs_done_ > 0 && upload_pending_[(pairs_done_ - 1) % kUploads])
            ? (pairs_done_ - 1) % kUploads
            : kUploads;

    auto resolvePhys = [&graph](RGResourceHandle handle) -> ResourceHandle {
        auto* r = graph.GetResource(handle);
        return r ? r->GetPhysicalHandle() : handles::INVALID_RESOURCE;
    };
    const ResourceHandle blit_input_phys = resolvePhys(inputs.blit_input);

    graph.AddPass<int>(
        "NeuralFrameGen",
        RGPassType::Compute,
        RGPassCategory::PostProcess,
        [&](int&, RenderGraphBuilder& builder) {
            builder.SideEffect();
            if (blit_input_phys != handles::INVALID_RESOURCE) {
                builder.Read(inputs.blit_input, ResourceState::ShaderResource);
            }
        },
        [this, w, h, blit_input_phys,
         upload_idx = upload_idx_for_present_](const int&,
                                               RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;

            // ---- 上传：上一轮插值结果 → fg_result_（供本帧 FinalBlit 读）----
            if (upload_idx < kUploads && upload_pending_[upload_idx]) {
                // 首次上传前发首用布局转换（Vulkan 校验：UNDEFINED → 转换目标）。
                if (frames_uploaded_ == 0) {
                    InsertStateBarrier(cmd, fg_result_, ResourceState::Unknown,
                                       ResourceState::CopyDest);
                } else {
                    InsertStateBarrier(cmd, fg_result_, ResourceState::ShaderResource,
                                       ResourceState::CopyDest);
                }
                BufferTextureCopyRegion up{};
                up.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                up.imageExtent = Extent3D{w, h, 1};
                cmd->CopyBufferToTexture(upload_[upload_idx], fg_result_, &up, 1);
                InsertStateBarrier(cmd, fg_result_, ResourceState::CopyDest,
                                   ResourceState::ShaderResource);
                upload_pending_[upload_idx] = false;
                ++frames_uploaded_;
            }

            // ---- 回读：当帧 blit 输入 → 环槽（builder.Read 已保证 SRV 态）----
            if (blit_input_phys == handles::INVALID_RESOURCE) return;
            const u32 slot = static_cast<u32>((tick_ - 1) % kSlots);
            InsertStateBarrier(cmd, blit_input_phys, ResourceState::ShaderResource,
                               ResourceState::CopySource);
            BufferTextureCopyRegion r{};
            r.imageSubresource = TextureSubresourceLayers{0, 0, 1};
            r.imageExtent = Extent3D{w, h, 1};
            cmd->CopyTextureToBuffer(blit_input_phys, readback_[slot], &r, 1);
            InsertStateBarrier(cmd, blit_input_phys, ResourceState::CopySource,
                               ResourceState::ShaderResource);

            // ---- dump 快照（插值输出）----
            if (!dump_path_.empty() && tick_ == dump_frame_ && !dump_copied_) {
                InsertStateBarrier(cmd, fg_result_, ResourceState::ShaderResource,
                                   ResourceState::CopySource);
                BufferTextureCopyRegion rd{};
                rd.imageSubresource = TextureSubresourceLayers{0, 0, 1};
                rd.imageExtent = Extent3D{w, h, 1};
                cmd->CopyTextureToBuffer(fg_result_, staging_, &rd, 1);
                InsertStateBarrier(cmd, fg_result_, ResourceState::CopySource,
                                   ResourceState::ShaderResource);
                dump_copied_ = true;
                dump_map_tick_ = tick_ + 5;
            }
        });

    // dump 回读（CPU，+5 帧）
    if (dump_copied_ && !dump_done_ && tick_ >= dump_map_tick_) {
        if (auto* texels = static_cast<const u16*>(device_->MapBuffer(staging_))) {
            std::ofstream out(dump_path_, std::ios::binary);
            out.write(reinterpret_cast<const char*>(texels),
                      (std::streamsize)staging_size_);
            device_->UnmapBuffer(staging_);
            PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralFrameGen] dump written: "
                      << dump_path_ << " (" << staging_size_ << " B RGBA16F)" << std::endl;
        }
        dump_done_ = true;
    }

    // ---- CPU 处理：(t-3, t-2) 帧对（≥2 帧裕量免 stall）----
    if (tick_ >= kSlots) {
        const u32 f = static_cast<u32>(tick_ - 1);   // 本槽已写入的帧号
        const u32 slot_a = (f - (kSlots - 1)) % kSlots;   // t-3
        const u32 slot_b = (f - (kSlots - 2)) % kSlots;   // t-2
        const u32 ui = pairs_done_ % kUploads;
        if (!upload_pending_[ui]) {
            const auto cpu_start = std::chrono::steady_clock::now();
            const bool ok = ProcessPair(slot_a, slot_b, ui, w, h);
            const f32 ms = std::chrono::duration<f32, std::milli>(
                               std::chrono::steady_clock::now() - cpu_start).count();
            if (ok) {
                upload_pending_[ui] = true;
                ++pairs_done_;
            }
            last_cpu_ms_ = ms;
            cpu_ms_peak_ = std::max(cpu_ms_peak_, ms);
            if (tick_ % 120 == 0) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[NeuralFrameGen] interp " << w << "x" << h
                    << " cpu=" << ms << "ms (decode=" << decode_ms_
                    << " infer=" << infer_ms_ << " encode=" << encode_ms_
                    << ") peak=" << cpu_ms_peak_
                    << "ms phase=" << phase_
                    << " uploaded=" << frames_uploaded_
                    << (infer_ms_ < 16.0f ? " infer16ms=PASS" : " infer16ms=FAIL")
                    << std::endl;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ProcessPair：槽解码（rgba8 LDR 或 rgba16f HDR→tonemap）→ interpolate → f16 编码
// ---------------------------------------------------------------------------
bool NeuralFrameGenModule::ProcessPair(u32 slot_a, u32 slot_b, u32 upload_idx,
                                       u32 w, u32 h) {
    const size_t plane = static_cast<size_t>(w) * h;

    auto decode = [&](u32 slot, std::vector<f32>& frame) -> bool {
        const u16* raw16 = static_cast<const u16*>(device_->MapBuffer(readback_[slot]));
        if (!raw16) return false;
        const u8* raw8 = reinterpret_cast<const u8*>(raw16);
        for (size_t i = 0; i < plane; ++i) {
            frame[i * 3 + 0] = static_cast<f32>(raw8[i * 4 + 0]) * (1.0f / 255.0f);
            frame[i * 3 + 1] = static_cast<f32>(raw8[i * 4 + 1]) * (1.0f / 255.0f);
            frame[i * 3 + 2] = static_cast<f32>(raw8[i * 4 + 2]) * (1.0f / 255.0f);
        }
        device_->UnmapBuffer(readback_[slot]);
        return true;
    };
    const auto t0 = std::chrono::steady_clock::now();
    if (!decode(slot_a, frame_a_) || !decode(slot_b, frame_b_)) return false;
    const auto t1 = std::chrono::steady_clock::now();

    std::string err;
    if (!backend_.Interpolate(frame_a_.data(), frame_b_.data(), w, h, phase_,
                              frame_out_.data(), &err)) {
        if (infer_err_.empty()) {
            infer_err_ = err;
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralFrameGen] interpolate failed: "
                      << err << "——停止处理" << std::endl;
        }
        return false;
    }
    const auto t2 = std::chrono::steady_clock::now();

    auto* up = static_cast<u16*>(device_->MapBuffer(upload_[upload_idx]));
    if (!up) return false;
    for (size_t i = 0; i < plane; ++i) {
        up[i * 4 + 0] = F32ToF16(std::min(std::max(frame_out_[i * 3 + 0], 0.0f), 1.0f));
        up[i * 4 + 1] = F32ToF16(std::min(std::max(frame_out_[i * 3 + 1], 0.0f), 1.0f));
        up[i * 4 + 2] = F32ToF16(std::min(std::max(frame_out_[i * 3 + 2], 0.0f), 1.0f));
        up[i * 4 + 3] = F32ToF16(1.0f);
    }
    device_->UnmapBuffer(upload_[upload_idx]);
    const auto t3 = std::chrono::steady_clock::now();

    decode_ms_ = std::chrono::duration<f32, std::milli>(t1 - t0).count();
    infer_ms_ = std::chrono::duration<f32, std::milli>(t2 - t1).count();
    encode_ms_ = std::chrono::duration<f32, std::milli>(t3 - t2).count();
    return true;
}

}  // namespace primal::graphics
