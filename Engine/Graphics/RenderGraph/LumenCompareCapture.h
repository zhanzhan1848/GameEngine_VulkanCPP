#pragma once
// Diagnostic-only observer. Copies immediately after the real producer, before
// a later pass can overwrite the same physical allocation. No additional rays.
// F0-11 (D3D12 M0): 后端专属查询经 RHICaptureBridge 分派（本头不再 include
// 任何 Platforms/* 头，不再 static_cast 后端设备——Vulkan 行为零变化，
// 特化在 Platforms/Vulkan/VulkanCaptureBridge.cpp，D3D12 待 M1 同模式提供）。
#include "RenderGraphPass.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RHICaptureBridge.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <chrono>

namespace primal::graphics::lumencompare {
using namespace rhi;
using namespace rendergraph;
struct Item {
    std::string pass, name, status = "pending", file;
    u32 index = 0, version = 0, width = 0, height = 0, depth = 1, bpp = 0, format = 0;
    u64 bytes = 0, physical = 0, sourceOffset = 0;
    ResourceHandle staging = handles::INVALID_RESOURCE;
};
inline std::vector<Item> items;
inline u32 ordinal = 0, passIndex = 0;
inline bool enabled = false;
inline constexpr u64 defaultStagingBudgetBytes = 1024ull * 1024 * 1024;
inline u64 stagingBudgetBytes = defaultStagingBudgetBytes;
// Observer allocation only. Strict decimal MiB, bounded to avoid accidental
// unbounded readback allocations; malformed/out-of-range values keep 1 GiB.
inline u64 ParseStagingBudgetBytes(const char* value) {
    if (!value || !*value) return defaultStagingBudgetBytes;
    u32 mib = 0;
    for (const char* c = value; *c; ++c) {
        if (*c < '0' || *c > '9') return defaultStagingBudgetBytes;
        mib = mib * 10u + static_cast<u32>(*c - '0');
        if (mib > 4096u) return defaultStagingBudgetBytes;
    }
    return mib >= 64u ? u64(mib) * 1024u * 1024u : defaultStagingBudgetBytes;
}
inline const auto clockStart = std::chrono::steady_clock::now();
inline void Begin(u32 frameOrdinal) {
    ordinal = frameOrdinal; passIndex = 0;
    enabled = false;
    if (!std::getenv("LUMEN_COMPARE_DIR")) return;
    stagingBudgetBytes = ParseStagingBudgetBytes(std::getenv("LUMEN_COMPARE_STAGING_MIB"));
    const char* requested = std::getenv("LUMEN_COMPARE_FRAMES");
    std::istringstream in(requested ? requested : "1,5,35,75,150,360");
    std::string token;
    while (std::getline(in, token, ',')) if (std::strtoul(token.c_str(), nullptr, 10) == ordinal) enabled = true;
}
inline bool Selected(const std::string& name) {
    const char* filter = std::getenv("LUMEN_COMPARE_PASSES");
    std::istringstream in(filter ? filter : "DDGI,SurfaceCache,ScreenProbe,GI,Fusion,TAA,Tone");
    std::string token;
    while (std::getline(in, token, ',')) if (token == "*" || (!token.empty() && name.find(token) != std::string::npos)) return true;
    return false;
}
inline u32 PixelBytes(DataFormat f) {
    switch (f) {
    case DataFormat::R8_UNorm: case DataFormat::R8_UInt: return 1;
    case DataFormat::R16_Float: case DataFormat::R16_UInt: case DataFormat::RG8_UNorm: return 2;
    case DataFormat::D32_Float: case DataFormat::R32_Float: case DataFormat::R32_UInt: case DataFormat::RG16_Float:
    case DataFormat::RGBA8_UNorm: case DataFormat::BGRA8_UNorm: case DataFormat::RGBA8_sRGB: return 4;
    case DataFormat::RG32_Float: case DataFormat::RGBA16_Float: return 8;
    case DataFormat::RGB32_Float: return 12;
    case DataFormat::RGBA32_Float: return 16;
    default: return 0;
    }
}
inline void AfterPass(RHIDeviceBase& base, RHICommandBuffer* cmd, const RenderGraphPass& pass) {
    if (!enabled) return;
    ++passIndex;
    // F0-11: 旧门 = ENABLE_VULKAN 编译 + GetPlatform()==Vulkan；新门 = 该后端
    // 是否注册了 capture 特化。Vulkan 注册见 VulkanCaptureBridge.cpp；
    // 未注册后端（Metal/Dawn/D3D12-未实现）与旧行为一致：整帧零捕获。
    if (!rhicapture::BackendCaptureSupported(base) || !Selected(pass.GetName())) return;
    for (const auto& output : pass.GetOutputs()) {
        auto* resource = output.resource;
        if (!resource) continue;
        Item item;
        item.pass = pass.GetName(); item.name = resource->GetName();
        item.index = resource->GetHandle().index; item.version = resource->GetHandle().version;
        item.physical = resource->GetPhysicalHandle();
        item.file = "pass-" + std::to_string(passIndex) + "-resource-" + std::to_string(item.index) + "-v" + std::to_string(item.version) + ".raw";
        // Vulkan has separate handle allocators for buffers and textures: the
        // numeric handle alone is NOT a type discriminator. Legacy imports
        // have Unknown RG type. Only explicitly audited import names may be
        // resolved here; other unknowns remain a visible coverage gap.
        const bool ddgiBuffer = item.name.find("LumenDDGI_Irradiance") != std::string::npos ||
            item.name.find("LumenDDGI_Depth") != std::string::npos || item.name.find("DDGI_DepthProvenance") != std::string::npos;
        const bool knownTexture = item.name.find("SurfaceCache_Lighting") == 0 ||
            item.name == "SC_SurfaceValidity" || // audited albedo-atlas import, never a buffer
            item.name.find("SurfaceCache_NoDirect") == 0 || item.name.find("ScreenProbeGI_") == 0 ||
            item.name == "DDGIHalfResGI" || item.name == "DDGIHalfResGlossy" || item.name == "FusionIndirect" ||
            item.name.find("RadianceCascade_Write_") == 0 ||
            item.name.find("ShadowMap_C") == 0 || item.name == "ShadowVisibility" ||
            item.name == "ShadowTranslucent" || item.name == "LumenSSAO_Trace" ||
            item.name == "LumenSSAO_Filter" || item.name == "BackBuffer";
        if (resource->GetType() == RGResourceType::Unknown && !ddgiBuffer && !knownTexture) {
            item.status = "untyped_import"; items.push_back(item); continue;
        }
        const bool isBuffer = resource->GetType() == RGResourceType::Buffer || ddgiBuffer;
        // F0-11: 纹理解析经后端桥（Vulkan 特化 = 旧直查路径，零行为变化）。
        rhicapture::TextureResolve resolve;
        const auto* texture = isBuffer ? nullptr
            : (rhicapture::ResolveTexture(base, item.physical, resolve) && resolve.hasNativeImage ? &resolve : nullptr);
        if (texture) {
            const auto& desc = texture->desc;
            item.format = u32(desc.format); item.bpp = PixelBytes(desc.format);
            item.width = texture->width; item.height = texture->height;
            item.depth = std::max(1u, desc.size.z);
            // Initial format contract: complete single-subresource color textures.
            // Unsupported depth/arrays/mips are listed explicitly, never called complete.
            if (!item.bpp || desc.mipLevels != 1 || desc.arraySize != 1 ||
                (desc.type != TextureType::Texture2D && desc.type != TextureType::Texture3D && desc.type != TextureType::Unknown)) {
                item.status = "unsupported_texture_layout_or_format"; items.push_back(item); continue;
            }
            item.bytes = u64(item.width) * item.height * item.depth * item.bpp;
        } else if (!base.GetBufferByteSize(item.physical, item.bytes)) {
            item.status = "resource_unresolved"; items.push_back(item); continue;
        }
        u64 pendingBytes = 0;
        for (const auto& i : items) if (i.staging != handles::INVALID_RESOURCE) pendingBytes += i.bytes;
        if (!item.bytes || pendingBytes + item.bytes > stagingBudgetBytes) {
            item.status = "staging_budget_exceeded"; items.push_back(item); continue;
        }
        BufferDesc staging{}; staging.size = item.bytes;
        staging.bindFlags = u32(BufferUsageFlags::TransferDst); staging.memoryUsage = GPUMemoryUsage::Readback;
        item.staging = base.CreateBuffer(staging);
        if (item.staging == handles::INVALID_RESOURCE) {
            item.status = "allocation_failed"; items.push_back(item); continue;
        }
        cmd->MemoryBarrier(PipelineStage::AllCommands, PipelineStage::Transfer, AccessFlag::MemoryWrite, AccessFlag::TransferRead);
        if (texture) {
            ResourceBarrier barrier{}; barrier.resource = item.physical;
            barrier.beforeState = output.state; barrier.afterState = ResourceState::CopySource;
            cmd->InsertBarrier(&barrier, 1);
            BufferTextureCopyRegion region{}; region.imageSubresource.layerCount = 1;
            region.imageExtent = {item.width, item.height, item.depth};
            cmd->CopyTextureToBuffer(item.physical, item.staging, &region, 1);
            barrier.beforeState = ResourceState::CopySource; barrier.afterState = output.state;
            cmd->InsertBarrier(&barrier, 1);
        } else {
            cmd->CopyBuffer(item.physical, item.staging, 0, 0, item.bytes);
        }
        cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::AllCommands | PipelineStage::Host,
            AccessFlag::TransferRead | AccessFlag::TransferWrite, AccessFlag::MemoryRead | AccessFlag::MemoryWrite | AccessFlag::HostRead);
        items.push_back(item);
    }
}
// For dispatches hidden inside a larger RG pass: the caller supplies the exact
// native output and its real state at this producer boundary.
inline void Output(RHIDeviceBase* device, RHICommandBuffer* cmd, const std::string& stage,
                   const std::string& name, ResourceHandle handle, bool buffer,
                   ResourceState state = ResourceState::UnorderedAccess) {
    if (!enabled || !device || !cmd || handle == handles::INVALID_RESOURCE) return;
    struct Boundary final : RenderGraphPass {
        explicit Boundary(const std::string& label) : RenderGraphPass(label, RGPassType::Compute) {}
        void Execute(RenderGraphContext&) override {}
    } boundary(stage);
    RenderGraphResource resource(name, {}, buffer ? RGResourceType::Buffer : RGResourceType::Texture);
    resource.SetPhysicalHandle(handle);
    boundary.AddOutput(&resource, state);
    AfterPass(*device, cmd, boundary);
}
// Explicit bounded buffer ranges for long history observations. Never infer a
// source type from a numeric handle; caller has already identified a buffer.
inline void BufferSlice(RHIDeviceBase* base, RHICommandBuffer* cmd, const std::string& stage,
                        const std::string& name, ResourceHandle source, u64 offset, u64 bytes) {
    if (!enabled || !base || !cmd || !Selected(stage)) return;
    if (!rhicapture::BackendCaptureSupported(*base)) return;  // F0-11: 后端能力门（同旧 Vulkan 门语义）
    Item item; item.pass=stage; item.name=name; item.physical=source;
    item.sourceOffset=offset; item.bytes=bytes;
    item.file="slice-"+std::to_string(++passIndex)+".raw";
    u64 size=0, pending=0;
    for (const auto& i:items) if(i.staging!=handles::INVALID_RESOURCE) pending+=i.bytes;
    if (!bytes || !base->GetBufferByteSize(source,size) || offset>size || bytes>size-offset) {
        item.status="invalid_buffer_slice";items.push_back(item);return;
    }
    if (pending>stagingBudgetBytes || bytes>stagingBudgetBytes-pending) {
        item.status="staging_budget_exceeded";items.push_back(item);return;
    }
    BufferDesc desc{};desc.size=bytes;desc.bindFlags=u32(BufferUsageFlags::TransferDst);
    desc.memoryUsage=GPUMemoryUsage::Readback;
    item.staging=base->CreateBuffer(desc);
    if(item.staging==handles::INVALID_RESOURCE) {
        item.status="allocation_failed";items.push_back(item);return;
    }
    cmd->MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,
        AccessFlag::MemoryWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(source,item.staging,offset,0,bytes);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::AllCommands|PipelineStage::Host,
        AccessFlag::TransferRead|AccessFlag::TransferWrite,
        AccessFlag::MemoryRead|AccessFlag::MemoryWrite|AccessFlag::HostRead);
    items.push_back(item);
}
inline bool Drain(RHIDeviceBase* device, u64 engineFrame, u32 giAgeAfterRender) {
    if (!enabled) return true;
    const std::filesystem::path directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) / ("frame-" + std::to_string(ordinal));
    std::filesystem::create_directories(directory);
    const auto handle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* cmd = GetCommandBuffer(handle);
    bool completed = cmd && cmd->Begin();
    if (completed) completed = cmd->End() && cmd->Submit() && cmd->WaitForCompletion();
    if (handle != handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(handle);
    bool ioOk = completed;
    for (auto& item : items) {
        if (item.staging == handles::INVALID_RESOURCE) continue;
        item.status = "submission_failed";
        if (completed) {
            void* data = device->MapBuffer(item.staging, 0, item.bytes);
            if (data) {
                std::ofstream out(directory / item.file, std::ios::binary);
                out.write(static_cast<const char*>(data), item.bytes); out.close();
                item.status = out ? "captured" : "write_failed";
                device->UnmapBuffer(item.staging);
            } else item.status = "map_failed";
        }
        if (item.status != "captured") ioOk = false;
        device->DestroyBuffer(item.staging);
    }
    std::ofstream meta(directory / "capture.json");
    meta << "{\"schema\":1,\"engine\":\"primal\",\"ordinal\":" << ordinal
         << ",\"engine_frame\":" << engineFrame << ",\"gi_age_after_render\":" << giAgeAfterRender
         << ",\"staging_budget_bytes\":" << stagingBudgetBytes
         << ",\"elapsed_seconds\":" << std::chrono::duration<double>(std::chrono::steady_clock::now()-clockStart).count()
         << ",\"frame_origin\":\"first measured GI view after optional raster-only preparation (one based)\",\"completion_checked\":"
         << (completed ? "true" : "false") << ",\"complete\":";
    bool all = ioOk && !items.empty();
    for (const auto& item : items) all = all && item.status == "captured";
    meta << (all ? "true" : "false") << ",\"resources\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& s = items[i];
        if (i) meta << ',';
        meta << "{\"pass\":" << std::quoted(s.pass) << ",\"name\":" << std::quoted(s.name)
             << ",\"status\":" << std::quoted(s.status) << ",\"file\":" << std::quoted(s.file)
             << ",\"index\":" << s.index << ",\"version\":" << s.version << ",\"physical\":" << s.physical
             << ",\"width\":" << s.width << ",\"height\":" << s.height << ",\"depth\":" << s.depth << ",\"bpp\":" << s.bpp
             << ",\"format\":" << s.format << ",\"bytes\":" << s.bytes
             << ",\"source_offset\":" << s.sourceOffset << '}';
    }
    meta << "]}\n"; meta.close();
    items.clear(); enabled = false;
    return ioOk && bool(meta);
}
}
