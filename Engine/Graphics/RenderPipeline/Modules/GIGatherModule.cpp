#include "GIGatherModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/RHI/Core/RHIMath.h"
// T1.9 (E3): platform headers removed — this module is backend-agnostic
// (platform branches go through the RHI capability bits below). Do not
// re-add platform includes here; Dawn/Metal builds must stay compilable.
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

static const char* GatherAuditFormatName(DataFormat format) {
    switch (format) {
        case DataFormat::R16_Float:    return "r16f";
        case DataFormat::RGBA16_Float: return "rgba16f";
        case DataFormat::BGRA8_UNorm:  return "bgra8";
        case DataFormat::RGBA8_UNorm:  return "rgba8";
        case DataFormat::D32_Float:    return "d32f";
        case DataFormat::R32_Float:    return "r32f";
        default:                       return "unknown";
    }
}

struct GIGatherCB {
    math::m4x4 inv_view_projection;
    math::v4   probe_origin_spacing;
    math::v4   probe_counts;
    math::v4   camera_pos;  // T4.3: world-space camera for reflection vectors
    // T6.3 sub-A: probe_vis 数据面。debug_vis.x = GIDebug().probe_vis 模式
    // （0=Off）。经 binding 6（48B range，第三 vec4 = offset 112）上传；
    // camera_pos（offset 96）由 Vulkan binding 6 / Metal buffer(2) 读取。
    math::v4   debug_vis;
};

static_assert(offsetof(GIGatherCB, probe_origin_spacing) == 64);
static_assert(offsetof(GIGatherCB, probe_counts) == 80);
static_assert(offsetof(GIGatherCB, camera_pos) == 96);
static_assert(offsetof(GIGatherCB, debug_vis) == 112);

// T6.3 sub-B → T3.6/D1: volume 参数 CB（binding 18，std140 布局与
// DDGIGIGather.comp 的 FlatVolumeCB 一致）。NVolumeCB 原位改型：counts[].w
// 从 enabled 位改为该 volume 在 flat SSBO 里的 probe 偏移（enabled 由
// spacing>0 隐含——未用槽位 CPU 侧清零，spacing=0 被 shader 选择器跳过）。
struct FlatVolumeCB {
    math::v4 origin_spacing[4];   // xyz = origin, w = spacing
    math::v4 counts_offset[4];    // xyz = counts, w = probe offset (flat SSBO)
    math::v4 meta;                // x = 有效 volume 数（0 = flat 路径关闭）
                                  // w = low-frequency gather blend override
                                  //     [0,1], -1 keeps backend default.
};

struct GPUStaticProbeData {
    math::v4 ProbeOrigin;
    float  ProbeSpacing;
    u32    GridDimX;
    u32    GridDimY;
    u32    GridDimZ;
    float  _pad[2];
    math::v4 SkySH[9];
};

bool GIGatherModule::Initialize(RHIDeviceBase* device, ShaderHandle shader,
                                 u32 render_width, u32 render_height,
                                 ShaderHandle glossy_source_audit_shader) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

    if (shader == handles::INVALID_SHADER) return false;

    // 15 bindings: 6 textures + 9 buffers.
    // Metal: overlapping texture/buffer namespaces (idiomatic Metal).
    // Vulkan: flat 0..14 (depth as SampledDepthImage) — matches GLSL shader.
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;
    const char* glossyAuditEnv = std::getenv("DDGI_GLOSSY_SOURCE_AUDIT");
    const bool glossyAuditRequested = isVk && glossyAuditEnv &&
        glossyAuditEnv[0] == '1' && glossyAuditEnv[1] == '\0';
    const char* gatherAuditPrefix = std::getenv("DDGI_GATHER_AUDIT");
    if (glossyAuditRequested && (!gatherAuditPrefix || !gatherAuditPrefix[0])) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_GLOSSY_SOURCE_AUDIT] requested without DDGI_GATHER_AUDIT prefix"
            << std::endl;
        return false;
    }
    if (glossyAuditRequested && glossy_source_audit_shader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[DDGI_GLOSSY_SOURCE_AUDIT] requested but auxiliary shader handle is invalid"
            << std::endl;
        return false;
    }
    glossy_source_audit_enabled_ = glossyAuditRequested;
    auto failAuditInitialization = [&]() {
        // A diagnostic setup failure must never leave the production pipeline
        // looking usable to a caller that ignores Initialize's bool result.
        if (glossyAuditRequested)
            pipeline_ = handles::INVALID_PIPELINE;
        return false;
    };
    // T3.6/D1: flat-buffer 路径取代 N-volume descriptor-indexing 数组与
    // near/far 双槽 legacy 路径——不再依赖 descriptor indexing，Metal 与
    // Vulkan 同一条路径（消除 nvolume_supported_ 能力差异）。
    if (isVk) {
        DescriptorSetLayoutBinding vkBindings[21] = {
            {0,  DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // depth
            {1,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // normal
            {2,  DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // output
            {3,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // history
            {4,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // invViewProj; camera is in binding 6
            {5,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {6,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeCounts (+debugVis, 48B)
            {7,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkySH
            {8,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkyFactor
            {9,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // staticProbeParams
            {10, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // confidenceBuffer
            {11, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // near irradiance (debug route A)
            {12, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // depthBuffer
            {13, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // gbuffer_orm (T4.3)
            {14, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // specular output (T4.3)
            // T3.6/D1: binding 17/18 原位改型——17 = flat irradiance SSBO
            //（单 buffer，offset 256 跳过头部），18 = FlatVolumeCB。原 15/16
            //（far 双槽）与 17 array[4] 删除。
            {17, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // flat irradiance (T3.6/D1)
            {18, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // FlatVolumeCB (T3.6/D1)
            // 2026-09-17 P0：近场 volume 重定位后有效起点（可见性锚点）
            {19, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // near relocated origins
            // GI 根治 Phase 3.2: flat openSky 统计（天空可见性分类）
            {20, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // flat probe openSky
        };
        set_layout_ = device->CreateDescriptorSetLayout({19, vkBindings});
        if (glossy_source_audit_enabled_) {
            // Binding 19/20 are already live production resources (relocated
            // origins/openSky).  Keep the regular 19-entry descriptor layout
            // byte-for-byte stable and append the diagnostic SSBOs only to
            // the auxiliary source layout.
            vkBindings[19] = {21, DescriptorType::StorageBuffer, 1,
                               ShaderStage::Compute, nullptr};
            vkBindings[20] = {22, DescriptorType::StorageBuffer, 1,
                               ShaderStage::Compute, nullptr};
            glossy_source_audit_set_layout_ =
                device->CreateDescriptorSetLayout({21, vkBindings});
        }
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // history
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // orm (T4.3)
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // specular output (T4.3)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // invViewProj
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // probeCounts
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // staticSkySH
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // staticSkyFactor
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // staticProbeParams
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // confidenceBuffer
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // depthBuffer
            // T3.6/D1: 10/11 原位改型为 flat SSBO + FlatVolumeCB；原 7（near
            // irr）/9（dup）/10（far irr）/11（far params）删除。
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // flat irradiance (T3.6/D1)
            {11, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // FlatVolumeCB (T3.6/D1)
            // 2026-09-17 P0：near relocated origins（Metal buffer(12)，与 Vulkan flat 19 对等）
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // near relocated origins
            // GI 根治 Phase 3.2: flat openSky（Metal buffer(13)，与 Vulkan flat 20 对等）
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // flat probe openSky
        };
        // 修复：18 个条目（含 openSky buffer(13)）但 count 传 17——最后一个
        // 条目从未进布局，Metal ddgi_gi_gather 编码期断言
        // "missing Buffer binding at index 12/13"。
        set_layout_ = device->CreateDescriptorSetLayout({18, bindings});
    }
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    if (glossy_source_audit_enabled_) {
        glossy_source_audit_layout_ =
            device->CreatePipelineLayout({1, &glossy_source_audit_set_layout_});
        if (glossy_source_audit_layout_ == handles::INVALID_PIPELINE_LAYOUT ||
            glossy_source_audit_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] failed to create auxiliary layout"
                << std::endl;
            return failAuditInitialization();
        }
    }

    ComputePipelineDesc pd{};
    pd.computeShader = shader;
    pd.layout = layout_;
    pd.threadGroupSize = {8, 8, 1};
    pipeline_ = device->CreateComputePipeline(pd);
    if (pipeline_ == handles::INVALID_PIPELINE) return false;

    if (glossy_source_audit_enabled_) {
        ComputePipelineDesc auditPd{};
        auditPd.computeShader = glossy_source_audit_shader;
        auditPd.layout = glossy_source_audit_layout_;
        auditPd.threadGroupSize = {8, 8, 1};
        glossy_source_audit_pipeline_ = device->CreateComputePipeline(auditPd);
        if (glossy_source_audit_pipeline_ == handles::INVALID_PIPELINE) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary pipeline creation failed"
                << std::endl;
            return failAuditInitialization();
        }
    }

    for (u32 i = 0; i < 3; ++i)
        descriptor_sets_[i] = device->CreateDescriptorSet({set_layout_});

    // Vulkan 全分辨率诊断直接使用各接收像素的深度/法线，避免半分辨率
    // DDGI 在线性上采样时混合不同表面。默认保留半分辨率；Metal 沿用全分辨率。
    // 命名 gi_halfres_* 沿用历史，实际尺寸以 gather_width_/gather_height_ 为准。
    const char* fullResGatherEnv = std::getenv("PRIMAL_DDGI_FULL_RES_GATHER");
    const bool fullResGatherOptIn = isVk && fullResGatherEnv &&
        std::strcmp(fullResGatherEnv, "1") == 0;
    if (device->GetPlatform() == RHIPlatform::Metal || fullResGatherOptIn) {
        gather_width_ = render_width;
        gather_height_ = render_height;
    } else {
        gather_width_ = render_width / 2;
        gather_height_ = render_height / 2;
    }
    PRIMAL_LOG_STREAM(Info, "Render") << "[DDGI_GATHER_RESOLUTION] "
        << gather_width_ << "x" << gather_height_
        << " vulkan_full_res_opt_in=" << fullResGatherOptIn << std::endl;
    TextureDesc texDesc{};
    texDesc.size = {gather_width_, gather_height_, 1};
    texDesc.format = DataFormat::RGBA16_Float;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    gi_halfres_texture_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_halfres_texture_);  // P0-B 首帧 priming
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    gi_halfres_history_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_halfres_history_);  // P0-B 首帧 priming
    // T4.3: half-res glossy specular output (RGB = SH(R) radiance, A = NdotV)
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    gi_specular_texture_ = device->CreateTexture(texDesc);
    device->PrimeTextureLayout(gi_specular_texture_);  // P0-B 首帧 priming

    if (glossy_source_audit_enabled_) {
        // Nine vec4 records per output pixel: the first three carry the
        // receiver/source tuple, the remaining six carry the exact packed
        // fetch/weight/index fields.  Keep the opt-in allocation bounded so a
        // diagnostic request cannot consume an unbounded amount of VRAM.
        const u64 pixels = static_cast<u64>(gather_width_) * gather_height_;
        constexpr u64 kRecordsPerPixel = 9;
        constexpr u64 kBytesPerRecord = 16;
        constexpr u64 kMaxBytes = 128ull * 1024ull * 1024ull;
        glossy_source_audit_bytes_ = pixels * kRecordsPerPixel * kBytesPerRecord;
        if (glossy_source_audit_bytes_ == 0 || glossy_source_audit_bytes_ > kMaxBytes) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] refusing auxiliary buffer size "
                << glossy_source_audit_bytes_ << " bytes (limit=" << kMaxBytes << ")"
                << std::endl;
            return failAuditInitialization();
        }

        TextureDesc auditTexDesc{};
        auditTexDesc.size = {gather_width_, gather_height_, 1};
        auditTexDesc.format = DataFormat::RGBA16_Float;
        auditTexDesc.type = TextureType::Texture2D;
        auditTexDesc.usage = TextureUsage::ShaderResource |
                             TextureUsage::UnorderedAccess |
                             TextureUsage::CopySource;
        auditTexDesc.name = "DDGI_GlossySourceAuditOutput";
        glossy_source_audit_output_ = device->CreateTexture(auditTexDesc);
        auditTexDesc.name = "DDGI_GlossySourceAuditSpecular";
        glossy_source_audit_specular_ = device->CreateTexture(auditTexDesc);
        if (glossy_source_audit_output_ == handles::INVALID_RESOURCE ||
            glossy_source_audit_specular_ == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary output allocation failed"
                << std::endl;
            return failAuditInitialization();
        }
        device->PrimeTextureLayout(glossy_source_audit_output_);
        device->PrimeTextureLayout(glossy_source_audit_specular_);

        BufferDesc auditDesc{};
        auditDesc.size = glossy_source_audit_bytes_;
        auditDesc.type = BufferType::Structured;
        auditDesc.usage = GPUMemoryUsage::Dynamic;
        auditDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        auditDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                              static_cast<u32>(BufferUsageFlags::TransferSrc);
        auditDesc.name = "DDGI_GlossySourceAuditRecords";
        for (u32 i = 0; i < 3; ++i) {
            glossy_source_audit_buffer_[i] = device->CreateBuffer(auditDesc);
            if (glossy_source_audit_buffer_[i] == handles::INVALID_RESOURCE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary record allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
            glossy_source_audit_sets_[i] = device->CreateDescriptorSet({glossy_source_audit_set_layout_});
            if (glossy_source_audit_sets_[i] == handles::INVALID_DESCRIPTOR_SET) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] auxiliary descriptor allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
        }

        // Two vec4 records per half-res pixel: actual ORM texel/roughness/
        // frame stamp plus the exact fetched RGBA sample.  This is an
        // independent 32 MiB-per-buffer budget; the existing 9-vec4 source
        // records keep their separate 128 MiB budget above.
        constexpr u64 kMaterialRecordsPerPixel = 2;
        constexpr u64 kMaxMaterialBytes = 32ull * 1024ull * 1024ull;
        glossy_material_audit_bytes_ = pixels * kMaterialRecordsPerPixel * kBytesPerRecord;
        if (glossy_material_audit_bytes_ == 0 ||
            glossy_material_audit_bytes_ > kMaxMaterialBytes) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[DDGI_GLOSSY_SOURCE_AUDIT] refusing material buffer size "
                << glossy_material_audit_bytes_ << " bytes (limit=" << kMaxMaterialBytes << ")"
                << std::endl;
            return failAuditInitialization();
        }
        BufferDesc materialDesc{};
        materialDesc.size = glossy_material_audit_bytes_;
        materialDesc.type = BufferType::Structured;
        materialDesc.usage = GPUMemoryUsage::Dynamic;
        materialDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        materialDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                 static_cast<u32>(BufferUsageFlags::TransferSrc);
        materialDesc.name = "DDGI_GlossyMaterialAuditRecords";
        for (u32 i = 0; i < 3; ++i) {
            glossy_material_audit_buffer_[i] = device->CreateBuffer(materialDesc);
            if (glossy_material_audit_buffer_[i] == handles::INVALID_RESOURCE) {
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[DDGI_GLOSSY_SOURCE_AUDIT] material record allocation failed"
                    << std::endl;
                return failAuditInitialization();
            }
        }
    }

    // Constant buffers — need UniformBuffer bind flag for UBO descriptors.
    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    for (u32 i = 0; i < 3; ++i) {
        probe_cb_[i] = device->CreateBuffer(cbDesc);
        // T4.4: far-volume params (origin+spacing, counts).
        far_cb_[i] = device->CreateBuffer(cbDesc);
        // T6.3 sub-B: N-volume params (144B used in a 256B slot).
        nvolumes_cb_[i] = device->CreateBuffer(cbDesc);
    }

    BufferDesc spCbDesc{};
    spCbDesc.size = 512;
    spCbDesc.type = BufferType::Constant;
    spCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    spCbDesc.usage = GPUMemoryUsage::Dynamic;
    spCbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    for (u32 i = 0; i < 3; ++i)
        static_probe_cb_[i] = device->CreateBuffer(spCbDesc);

    // Dummy buffer for missing static probe bindings — used as SSBO.
    BufferDesc dummyDesc{};
    dummyDesc.size = 64;
    dummyDesc.type = BufferType::Structured;
    dummyDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    dummyDesc.usage = GPUMemoryUsage::Dynamic;
    dummyDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    dummy_buffer_ = device->CreateBuffer(dummyDesc);

    return true;
}

void GIGatherModule::Shutdown() {
    if (device_) for (auto& s : gather_audit_sources_) {
        if (s.staging!=handles::INVALID_RESOURCE) device_->DestroyBuffer(s.staging);
        s={};
    }
    if (device_) {
        for (u32 i = 0; i < 3; ++i) {
            if (glossy_source_audit_sets_[i] != handles::INVALID_DESCRIPTOR_SET)
                device_->DestroyDescriptorSet(glossy_source_audit_sets_[i]);
            if (glossy_source_audit_buffer_[i] != handles::INVALID_RESOURCE)
                device_->DestroyBuffer(glossy_source_audit_buffer_[i]);
            if (glossy_material_audit_buffer_[i] != handles::INVALID_RESOURCE)
                device_->DestroyBuffer(glossy_material_audit_buffer_[i]);
            glossy_source_audit_sets_[i] = handles::INVALID_DESCRIPTOR_SET;
            glossy_source_audit_buffer_[i] = handles::INVALID_RESOURCE;
            glossy_material_audit_buffer_[i] = handles::INVALID_RESOURCE;
        }
        if (glossy_source_audit_output_ != handles::INVALID_RESOURCE)
            device_->DestroyTexture(glossy_source_audit_output_);
        if (glossy_source_audit_specular_ != handles::INVALID_RESOURCE)
            device_->DestroyTexture(glossy_source_audit_specular_);
        if (glossy_source_audit_pipeline_ != handles::INVALID_PIPELINE)
            device_->DestroyPipeline(glossy_source_audit_pipeline_);
        if (glossy_source_audit_layout_ != handles::INVALID_PIPELINE_LAYOUT)
            device_->DestroyPipelineLayout(glossy_source_audit_layout_);
        if (glossy_source_audit_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
            device_->DestroyDescriptorSetLayout(glossy_source_audit_set_layout_);
    }
    glossy_source_audit_output_ = handles::INVALID_RESOURCE;
    glossy_source_audit_specular_ = handles::INVALID_RESOURCE;
    glossy_source_audit_pipeline_ = handles::INVALID_PIPELINE;
    glossy_source_audit_layout_ = handles::INVALID_PIPELINE_LAYOUT;
    glossy_source_audit_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    glossy_source_audit_bytes_ = 0;
    glossy_material_audit_bytes_ = 0;
    glossy_source_audit_enabled_ = false;
    gather_audit_orm_resource_ = handles::INVALID_RESOURCE;
    gather_audit_orm_runtime_format_ = DataFormat::Unknown;
    gather_audit_orm_width_ = gather_audit_orm_height_ = 0;
    gather_audit_orm_metadata_valid_ = false;
    gather_audit_orm_from_scene_ = false;
    gather_audit_queued_=gather_audit_dumped_=false;
}

GIGatherOutputs GIGatherModule::AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs) {
    GIGatherOutputs outputs{};
    if (!inputs.ddgi_pass || pipeline_ == handles::INVALID_PIPELINE) return outputs;

    // One-shot, opt-in snapshot of the actual consumer inputs, read only once
    // this completed frame slot is reused. No diagnostic mid-frame submit.
    const char* auditPrefix=std::getenv("DDGI_GATHER_AUDIT");
    if (auditPrefix && auditPrefix[0] && gather_audit_queued_ && !gather_audit_dumped_ &&
        inputs.frame_index>=gather_audit_frame_+3u &&
        inputs.current_buffer_index%3==gather_audit_slot_) {
        // Diagnostic-only queue completion certificate. The captured copies
        // were submitted on this graphics queue in the older frame. Submit a
        // checked host dependency after them, wait its fence, then map. Do not
        // infer completion merely from a reused swapchain/triple-buffer slot.
        const auto completionHandle=device_->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* completion=rhi::GetCommandBuffer(completionHandle);
        auto requireAudit=[&](bool ok,const char* operation) {
            if (!ok) {
                PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_GATHER_AUDIT] failed " << operation;
                std::abort(); // Explicit opt-in diagnostic failure; never export stale data.
            }
        };
        requireAudit(completion!=nullptr,"completion command");
        requireAudit(completion->Begin(),"Begin");
        completion->MemoryBarrier(PipelineStage::Transfer | PipelineStage::ComputeShader,
            PipelineStage::Host,AccessFlag::TransferWrite | AccessFlag::ShaderWrite,AccessFlag::HostRead);
        requireAudit(completion->End(),"End");
        requireAudit(completion->Submit(),"Submit");
        requireAudit(completion->WaitForCompletion(),"WaitForCompletion");
        device_->DestroyCommandBuffer(completionHandle);
        // The material record is only certifiable together with the exact ORM
        // imageView selected for descriptor binding 13 at the captured
        // dispatch.  A fallback is recorded explicitly so replay cannot call
        // it a scene ORM source.
        bool complete=gather_audit_orm_metadata_valid_;
        for (u32 i=0;i<gather_audit_source_count_;++i) {
            const auto& s=gather_audit_sources_[i];
            if (s.staging==handles::INVALID_RESOURCE) { complete=false;continue; }
            auto* mapped=device_->MapBuffer(s.staging,0,s.bytes);
            if (!mapped) { complete=false;continue; }
            const std::string path=std::string(auditPrefix)+"-"+s.name+".raw";
            if (FILE* out=std::fopen(path.c_str(),"wb")) {
                complete &= std::fwrite(mapped,1,s.bytes,out)==s.bytes;
                std::fclose(out);
            } else complete=false;
            device_->UnmapBuffer(s.staging);
        }
        const std::string path=std::string(auditPrefix)+"-meta.json";
        if (FILE* out=std::fopen(path.c_str(),"w")) {
            const auto glossySourceResource = glossy_source_audit_enabled_
                ? glossy_source_audit_buffer_[gather_audit_slot_]
                : handles::INVALID_RESOURCE;
            const auto materialSourceResource = glossy_source_audit_enabled_
                ? glossy_material_audit_buffer_[gather_audit_slot_]
                : handles::INVALID_RESOURCE;
            std::fprintf(out,"{\"frame\":%u,\"read_frame\":%u,\"ddgi_frame\":%u,\"ray_sample_mode\":%u,\"rays_per_probe\":%u,\"width\":%u,\"height\":%u,\"complete\":%s,\"glossy_source_audit\":{\"enabled\":%s,\"records_per_pixel\":9,\"bytes\":%llu,\"resource_handle\":%llu,\"binding\":21,\"auxiliary_scope\":\"same_inputs_shadow_dispatch\",\"default_main_preserved\":true,\"comparison_policy\":\"aux_delta_only; mismatch_unknown; never_substitute\"},\"material_source_audit\":{\"enabled\":%s,\"schema\":1,\"records_per_pixel\":2,\"bytes\":%llu,\"resource_handle\":%llu,\"binding\":22,\"layout\":\"storage_buffer\",\"format\":\"vec4x2_f32\",\"runtime_format\":%u,\"record_format\":\"vec4x2_f32\",\"record_runtime_format\":0,\"record_width\":%u,\"record_height\":%u,\"orm_width\":%u,\"orm_height\":%u,\"orm_descriptor_handle\":%llu,\"orm_binding\":13,\"orm_layout\":\"shader_resource\",\"orm_format\":\"%s\",\"orm_runtime_format\":%u,\"orm_selection\":\"%s\",\"source_identity\":\"DDGI_GLOSSY_SOURCE_AUDIT material records\",\"auxiliary_scope\":\"same_inputs_shadow_dispatch\"},\"sources\":[",
                gather_audit_frame_,inputs.frame_index,gather_audit_ddgi_frame_,gather_audit_ray_mode_,gather_audit_rays_,gather_width_,gather_height_,complete?"true":"false",
                glossy_source_audit_enabled_?"true":"false",
                static_cast<unsigned long long>(glossy_source_audit_bytes_),
                static_cast<unsigned long long>(glossySourceResource),
                materialSourceResource!=handles::INVALID_RESOURCE?"true":"false",
                static_cast<unsigned long long>(glossy_material_audit_bytes_),
                static_cast<unsigned long long>(materialSourceResource),
                0u,
                gather_width_,gather_height_,gather_audit_orm_width_,gather_audit_orm_height_,
                static_cast<unsigned long long>(gather_audit_orm_resource_),
                GatherAuditFormatName(gather_audit_orm_runtime_format_),
                static_cast<u32>(gather_audit_orm_runtime_format_),
                gather_audit_orm_from_scene_?"scene_gbuffer_orm":"fallback_descriptor");
            for (u32 i=0;i<gather_audit_source_count_;++i) {
                const auto& s=gather_audit_sources_[i];
                std::fprintf(out,"%s{\"name\":\"%s\",\"format\":\"%s\",\"runtime_format\":%u,\"bytes\":%llu,\"width\":%u,\"height\":%u,\"live\":%s,\"resource_handle\":%llu,\"binding\":%u,\"layout\":\"%s\",\"source_identity\":\"%s\"}",
                    i?",":"",s.name,s.format,s.runtime_format,static_cast<unsigned long long>(s.bytes),s.width,s.height,
                    s.staging!=handles::INVALID_RESOURCE?"true":"false",
                    static_cast<unsigned long long>(s.resource),s.binding,s.layout,s.source_identity);
            }
            std::fprintf(out,"],\"completion_checked\":true,\"begin_end_submit_checked\":true,\"explicit_host_dependency\":true,\"completion_scope\":\"checked_graphics_queue_command_after_snapshot_copies\"}\n");std::fclose(out);
        } else complete=false;
        gather_audit_dumped_=true;
        PRIMAL_LOG_STREAM(Error,"Render") << "[DDGI_GATHER_AUDIT] frame=" << gather_audit_frame_
            << " read_frame=" << inputs.frame_index << " complete=" << complete << std::endl;
    }

    outputs.gi_output_tex = gi_halfres_texture_;
    outputs.gi_output_rg = graph.ImportResource("DDGIHalfResGI", gi_halfres_texture_);
    outputs.gi_specular_tex = gi_specular_texture_;
    outputs.gi_specular_rg = graph.ImportResource("DDGIHalfResGlossy", gi_specular_texture_);

    // Read the slot DDGI trace + SC-DDGI integration wrote this frame
    // (currentIndex — RG ordering guarantees the writes land before this
    // read). Phase 1d: 槽位单一事实源 DDGISlotMap.h；frame_index 缺省时
    // 回退旧 cbIdx 时钟（仅未迁移的调用方触达）。
    u32 ddgiReadIdx = (inputs.frame_index != GIGatherInputs::kFrameIndexUnset)
                          ? lumen::DDGISlotsForFrame(inputs.frame_index).currentIndex
                          : ((inputs.current_buffer_index + 2) % 3);

    // Import DDGI buffers
    auto irrHandle = graph.ImportResource("DDGIIrrRead", inputs.ddgi_pass->GetIrradianceBufferBySlot(ddgiReadIdx));
    auto depthHandle = graph.ImportResource("DDGIDepthRead", inputs.ddgi_pass->GetDepthBufferBySlot(ddgiReadIdx));
    // T4.4: far volume read dependency (ordering vs. its trace pass).
    if (inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
        graph.ImportResource("DDGIFarIrrRead", inputs.ddgi_far_pass->GetIrradianceBufferBySlot(ddgiReadIdx));
    }
    // T6.3 sub-B: 附加 volume（索引 ≥2）的读依赖注册——与 far import 同模式，
    // 把第 3/4 个 volume 的 irradiance 纳入本帧图（别名注册 + 与其 trace pass
    // 的次序约束）。volume[0]/[1] 已由上方 near/far import 覆盖。
    // T3.6/D1: flat 路径不再依赖 descriptor indexing——所有有效 volume 都
    // 参与打包，import 不再受能力位门控。
    if (inputs.volume_count > 2) {
        for (u32 vi = 2; vi < inputs.volume_count && vi < GIGatherInputs::kMaxVolumes; ++vi) {
            if (inputs.volume_views[vi].IsValid())
                graph.ImportResource("DDGINvolIrrRead" + std::to_string(vi),
                                     inputs.volume_views[vi].irradiance);
        }
    }

    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    // The auxiliary source shader writes only shadow outputs.  Import them
    // into the same pass so the render graph owns their UAV/SRV transitions;
    // they never replace the production gather outputs consumed downstream.
    const auto glossyAuditOutputRG = glossy_source_audit_enabled_
        ? graph.ImportResource("DDGIGlossySourceAuditOutput", glossy_source_audit_output_)
        : rendergraph::RGResourceHandle{};
    const auto glossyAuditSpecRG = glossy_source_audit_enabled_
        ? graph.ImportResource("DDGIGlossySourceAuditSpecular", glossy_source_audit_specular_)
        : rendergraph::RGResourceHandle{};

    graph.AddPass<PassData>("DDGIGIGather",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [outRG = outputs.gi_output_rg, specRG = outputs.gi_specular_rg,
         glossyAuditOutputRG, glossyAuditSpecRG,
         irrHandle, depthHandle,
         depthRG = inputs.gbuffer_depth_rg, normalRG = inputs.gbuffer_normal_rg, ormRG = inputs.gbuffer_orm_rg]
        (PassData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling — outputs go to imported textures
            builder.Read(irrHandle, ResourceState::ShaderResource);
            builder.Read(depthHandle, ResourceState::ShaderResource);
            if (depthRG.IsValid()) builder.Read(depthRG, ResourceState::ShaderResource);
            if (normalRG.IsValid()) builder.Read(normalRG, ResourceState::ShaderResource);
            if (ormRG.IsValid()) builder.Read(ormRG, ResourceState::ShaderResource);
            data.output = builder.Write(outRG, ResourceState::UnorderedAccess);
            builder.Write(specRG, ResourceState::UnorderedAccess);
            if (glossyAuditOutputRG.IsValid())
                builder.Write(glossyAuditOutputRG, ResourceState::UnorderedAccess);
            if (glossyAuditSpecRG.IsValid())
                builder.Write(glossyAuditSpecRG, ResourceState::UnorderedAccess);
        },
        [this, inputs, ddgiReadIdx](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            auto ddgi = inputs.ddgi_pass;
            const u32 bindingIdx = inputs.current_buffer_index % 3;
            const DescriptorSetHandle descriptorSet = descriptor_sets_[bindingIdx];
            const ResourceHandle probeCB = probe_cb_[bindingIdx];
            const ResourceHandle staticProbeCB = static_probe_cb_[bindingIdx];
            const ResourceHandle nvolumesCB = nvolumes_cb_[bindingIdx];
            const char* gatherPrefix = std::getenv("DDGI_GATHER_AUDIT");
            const u32 auditFrame = std::getenv("DDGI_GATHER_FRAME")
                ? u32(std::max(0, std::atoi(std::getenv("DDGI_GATHER_FRAME"))))
                : 350u;
            const bool glossyAuditDispatch = glossy_source_audit_enabled_ &&
                gatherPrefix && gatherPrefix[0] && inputs.frame_index == auditFrame;
            const DescriptorSetHandle glossyAuditSet = glossy_source_audit_sets_[bindingIdx];
            // This is populated from imgInfos[4].imageView below.  The
            // deferred material metadata must describe the descriptor that
            // actually reached the auxiliary shader, including fallback use.
            ResourceHandle actualBoundORM = handles::INVALID_RESOURCE;

            // X2/A3: 远场 volume 数据优先走 view；view 未填时回退旧 pass 指针
            // 路径。两条来源字段一致（origin/counts/spacing/irradiance 同源同
            // 槽位），CB 上传与 barrier/descriptor 取数逐字节等价。
            lumen::DDGIVolumeView farView = inputs.far_volume_view;
            if (!farView.IsValid() && inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
                farView = inputs.ddgi_far_pass->GetVolumeViewBySlot(ddgiReadIdx);
            }

            // T3.6/D1: 组装 flat volume 表。inputs.volume_views 优先（管线侧把
            // ddgi_volumes_[0..count) 全量传入）；[0]/[1] 缺省时回退 near
            // pass / farView——与旧 near/far 槽同源同槽位。flatActive 在
            // ≥1 个有效 volume 时为真（near 恒在），不再有 descriptor
            // indexing 能力门控——Metal/Vulkan 同一条 flat 路径。
            lumen::DDGIVolumeView nvolViews[GIGatherInputs::kMaxVolumes]{};
            u32 nvolCount = 0;
            {
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi) {
                    if (vi < inputs.volume_count && inputs.volume_views[vi].IsValid())
                        nvolViews[vi] = inputs.volume_views[vi];
                }
                if (!nvolViews[0].IsValid())
                    nvolViews[0] = ddgi->GetVolumeViewBySlot(ddgiReadIdx);
                if (!nvolViews[1].IsValid() && farView.IsValid())
                    nvolViews[1] = farView;
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi)
                    if (nvolViews[vi].IsValid()) nvolCount = vi + 1;
            }
            const bool flatActive = nvolCount >= 1;
            // 路径选择一次性日志（SDFViz 先例）。
            static bool loggedGatherPath = false;
            if (!loggedGatherPath) {
                loggedGatherPath = true;
                PRIMAL_LOG_STREAM(Error, "Render") << "[GIGather] path = flat-buffer (volumes="
                          << nvolCount << ")" << std::endl;
            }

            // T3.6/D1: flat 布局计算——probeOffset 累计 + 按需增长 flat SSBO。
            // [header 256B][vol0: probes×9×vec4][vol1]...；描述符绑 offset 256
            // 跳过头部，shader 侧即纯探针数组（256 满足所有后端的 SSBO
            // offset 对齐）。头部 16B 有效内容：totalProbes, volumeCount。
            u32 fvOffsets[GIGatherInputs::kMaxVolumes] = {};
            u32 fvTotalProbes = 0;
            ResourceHandle flatBuf = handles::INVALID_RESOURCE;
            if (flatActive) {
                constexpr u64 kFlatHeaderBytes = 256;
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    fvOffsets[vi] = fvTotalProbes;
                    const auto& v = nvolViews[vi];
                    fvTotalProbes += (u32)v.counts.x * (u32)v.counts.y * (u32)v.counts.z;
                }
                const u64 needed = kFlatHeaderBytes + (u64)fvTotalProbes * 144ull;
                if (flat_irr_buffer_[bindingIdx] == handles::INVALID_RESOURCE ||
                    flat_irr_capacity_[bindingIdx] < needed) {
                    // 按需增长（三槽一起换，容量一致）。实际只在首帧或
                    // volume 数变化时触发。
                    for (u32 s = 0; s < 3; ++s) {
                        if (flat_irr_buffer_[s] != handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(flat_irr_buffer_[s]);
                        BufferDesc fbDesc{};
                        fbDesc.size = needed;
                        fbDesc.type = BufferType::Structured;
                        fbDesc.usage = GPUMemoryUsage::Dynamic;
                        fbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        fbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                           static_cast<u32>(BufferUsageFlags::TransferDst);
                        fbDesc.name = "GIGather_FlatIrr";
                        flat_irr_buffer_[s] = device_->CreateBuffer(fbDesc);
                        flat_irr_capacity_[s] = needed;
                    }
                }
                flatBuf = flat_irr_buffer_[bindingIdx];
                // 头部（host 写，立即驻留；后续 CopyBuffer 只写 [256, ...)）。
                auto* hdr = static_cast<u32*>(device_->MapBuffer(flatBuf));
                if (hdr) {
                    hdr[0] = fvTotalProbes;
                    hdr[1] = nvolCount;
                    hdr[2] = 0;
                    hdr[3] = 0;
                    device_->UnmapBuffer(flatBuf);
                }
                // GI 根治 Phase 3.2 + 能量所有权 Phase 4a: flat openSky 缓冲
                // （256B 头 + probes×8B，float2/探针 = {escapeRatio, sER}），
                // 同槽位按需增长。
                const u64 neededSky = kFlatHeaderBytes + (u64)fvTotalProbes * 8ull;
                if (flat_opensky_buffer_[bindingIdx] == handles::INVALID_RESOURCE ||
                    flat_opensky_capacity_[bindingIdx] < neededSky) {
                    for (u32 s2 = 0; s2 < 3; ++s2) {
                        if (flat_opensky_buffer_[s2] != handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(flat_opensky_buffer_[s2]);
                        BufferDesc osDesc{};
                        osDesc.size = neededSky;
                        osDesc.type = BufferType::Structured;
                        osDesc.usage = GPUMemoryUsage::Dynamic;
                        osDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        osDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage) |
                                           static_cast<u32>(BufferUsageFlags::TransferDst);
                        osDesc.name = "GIGather_FlatOpenSky";
                        flat_opensky_buffer_[s2] = device_->CreateBuffer(osDesc);
                        flat_opensky_capacity_[s2] = neededSky;
                    }
                }
            }

            // Upload GIGatherCB
            {
                math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
                math::m4x4 invView = Inverse(inputs.view_matrix);
                auto* cb = static_cast<GIGatherCB*>(device_->MapBuffer(probeCB));
                if (cb) {
                    cb->inv_view_projection = Inverse(vp);
                    const auto& vol = ddgi->GetVolumeData();
                    const auto& params = ddgi->GetParams();
                    cb->probe_origin_spacing = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, params.probe_spacing};
                    const u32 auditFrameStamp =
                        inputs.frame_index == GIGatherInputs::kFrameIndexUnset
                            ? 0u : inputs.frame_index + 1u;
                    cb->probe_counts = math::v4{(float)params.probe_count_x, (float)params.probe_count_y, (float)params.probe_count_z,
                        glossy_source_audit_enabled_ ? static_cast<float>(auditFrameStamp) : 9.0f};
                    cb->camera_pos = math::v4{invView.columns[3][0], invView.columns[3][1], invView.columns[3][2], 1.0f};
                    // T6.3 sub-A: probe_vis 数据面——GIDebug() 热调旋钮进 CB，
                    // shader binding 6 第三 vec4（offset 112）。Off（默认）时
                    // x=0，gather 输出零变化。
                    cb->debug_vis = math::v4{static_cast<float>(
                        static_cast<u32>(lumen::GIDebug().probe_vis)), 0.0f, 0.0f, 0.0f};
                    // Opt-in scene-floor oracle, NOT a production visibility
                    // heuristic. The patch must be independently certified from
                    // loaded triangles. Only dual-policy mode5 may consume it.
                    const auto& depthPolicy=ddgi->GetVolumeData();
                    if (device_->GetPlatform()==RHIPlatform::Vulkan && depthPolicy.RaySampleMode==5 && depthPolicy.RaysPerProbe==128 &&
                        depthPolicy.SdfVoxelSizes[0].y==0.f && depthPolicy.SdfVoxelSizes[1].w==3.f) {
                        if (const char* e=std::getenv("DDGI_GATHER_FLOOR_PATCH")) {
                            float plane=0,halfX=0,halfZ=0; char trailing=0;
                            if (std::sscanf(e,"%f,%f,%f%c",&plane,&halfX,&halfZ,&trailing)==3 &&
                                std::isfinite(plane) && std::isfinite(halfX) && std::isfinite(halfZ) && halfX>0 && halfZ>0) {
                                cb->debug_vis.y=halfX; cb->debug_vis.z=halfZ; cb->debug_vis.w=plane;
                            }
                        }
                    }
                    // TEMP diagnosis: verify the CB the gather will read.
                    static bool s_cbDbg = false;
                    if (!s_cbDbg) { s_cbDbg = true;
                        PRIMAL_LOG_STREAM(Error, "Render") << "[GATHER_CB] origin_spacing=("
                                  << cb->probe_origin_spacing.x << "," << cb->probe_origin_spacing.y
                                  << "," << cb->probe_origin_spacing.z << "," << cb->probe_origin_spacing.w
                                  << ") counts=(" << cb->probe_counts.x << "," << cb->probe_counts.y
                                  << "," << cb->probe_counts.z << "," << cb->probe_counts.w
                                  << ") cam=(" << cb->camera_pos.x << "," << cb->camera_pos.y
                                  << "," << cb->camera_pos.z << ")" << std::endl; }
                    device_->UnmapBuffer(probeCB);
                }
            }

            // T3.6/D1: far 双槽 CB 上传删除（far volume 数据经 flat SSBO +
            // FlatVolumeCB 到达 shader；far_cb_ 成员保留但不再绑定）。

            // GI 根治 Phase 3.2: openSky 分类开关（PRIMAL_GI_OPENSKY=0 回滚），
            // 供 FlatVolumeCB.meta.y 与下方 flat 装配共用。
            bool openskyActive = flatActive;
            if (const char* e = std::getenv("PRIMAL_GI_OPENSKY")) {
                if (e[0] == '0') openskyActive = false;
            }
            // 能量所有权 Phase 4a: openSky V2 开关（meta.z）——E_geo/E_sky
            // 拆分消费；PRIMAL_GI_OPENSKY_V2=0 回退旧整探针分类（sER=1）。
            bool openskyV2 = true;
            if (const char* e = std::getenv("PRIMAL_GI_OPENSKY_V2")) {
                if (e[0] == '0') openskyV2 = false;
            }
            // Diagnostic/quality control for the visibility-free trilinear
            // component.  Keep -1 as the default so this instrumentation does
            // not silently alter either backend's established image.
            float lowFrequencyBlend = -1.0f;
            if (const char* e = std::getenv("PRIMAL_DDGI_LOW_FREQ_BLEND")) {
                char* end = nullptr;
                const float parsed = std::strtof(e, &end);
                if (end != e && std::isfinite(parsed))
                    lowFrequencyBlend = std::clamp(parsed, 0.0f, 1.0f);
            }
            // T6.3 sub-B → T3.6/D1: upload FlatVolumeCB（binding 18）。flatActive
            // 时写入 volume[0..nvolCount) 的 origin/spacing/counts + probe
            // offset（w 槽，原 NVolumeCB 的 enabled 位改型）；否则整块清零
            // （meta.x=0 → shader 输出 dim ambient 防御路径）。无效中段槽位
            // （spacing=0）被 shader 选择器跳过。
            {
                auto* nv = static_cast<FlatVolumeCB*>(device_->MapBuffer(nvolumesCB));
                if (nv) {
                    memset(nv, 0, sizeof(FlatVolumeCB));
                    if (flatActive) {
                        for (u32 vi = 0; vi < nvolCount; ++vi) {
                            const auto& v = nvolViews[vi];
                            nv->origin_spacing[vi] = math::v4{v.origin.x, v.origin.y, v.origin.z, v.spacing};
                            nv->counts_offset[vi] = math::v4{v.counts.x, v.counts.y, v.counts.z,
                                                             (float)fvOffsets[vi]};
                        }
                        // y = openSky 分类开关（Phase 3.2，实际生效还要求
                        // openskyActive——volume 视图缺 opensky 时 shader 侧
                        // 由 dummy 兜底但开关同时关掉）。
                        // z = openSky V2 开关（Phase 4a；PRIMAL_GI_OPENSKY_V2=0
                        // 回滚旧整探针分类——shader 侧 sER=1 等价旧行为）。
                        nv->meta = math::v4{(float)nvolCount,
                                            openskyActive ? 1.0f : 0.0f,
                                            openskyV2 ? 1.0f : 0.0f,
                                            lowFrequencyBlend};
                    }
                    device_->UnmapBuffer(nvolumesCB);
                }
            }

            // Upload static probe params
            {
                auto* sp = static_cast<GPUStaticProbeData*>(device_->MapBuffer(staticProbeCB));
                if (sp) {
                    memset(sp, 0, sizeof(GPUStaticProbeData));
                    if (inputs.static_probe_volume) {
                        const auto& vol = ddgi->GetVolumeData();
                        sp->ProbeOrigin = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, 0.0f};
                        sp->ProbeSpacing = ddgi->GetParams().probe_spacing;
                    }
                    device_->UnmapBuffer(staticProbeCB);
                }
            }

            // Ensure history texture is in SRV layout (binding 3 placeholder).
            {
                ResourceBarrier histBar{};
                histBar.resource = gi_halfres_history_;
                histBar.beforeState = ResourceState::ShaderResource;
                histBar.afterState = ResourceState::ShaderResource;
                histBar.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&histBar, 1);
            }

            // Barrier DDGI buffers from UAV to ShaderResource
            {
                // T3.6/D1: 只保留仍以 SRV 直接绑定的近场 irr/depth/conf。
                // far/附加 volume 的 irradiance 改为 flat 打包的 CopyBuffer
                // 源（同 CB 内队列序先于 dispatch，无需 barrier——Vulkan 端
                // buffer barrier 本就是 no-op，Metal 无显式 buffer 布局）。
                ResourceBarrier barriers[4]{};
                int bc = 0;
                auto irrBuf = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx);
                auto depthBuf = ddgi->GetDepthBufferBySlot(ddgiReadIdx);
                auto confBuf = ddgi->GetConfidenceBuffer(ddgiReadIdx);
                // 2026-09-17 P0：近场重定位起点与 irr/depth 同为本帧 trace 产物。
                auto roBuf = nvolViews[0].IsValid() ? nvolViews[0].relocated_origins
                                                    : handles::INVALID_RESOURCE;
                if (irrBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = irrBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (depthBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = depthBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (confBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = confBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (roBuf != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = roBuf;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                if (bc > 0) cmd->InsertBarrier(barriers, bc);
            }

            // T3.6/D1: flat 打包 copy——各 volume irradiance → flat SSBO。
            // 编码于同一 CB、先于 dispatch（队列序即依赖）。bindingIdx 槽与
            // 描述符一致，头部已由上方 host 写驻留。
            ResourceHandle flatOpenSkyBuf = handles::INVALID_RESOURCE;
            if (flatActive && flatBuf != handles::INVALID_RESOURCE) {
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    const auto& v = nvolViews[vi];
                    if (v.irradiance == handles::INVALID_RESOURCE) continue;
                    const u64 probes = (u64)v.counts.x * (u64)v.counts.y * (u64)v.counts.z;
                    if (probes == 0) continue;
                    cmd->CopyBuffer(v.irradiance, flatBuf,
                                    0, 256ull + (u64)fvOffsets[vi] * 144ull,
                                    probes * 144ull);
                }
            }
            // GI 根治 Phase 3.2: flat openSky 装配（volume 顺序与 irr 一致）。
            if (openskyActive && flat_opensky_buffer_[bindingIdx] != handles::INVALID_RESOURCE) {
                flatOpenSkyBuf = flat_opensky_buffer_[bindingIdx];
                for (u32 vi = 0; vi < nvolCount; ++vi) {
                    const auto& v = nvolViews[vi];
                    if (v.opensky == handles::INVALID_RESOURCE) { openskyActive = false; break; }
                    const u64 probes = (u64)v.counts.x * (u64)v.counts.y * (u64)v.counts.z;
                    if (probes == 0) continue;
                    cmd->CopyBuffer(v.opensky, flatOpenSkyBuf,
                                    0, 256ull + (u64)fvOffsets[vi] * 8ull,
                                    probes * 8ull);
                }
            }

            // Update descriptor set
            ResourceHandle skySH = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkySH() : handles::INVALID_RESOURCE;
            ResourceHandle skyFactor = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkyFactor() : handles::INVALID_RESOURCE;

            // Texture bindings (6: depth/normal/output/history/orm/specular)
            WriteDescriptorSet texWrites[6] = {};
            DescriptorImageInfo imgInfos[6];
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 ormBinding = isVk ? 13u : 4u;
            const u32 specBinding = isVk ? 14u : 5u;
            {
                // texture(0): depth (SampledDepthImage on Vulkan/Dawn, SampledImage on Metal)
                texWrites[0].dstSet = descriptorSet;
                texWrites[0].dstBinding = 0;
                texWrites[0].dstArrayElement = 0;
                texWrites[0].descriptorCount = 1;
                texWrites[0].descriptorType = isVk ? DescriptorType::SampledDepthImage : DescriptorType::SampledImage;
                texWrites[0].imageInfo = &imgInfos[0];
                imgInfos[0].sampler = handles::INVALID_SAMPLER;
                imgInfos[0].imageView = inputs.gbuffer_depth;
                imgInfos[0].imageLayout = ResourceState::ShaderResource;
                // texture(1): normal
                texWrites[1].dstSet = descriptorSet;
                texWrites[1].dstBinding = 1;
                texWrites[1].dstArrayElement = 0;
                texWrites[1].descriptorCount = 1;
                texWrites[1].descriptorType = DescriptorType::SampledImage;
                texWrites[1].imageInfo = &imgInfos[1];
                imgInfos[1].sampler = handles::INVALID_SAMPLER;
                imgInfos[1].imageView = inputs.gbuffer_normal;
                imgInfos[1].imageLayout = ResourceState::ShaderResource;
                // texture(2): output (UAV)
                texWrites[2].dstSet = descriptorSet;
                texWrites[2].dstBinding = 2;
                texWrites[2].dstArrayElement = 0;
                texWrites[2].descriptorCount = 1;
                texWrites[2].descriptorType = DescriptorType::StorageImage;
                texWrites[2].imageInfo = &imgInfos[2];
                imgInfos[2].sampler = handles::INVALID_SAMPLER;
                imgInfos[2].imageView = gi_halfres_texture_;
                imgInfos[2].imageLayout = ResourceState::UnorderedAccess;
                // texture(3): history
                texWrites[3].dstSet = descriptorSet;
                texWrites[3].dstBinding = 3;
                texWrites[3].dstArrayElement = 0;
                texWrites[3].descriptorCount = 1;
                texWrites[3].descriptorType = DescriptorType::SampledImage;
                texWrites[3].imageInfo = &imgInfos[3];
                imgInfos[3].sampler = handles::INVALID_SAMPLER;
                imgInfos[3].imageView = gi_halfres_history_; // binding 3 (unused, but must be valid)
                imgInfos[3].imageLayout = ResourceState::ShaderResource;
                // texture(4/13): gbuffer ORM (T4.3) — fallback keeps descriptor valid
                ResourceHandle ormTex = inputs.gbuffer_orm;
                if (ormTex == handles::INVALID_RESOURCE) ormTex = inputs.fallback_texture;
                if (ormTex == handles::INVALID_RESOURCE) ormTex = gi_halfres_history_;
                actualBoundORM = ormTex;
                texWrites[4].dstSet = descriptorSet;
                texWrites[4].dstBinding = ormBinding;
                texWrites[4].dstArrayElement = 0;
                texWrites[4].descriptorCount = 1;
                texWrites[4].descriptorType = DescriptorType::SampledImage;
                texWrites[4].imageInfo = &imgInfos[4];
                imgInfos[4].sampler = handles::INVALID_SAMPLER;
                imgInfos[4].imageView = ormTex;
                imgInfos[4].imageLayout = ResourceState::ShaderResource;
                // texture(5/14): glossy specular output (T4.3, UAV)
                texWrites[5].dstSet = descriptorSet;
                texWrites[5].dstBinding = specBinding;
                texWrites[5].dstArrayElement = 0;
                texWrites[5].descriptorCount = 1;
                texWrites[5].descriptorType = DescriptorType::StorageImage;
                texWrites[5].imageInfo = &imgInfos[5];
                imgInfos[5].sampler = handles::INVALID_SAMPLER;
                imgInfos[5].imageView = gi_specular_texture_;
                imgInfos[5].imageLayout = ResourceState::UnorderedAccess;
                device_->UpdateDescriptorSets(6, texWrites);
                if (glossyAuditDispatch) {
                    // Reuse the exact input image descriptors, changing only
                    // the two write targets and descriptor set.  This keeps
                    // the auxiliary pass on the same receiver data.
                    for (u32 i = 0; i < 6; ++i)
                        texWrites[i].dstSet = glossyAuditSet;
                    imgInfos[2].imageView = glossy_source_audit_output_;
                    imgInfos[5].imageView = glossy_source_audit_specular_;
                    device_->UpdateDescriptorSets(6, texWrites);
                }
            }

            // T3.6/D1: Vulkan flat 绑定（17 = flat SSBO offset 256 跳过头部，
            // 18 = FlatVolumeCB）。原 15/16（far 双槽）与 17 array[4] 描述符
            // 写入删除。Metal 的 flat 槽（10/11）走下方 bufWrites 路径。
            if (isVk) {
                WriteDescriptorSet flatWrites[6] = {};
                DescriptorBufferInfo flatInfos[6];
                flatWrites[0].dstSet = descriptorSet;
                flatWrites[0].dstBinding = 17;
                flatWrites[0].dstArrayElement = 0;
                flatWrites[0].descriptorCount = 1;
                flatWrites[0].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[0].imageInfo = nullptr;
                flatWrites[0].bufferInfo = &flatInfos[0];
                flatInfos[0].buffer = (flatActive && flatBuf != handles::INVALID_RESOURCE)
                    ? flatBuf : dummy_buffer_;
                flatInfos[0].offset = 256;   // 跳过头部 → 纯探针数组
                flatInfos[0].range = ~0ull;
                flatWrites[1].dstSet = descriptorSet;
                flatWrites[1].dstBinding = 18;
                flatWrites[1].dstArrayElement = 0;
                flatWrites[1].descriptorCount = 1;
                flatWrites[1].descriptorType = DescriptorType::UniformBuffer;
                flatWrites[1].imageInfo = nullptr;
                flatWrites[1].bufferInfo = &flatInfos[1];
                flatInfos[1].buffer = nvolumesCB; flatInfos[1].offset = 0;
                flatInfos[1].range = sizeof(FlatVolumeCB);
                // 2026-09-17 P0：binding 19 = 近场重定位有效起点（INVALID 时绑
                // dummy，shader 侧 w=0 回退逻辑格点）。
                flatWrites[2].dstSet = descriptorSet;
                flatWrites[2].dstBinding = 19;
                flatWrites[2].dstArrayElement = 0;
                flatWrites[2].descriptorCount = 1;
                flatWrites[2].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[2].imageInfo = nullptr;
                flatWrites[2].bufferInfo = &flatInfos[2];
                {
                    ResourceHandle nearRo = nvolViews[0].IsValid()
                        ? nvolViews[0].relocated_origins : handles::INVALID_RESOURCE;
                    flatInfos[2].buffer = (nearRo != handles::INVALID_RESOURCE)
                        ? nearRo : dummy_buffer_;
                }
                flatInfos[2].offset = 0;
                flatInfos[2].range = ~0ull;
                // GI 根治 Phase 3.2: binding 20 = flat openSky（关闭时绑 dummy，
                // fvMeta.y=0 让 shader 跳过读取）。
                flatWrites[3].dstSet = descriptorSet;
                flatWrites[3].dstBinding = 20;
                flatWrites[3].dstArrayElement = 0;
                flatWrites[3].descriptorCount = 1;
                flatWrites[3].descriptorType = DescriptorType::StorageBuffer;
                flatWrites[3].imageInfo = nullptr;
                flatWrites[3].bufferInfo = &flatInfos[3];
                flatInfos[3].buffer = (flatOpenSkyBuf != handles::INVALID_RESOURCE)
                    ? flatOpenSkyBuf : dummy_buffer_;
                // 与 flatIrradiance 同款：offset 256 跳过头部（shader 侧索引
                // 即纯探针数组）。offset 0 会让 shader 读到头部整型位模式的
                // ≈0 浮点，分类静默失效。
                flatInfos[3].offset = 256;
                flatInfos[3].range = ~0ull;
                device_->UpdateDescriptorSets(4, flatWrites);
                if (glossyAuditDispatch) {
                    for (u32 i = 0; i < 4; ++i)
                        flatWrites[i].dstSet = glossyAuditSet;
                    flatWrites[4].dstSet = glossyAuditSet;
                    flatWrites[4].dstBinding = 21;
                    flatWrites[4].dstArrayElement = 0;
                    flatWrites[4].descriptorCount = 1;
                    flatWrites[4].descriptorType = DescriptorType::StorageBuffer;
                    flatWrites[4].imageInfo = nullptr;
                    flatWrites[4].bufferInfo = &flatInfos[4];
                    flatInfos[4].buffer = glossy_source_audit_buffer_[bindingIdx];
                    flatInfos[4].offset = 0;
                    flatInfos[4].range = glossy_source_audit_bytes_;
                    flatWrites[5].dstSet = glossyAuditSet;
                    flatWrites[5].dstBinding = 22;
                    flatWrites[5].dstArrayElement = 0;
                    flatWrites[5].descriptorCount = 1;
                    flatWrites[5].descriptorType = DescriptorType::StorageBuffer;
                    flatWrites[5].imageInfo = nullptr;
                    flatWrites[5].bufferInfo = &flatInfos[5];
                    flatInfos[5].buffer = glossy_material_audit_buffer_[bindingIdx];
                    flatInfos[5].offset = 0;
                    flatInfos[5].range = glossy_material_audit_bytes_;
                    device_->UpdateDescriptorSets(6, flatWrites);
                }
            }

            // Buffer bindings with sub-allocation from probe_cb_
            WriteDescriptorSet bufWrites[10] = {};
            DescriptorBufferInfo bufInfos[10];
            {
                for (int i = 0; i < 10; ++i) {
                    bufWrites[i].dstSet = descriptorSet;
                    bufWrites[i].dstArrayElement = 0;
                    bufWrites[i].descriptorCount = 1;
                    bufWrites[i].descriptorType = DescriptorType::UniformBuffer;
                    bufWrites[i].imageInfo = nullptr;
                    bufWrites[i].bufferInfo = &bufInfos[i];
                }
                // Vulkan: flat bindings 4..12 (texture namespace 0..3 already used).
                // Metal: overlapping buffer namespace 0..8.
                const u32 bufBase = isVk ? 4u : 0u;
                bufWrites[0].dstBinding = bufBase + 0;
                bufInfos[0].buffer = probeCB; bufInfos[0].offset = 0; bufInfos[0].range = 80;
                bufWrites[1].dstBinding = bufBase + 1;
                bufInfos[1].buffer = probeCB; bufInfos[1].offset = 64; bufInfos[1].range = 16;
                bufWrites[2].dstBinding = bufBase + 2;
                bufInfos[2].buffer = probeCB; bufInfos[2].offset = 80; bufInfos[2].range = 48;  // T6.3 sub-A: +debugVis(48B)
                bufWrites[3].dstBinding = bufBase + 3; bufWrites[3].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[3].buffer = (skySH != handles::INVALID_RESOURCE) ? skySH : dummy_buffer_;
                bufInfos[3].offset = 0; bufInfos[3].range = ~0ull;
                bufWrites[4].dstBinding = bufBase + 4; bufWrites[4].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[4].buffer = (skyFactor != handles::INVALID_RESOURCE) ? skyFactor : dummy_buffer_;
                bufInfos[4].offset = 0; bufInfos[4].range = ~0ull;
                bufWrites[5].dstBinding = bufBase + 5;
                bufInfos[5].buffer = staticProbeCB; bufInfos[5].offset = 0; bufInfos[5].range = ~0ull;
                bufWrites[6].dstBinding = bufBase + 6; bufWrites[6].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[6].buffer = ddgi->GetConfidenceBuffer(ddgiReadIdx); bufInfos[6].offset = 0; bufInfos[6].range = ~0ull;
                bufWrites[7].dstBinding = bufBase + 7; bufWrites[7].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[7].buffer = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx); bufInfos[7].offset = 0; bufInfos[7].range = ~0ull;
                bufWrites[8].dstBinding = bufBase + 8; bufWrites[8].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[8].buffer = ddgi->GetDepthBufferBySlot(ddgiReadIdx); bufInfos[8].offset = 0; bufInfos[8].range = ~0ull;
                if (isVk) {
                    // Vulkan: bindings 4..12（flat 槽 17/18 已由上方 flatWrites 写入）。
                    device_->UpdateDescriptorSets(9, bufWrites);
                    if (glossyAuditDispatch) {
                        for (u32 i = 0; i < 9; ++i)
                            bufWrites[i].dstSet = glossyAuditSet;
                        device_->UpdateDescriptorSets(9, bufWrites);
                    }
                } else {
                    // T3.6/D1: Metal flat 槽（10 = flat SSBO offset 256，11 = fvCB）。
                    // 原 7（near irr）/9（dual-slot dup）/10（far irr）/11（far
                    // params）删除——dual-slot 在 Vulkan 上还会把 SSBO 写进
                    // binding 13 的 orm SampledImage 槽（基线验证错误）。
                    bufWrites[9].dstBinding = 10;
                    bufWrites[9].descriptorType = DescriptorType::StorageBuffer;
                    bufInfos[9].buffer = (flatActive && flatBuf != handles::INVALID_RESOURCE)
                        ? flatBuf : dummy_buffer_;
                    bufInfos[9].offset = 256;
                    bufInfos[9].range = ~0ull;
                    device_->UpdateDescriptorSets(10, bufWrites);
                    // Metal fvCB（binding 11，UBO）。
                    WriteDescriptorSet fvWrite{};
                    DescriptorBufferInfo fvInfo{};
                    fvWrite.dstSet = descriptorSet;
                    fvWrite.dstBinding = 11;
                    fvWrite.dstArrayElement = 0;
                    fvWrite.descriptorCount = 1;
                    fvWrite.descriptorType = DescriptorType::UniformBuffer;
                    fvWrite.imageInfo = nullptr;
                    fvWrite.bufferInfo = &fvInfo;
                    fvInfo.buffer = nvolumesCB; fvInfo.offset = 0;
                    fvInfo.range = sizeof(FlatVolumeCB);
                    device_->UpdateDescriptorSets(1, &fvWrite);
                    // 2026-09-17 P0：Metal buffer(12) = 近场重定位有效起点。
                    WriteDescriptorSet roWrite{};
                    DescriptorBufferInfo roInfo{};
                    roWrite.dstSet = descriptorSet;
                    roWrite.dstBinding = 12;
                    roWrite.dstArrayElement = 0;
                    roWrite.descriptorCount = 1;
                    roWrite.descriptorType = DescriptorType::StorageBuffer;
                    roWrite.imageInfo = nullptr;
                    roWrite.bufferInfo = &roInfo;
                    {
                        ResourceHandle nearRo = nvolViews[0].IsValid()
                            ? nvolViews[0].relocated_origins : handles::INVALID_RESOURCE;
                        roInfo.buffer = (nearRo != handles::INVALID_RESOURCE)
                            ? nearRo : dummy_buffer_;
                    }
                    roInfo.offset = 0;
                    roInfo.range = ~0ull;
                    device_->UpdateDescriptorSets(1, &roWrite);   // 2026-09-15 补提交：此前填充后从未 Update，buffer(12) 缺绑 → validation abort
                    // GI 根治 Phase 3.2: Metal buffer(13) = flat openSky。
                    WriteDescriptorSet osWrite{};
                    DescriptorBufferInfo osInfo{};
                    osWrite.dstSet = descriptorSet;
                    osWrite.dstBinding = 13;
                    osWrite.dstArrayElement = 0;
                    osWrite.descriptorCount = 1;
                    osWrite.descriptorType = DescriptorType::StorageBuffer;
                    osWrite.imageInfo = nullptr;
                    osWrite.bufferInfo = &osInfo;
                    osInfo.buffer = (flatOpenSkyBuf != handles::INVALID_RESOURCE)
                        ? flatOpenSkyBuf : dummy_buffer_;
                    osInfo.offset = 0;
                    osInfo.range = ~0ull;
                    device_->UpdateDescriptorSets(1, &osWrite);
                }
            }

            // Dispatch
            cmd->BindComputePipeline(pipeline_);
            const DescriptorSetHandle sets[] = { descriptorSet };
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, layout_, 0, 1, sets, 0, nullptr);
            cmd->Dispatch((gather_width_ + 7) / 8, (gather_height_ + 7) / 8, 1);

            // Optional receiver-source pass.  It consumes the same descriptor
            // inputs and frame constants as the production dispatch, but its
            // writes go only to diagnostic shadow textures and the auxiliary
            // 9-vec4/pixel SSBO.  Production GI remains the first dispatch.
            if (glossyAuditDispatch) {
                cmd->BindComputePipeline(glossy_source_audit_pipeline_);
                const DescriptorSetHandle auditSets[] = { glossyAuditSet };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute,
                                        glossy_source_audit_layout_, 0, 1,
                                        auditSets, 0, nullptr);
                cmd->Dispatch((gather_width_ + 7) / 8, (gather_height_ + 7) / 8, 1);
                ResourceBarrier auditBarriers[2]{};
                auditBarriers[0].resource = glossy_source_audit_output_;
                auditBarriers[0].beforeState = ResourceState::UnorderedAccess;
                auditBarriers[0].afterState = ResourceState::ShaderResource;
                auditBarriers[0].subresource = 0xFFFFFFFF;
                auditBarriers[1].resource = glossy_source_audit_specular_;
                auditBarriers[1].beforeState = ResourceState::UnorderedAccess;
                auditBarriers[1].afterState = ResourceState::ShaderResource;
                auditBarriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(auditBarriers, 2);
            }

            // Barrier output UAV → SRV for downstream Fusion pass.
            // History blit disabled (temporal accumulation not used — causes
            // layout issues with uninitialized history texture on first frame).
            {
                ResourceBarrier outBarriers[2]{};
                outBarriers[0].resource = gi_halfres_texture_;
                outBarriers[0].beforeState = ResourceState::UnorderedAccess;
                outBarriers[0].afterState = ResourceState::ShaderResource;
                outBarriers[0].subresource = 0xFFFFFFFF;
                outBarriers[1].resource = gi_specular_texture_;
                outBarriers[1].beforeState = ResourceState::UnorderedAccess;
                outBarriers[1].afterState = ResourceState::ShaderResource;
                outBarriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(outBarriers, 2);
            }

            // Exact same-frame inputs for CPU replay/probe attribution. The
            // snapshot happens after both dispatches, while all bound buffers
            // still contain the values just consumed. No stale ray/source is
            // substituted when the auxiliary dispatch was not requested.
            if (gatherPrefix && gatherPrefix[0] && isVk && !gather_audit_queued_ &&
                inputs.frame_index==auditFrame) {
                const u64 nearProbes=u64(nvolViews[0].counts.x)*u64(nvolViews[0].counts.y)*u64(nvolViews[0].counts.z);
                gather_audit_orm_resource_ = actualBoundORM;
                gather_audit_orm_runtime_format_ = DataFormat::Unknown;
                gather_audit_orm_width_ = gather_audit_orm_height_ = 0;
                gather_audit_orm_metadata_valid_ = false;
                gather_audit_orm_from_scene_ =
                    inputs.gbuffer_orm != handles::INVALID_RESOURCE &&
                    actualBoundORM == inputs.gbuffer_orm;
                if (actualBoundORM != handles::INVALID_RESOURCE) {
                    gather_audit_orm_runtime_format_ = device_->GetTextureFormat(actualBoundORM);
                    const bool sized = device_->GetTextureSize(actualBoundORM,
                                                               gather_audit_orm_width_,
                                                               gather_audit_orm_height_);
                    gather_audit_orm_metadata_valid_ =
                        sized && gather_audit_orm_runtime_format_ != DataFormat::Unknown;
                }
                struct Source {
                    const char* name;
                    ResourceHandle resource;
                    u64 bytes;
                    u32 bpp;
                    const char* format;
                    u32 binding;
                    const char* identity;
                    const char* layout;
                };
                Source sources[15]{};
                u32 sourceCount = 0;
                bool sourceListOverflow = false;
                auto addSource = [&](const char* name, ResourceHandle resource,
                                     u64 bytes, u32 bpp, const char* format,
                                     u32 binding, const char* identity,
                                     const char* layout) {
                    if (sourceCount >= 15) {
                        sourceListOverflow = true;
                        return;
                    }
                    sources[sourceCount++] = {name, resource, bytes, bpp, format,
                                              binding, identity, layout};
                };
                addSource("view", probeCB, sizeof(GIGatherCB), 0, "f32", 6,
                          "GIGather probe/camera/debug CB", "uniform_buffer");
                addSource("volumes", nvolumesCB, sizeof(FlatVolumeCB), 0, "f32", 18,
                          "GIGather FlatVolumeCB", "uniform_buffer");
                addSource("irradiance", flatBuf, 256ull + u64(fvTotalProbes) * 144ull, 0, "f32", 17,
                          "GIGather flat irradiance", "storage_buffer");
                addSource("opensky", flatOpenSkyBuf, 256ull + u64(fvTotalProbes) * 8ull, 0, "f32", 20,
                          "GIGather flat openSky", "storage_buffer");
                addSource("moments", ddgi->GetDepthBufferBySlot(ddgiReadIdx), nearProbes * 128ull * 4ull, 0, "f32", 12,
                          "GIGather DDGI depth moments", "storage_buffer");
                addSource("origins", nvolViews[0].relocated_origins, nearProbes * 16ull, 0, "f32", 19,
                          "GIGather near relocated origins", "storage_buffer");
                addSource("depth", inputs.gbuffer_depth, 0, 4, "", 0,
                          "GIGather gbuffer depth", "shader_resource");
                addSource("normal", inputs.gbuffer_normal, 0, 8, "", 1,
                          "GIGather gbuffer normal", "shader_resource");
                addSource("output", gi_halfres_texture_, 0, 8, "", 2,
                          "GIGather production diffuse output", "shader_resource");
                const ResourceHandle provenance = ddgi->GetDepthProvenanceBufferBySlot(ddgiReadIdx);
                if (provenance != handles::INVALID_RESOURCE)
                    addSource("provenance", provenance, nearProbes * 64ull * 8ull * 4ull, 0, "u32", 0xFFFFFFFFu,
                              "DDGI depth provenance", "storage_buffer");
                addSource("spec_output", gi_specular_texture_, 0, 8, "", 14,
                          "GIGather production glossy output", "shader_resource");
                if (glossyAuditDispatch) {
                    addSource("glossy-source", glossy_source_audit_buffer_[bindingIdx],
                              glossy_source_audit_bytes_, 0, "vec4x9_f32", 21,
                              "DDGI_GLOSSY_SOURCE_AUDIT receiver records", "storage_buffer");
                    addSource("glossy-aux-output", glossy_source_audit_output_, 0, 8, "", 2,
                              "DDGI_GLOSSY_SOURCE_AUDIT auxiliary diffuse", "shader_resource");
                    addSource("glossy-aux-spec", glossy_source_audit_specular_, 0, 8, "", 14,
                              "DDGI_GLOSSY_SOURCE_AUDIT auxiliary glossy", "shader_resource");
                    addSource("glossy-material-source", glossy_material_audit_buffer_[bindingIdx],
                              glossy_material_audit_bytes_, 0, "vec4x2_f32", 22,
                              "DDGI_GLOSSY_SOURCE_AUDIT material records", "storage_buffer");
                }
                if (sourceListOverflow) {
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[DDGI_GATHER_AUDIT] source snapshot exceeds 15-entry bound"
                        << std::endl;
                    gather_audit_orm_metadata_valid_ = false;
                }
                gather_audit_source_count_=sourceCount;
                const auto auditVolume=ddgi->GetVolumeData();
                gather_audit_ddgi_frame_=auditVolume.FrameIndex;
                gather_audit_ray_mode_=u32(auditVolume.RaySampleMode);
                gather_audit_rays_=auditVolume.RaysPerProbe;
                cmd->MemoryBarrier(PipelineStage::ComputeShader | PipelineStage::Transfer | PipelineStage::Host,
                    PipelineStage::Transfer,AccessFlag::ShaderWrite | AccessFlag::TransferWrite | AccessFlag::HostWrite,
                    AccessFlag::TransferRead);
                for (u32 i=0;i<gather_audit_source_count_;++i) {
                    const auto& src=sources[i];auto& dst=gather_audit_sources_[i];
                    dst.name=src.name;dst.resource=src.resource;dst.bytes=src.bytes;
                    dst.format=src.format;dst.layout=src.layout;
                    dst.source_identity=src.identity;dst.binding=src.binding;
                    dst.runtime_format=0;
                    if (src.resource==handles::INVALID_RESOURCE) continue;
                    u32 bpp=src.bpp;
                    if (src.bpp) {
                        const auto format=device_->GetTextureFormat(src.resource);
                        dst.runtime_format=static_cast<u32>(format);
                        if (format==DataFormat::BGRA8_UNorm) { bpp=4;dst.format="bgra8"; }
                        else if (format==DataFormat::RGBA8_UNorm) { bpp=4;dst.format="rgba8"; }
                        else if (format==DataFormat::RGBA16_Float) { bpp=8;dst.format="rgba16f"; }
                        else if (format==DataFormat::D32_Float) { bpp=4;dst.format="r32f"; }
                        else continue; // Never copy an unknown format into a guessed-size buffer.
                        if (!device_->GetTextureSize(src.resource,dst.width,dst.height)) continue;
                        dst.bytes=u64(dst.width)*dst.height*bpp;
                    }
                    if (!dst.bytes) continue;
                    BufferDesc bd{};bd.size=dst.bytes;
                    bd.bindFlags=u32(BufferUsageFlags::TransferDst);bd.memoryUsage=GPUMemoryUsage::Readback;
                    dst.staging=device_->CreateBuffer(bd);
                    if (dst.staging==handles::INVALID_RESOURCE) continue;
                    if (!src.bpp) cmd->CopyBuffer(src.resource,dst.staging,0,0,dst.bytes);
                    else {
                        ResourceBarrier bar{};bar.resource=src.resource;
                        bar.beforeState=ResourceState::ShaderResource;bar.afterState=ResourceState::CopySource;
                        cmd->InsertBarrier(&bar,1);
                        BufferTextureCopyRegion rr{};rr.imageSubresource.layerCount=1;
                        rr.imageExtent={dst.width,dst.height,1};
                        cmd->CopyTextureToBuffer(src.resource,dst.staging,&rr,1);
                        bar.beforeState=ResourceState::CopySource;bar.afterState=ResourceState::ShaderResource;
                        cmd->InsertBarrier(&bar,1);
                    }
                }
                cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,
                    AccessFlag::TransferWrite,AccessFlag::HostRead);
                gather_audit_frame_=inputs.frame_index;gather_audit_slot_=bindingIdx;
                gather_audit_queued_=true;
            }

            // SC_DUMP_CARDS=1 — deferred luminance stats of the gather output
            // (Fusion's DDGI input). Localizes a "screen went black" drain to
            // before/after this stage; probes vs output vs screen.
            static const bool dumpOut = [] {
                const char* e = std::getenv("SC_DUMP_CARDS");
                return e && atoi(e) != 0;
            }();
            static u32 tick = 0;
            ++tick;
            if (dumpOut) {
                const u32 dHalfW = gather_width_;
                const u32 dHalfH = gather_height_;
                if (out_dump_frame_ != 0xFFFFFFFFu && tick > out_dump_frame_ + 3u) {
                    if (auto* texels = static_cast<const u16*>(
                            device_->MapBuffer(out_staging_))) {
                        auto f16 = [](u16 h) -> float {
                            u32 sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu,
                                mant = h & 0x3FFu;
                            if (exp == 0u) return 0.0f;
                            u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
                            float f; std::memcpy(&f, &bits, 4);
                            return f;
                        };
                        std::vector<float> lums;
                        lums.reserve((size_t)dHalfW * dHalfH / 8);
                        for (u32 y = 0; y < dHalfH; y += 3)
                            for (u32 x = 0; x < dHalfW; x += 3) {
                                const u16* t = texels + ((size_t)y * dHalfW + x) * 4;
                                float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                                if (std::isnan(r) || std::isnan(g) || std::isnan(b)) continue;
                                lums.push_back(0.2126f * r + 0.7152f * g + 0.0722f * b);
                            }
                        if (!lums.empty()) {
                            std::sort(lums.begin(), lums.end());
                            auto pct = [&](float q) {
                                return lums[std::min(lums.size() - 1,
                                                     (size_t)(q * (lums.size() - 1)))];
                            };
                            PRIMAL_LOG_STREAM(Error, "Render") << "[GIGather] out lum p10=" << pct(0.10f)
                                      << " p50=" << pct(0.50f)
                                      << " p90=" << pct(0.90f)
                                      << " max=" << lums.back() << std::endl;
                        }
                        device_->UnmapBuffer(out_staging_);
                    }
                    out_dump_frame_ = 0xFFFFFFFFu;
                }
                if (tick % 90u == 0u && out_dump_frame_ == 0xFFFFFFFFu) {
                    const u64 size = (u64)dHalfW * dHalfH * 8u;   // RGBA16F
                    if (out_staging_ == rhi::handles::INVALID_RESOURCE ||
                        out_staging_size_ < size) {
                        if (out_staging_ != rhi::handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(out_staging_);
                        rhi::BufferDesc d{};
                        d.size = size;
                        d.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                        d.usage = rhi::GPUMemoryUsage::Readback;
                        d.memoryUsage = rhi::GPUMemoryUsage::Readback;
                        d.name = "GIGather_OutDump";
                        out_staging_ = device_->CreateBuffer(d);
                        out_staging_size_ = size;
                    }
                    if (out_staging_ != rhi::handles::INVALID_RESOURCE) {
                        // TEMP diagnosis: raw dump of the probe irradiance
                        // buffer AS THE GATHER READS IT (same slot, buffer
                        // copy — bypasses every sampling/stride question).
                        {
                            auto irrHandle = ddgi->GetIrradianceBufferBySlot(ddgiReadIdx);
                            if (irrHandle != rhi::handles::INVALID_RESOURCE) {
                                const u64 irrBytes = 16384ull * 9u * 16u;
                                static rhi::ResourceHandle irrStaging = rhi::handles::INVALID_RESOURCE;
                                static u64 irrStagingSize = 0;
                                if (irrStaging == rhi::handles::INVALID_RESOURCE ||
                                    irrStagingSize < irrBytes) {
                                    if (irrStaging != rhi::handles::INVALID_RESOURCE)
                                        device_->DestroyBuffer(irrStaging);
                                    rhi::BufferDesc id{};
                                    id.size = irrBytes;
                                    id.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                                    id.usage = rhi::GPUMemoryUsage::Readback;
                                    id.memoryUsage = rhi::GPUMemoryUsage::Readback;
                                    id.name = "GIGather_IrrDump";
                                    irrStaging = device_->CreateBuffer(id);
                                    irrStagingSize = irrBytes;
                                }
                                if (irrStaging != rhi::handles::INVALID_RESOURCE) {
                                    cmd->CopyBuffer(irrHandle, irrStaging, 0, 0, irrBytes);
                                    static u64 s_irrDumpTickBase = 0;
                                    s_irrDumpTickBase = tick; (void)s_irrDumpTickBase;
                                    // Read next frame's stats print (delayed) —
                                    // reuse out_staging deferred pattern via a
                                    // simple 2-tick delayed map is complex; do
                                    // an immediate Wait+Map on a throwaway CB.
                                    auto chI = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
                                    auto* cI = rhi::GetCommandBuffer(chI);
                                    if (cI) {
                                        cI->Begin();
                                        cI->CopyBuffer(irrHandle, irrStaging, 0, 0, irrBytes);
                                        cI->End(); cI->Submit(); cI->WaitForCompletion();
                                        device_->DestroyCommandBuffer(chI);
                                        if (auto* q = static_cast<const float*>(
                                                device_->MapBuffer(irrStaging, 0, irrBytes))) {
                                            float maxAbs = 0.0f; u64 bad = 0;
                                            // 逐波段 maxAbs（band0=L0, 1-3=L1, 4-8=L2）
                                            // + 全局 argmax 探针——把怪物值定位到具体波段。
                                            float bandMax[9] = {}; double bandSum[9] = {};
                                            float worst[4] = {}; u32 worstProbe = 0;
                                            for (u64 p = 0; p < 16384ull; ++p) {
                                                const float* c = q + p * 36;
                                                float pAbs = 0.0f;
                                                for (u32 k = 0; k < 36; ++k) {
                                                    float v = c[k];
                                                    if (std::isnan(v) || std::isinf(v)) { ++bad; continue; }
                                                    float a = std::abs(v);
                                                    pAbs = std::max(pAbs, a);
                                                    maxAbs = std::max(maxAbs, a);
                                                    u32 band = k / 4;
                                                    bandMax[band] = std::max(bandMax[band], a);
                                                    bandSum[band] += a;
                                                }
                                                if (pAbs > worst[0]) {
                                                    worst[0] = pAbs; worstProbe = (u32)p;
                                                    for (int k = 0; k < 3; ++k) worst[1 + k] = c[k];
                                                }
                                            }
                                            float l0Mean = bandSum[0] / 16384.0f;
                                            float l1Mean = 0.0f, l2Mean = 0.0f;
                                            for (int b = 1; b < 4; ++b) l1Mean += bandSum[b];
                                            for (int b = 4; b < 9; ++b) l2Mean += bandSum[b];
                                            l1Mean /= 3.0f * 16384.0f; l2Mean /= 5.0f * 16384.0f;
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  band means: L0=" << l0Mean
                                                      << " L1=" << l1Mean << " L2=" << l2Mean
                                                      << " |L1|/|L0|=" << (l0Mean > 1e-6f ? l1Mean / l0Mean : 0.0f) << std::endl;
                                            double checksum = 0.0;
                                            for (u64 k = 0; k < irrBytes / 4; ++k) {
                                                float v = q[k];
                                                if (!std::isnan(v) && !std::isinf(v)) checksum += std::abs(v);
                                            }
                                            PRIMAL_LOG_STREAM(Error, "Render") << "[IRR_DUMP] slot=" << ddgiReadIdx
                                                      << " maxAbs=" << maxAbs
                                                      << " nanInf=" << bad
                                                      << " chk=" << (long long)checksum << std::endl;
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  bands maxAbs: L0=" << bandMax[0]
                                                      << " L1=(" << bandMax[1] << "," << bandMax[2] << "," << bandMax[3] << ")"
                                                      << " L2=(" << bandMax[4] << "," << bandMax[5] << "," << bandMax[6]
                                                      << "," << bandMax[7] << "," << bandMax[8] << ")" << std::endl;
                                            // 深度图健康检查：可见性加权的输入。
                                            // 全零/全 1 → UpdateDepth 未生效。
                                            if (auto* dp = static_cast<const float*>(
                                                    device_->MapBuffer(ddgi->GetDepthBufferBySlot(ddgiReadIdx), 0, 16384ull * 128u * 4u))) {
                                                u64 zeroMean = 0; float meanSum = 0.0f, varSum = 0.0f;
                                                for (u64 pr = 0; pr < 16384ull; ++pr) {
                                                    const float* dm = dp + pr * 128u;
                                                    float m = dm[0]; // 取 64 mean 中首个代表
                                                    for (int k = 1; k < 64; ++k) m += dm[k];
                                                    m /= 64.0f;
                                                    if (m <= 1e-6f) ++zeroMean;
                                                    meanSum += m;
                                                    for (int k = 64; k < 128; ++k) varSum += dm[k];
                                                }
                                                PRIMAL_LOG_STREAM(Error, "Render") << "  [DEPTH_DUMP] zeroMeanProbes=" << zeroMean
                                                          << "/16384 meanOfMeans=" << meanSum / 16384.0f
                                                          << " avgVar=" << varSum / (16384.0f * 64.0f) << std::endl;
                                                device_->UnmapBuffer(ddgi->GetDepthBufferBySlot(ddgiReadIdx));
                                            }
                                            PRIMAL_LOG_STREAM(Error, "Render") << "  worstProbe=" << worstProbe
                                                      << " abs=" << worst[0]
                                                      << " L0rgb=(" << worst[1] << "," << worst[2] << "," << worst[3] << ")" << std::endl;
                                            device_->UnmapBuffer(irrStaging);
                                        }
                                    }
                                }
                            }
                        }
                        ResourceBarrier toCopy{};
                        toCopy.resource = gi_halfres_texture_;
                        toCopy.beforeState = ResourceState::ShaderResource;
                        toCopy.afterState = ResourceState::CopySource;
                        toCopy.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toCopy, 1);

                        rhi::BufferTextureCopyRegion region{};
                        region.bufferOffset = 0;
                        region.bufferRowLength = dHalfW;
                        region.bufferImageHeight = dHalfH;
                        region.imageSubresource.mipLevel = 0;
                        region.imageSubresource.baseArrayLayer = 0;
                        region.imageSubresource.layerCount = 1;
                        region.imageOffset = {0, 0, 0};
                        region.imageExtent = {dHalfW, dHalfH, 1};
                        cmd->CopyTextureToBuffer(gi_halfres_texture_, out_staging_,
                                                 &region, 1);

                        ResourceBarrier toSrv{};
                        toSrv.resource = gi_halfres_texture_;
                        toSrv.beforeState = ResourceState::CopySource;
                        toSrv.afterState = ResourceState::ShaderResource;
                        toSrv.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&toSrv, 1);
                        out_dump_frame_ = tick;
                    }
                }
            }
        }
    );

    return outputs;
}

} // namespace primal::graphics
