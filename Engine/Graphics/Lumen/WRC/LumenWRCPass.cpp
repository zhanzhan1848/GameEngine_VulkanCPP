#include "LumenWRCPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Lumen/GIWarmup.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::lumen {

// ============================================================================
// LumenWRCPass — [WRC] World Space Radiance Cache 实现。
// 铁律：本文件禁止 include 任何 DDGI 专有头（设计文档 §1.2-1）；共享设施仅
// GICacheView.h / GIWarmup.h / GIDebugParams.h / LumenTypes.h 白名单。
// ============================================================================

using namespace rhi;
using namespace rendergraph;

namespace {

struct DescriptorData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set,
                         const DescriptorData* params, uint32_t count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (uint32_t i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer ||
            params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// Vulkan：预编译 .comp.spv（Vulkan/Shaders/Lumen/，include 已在构建期内联）。
std::vector<u8> LoadWrcSpv(const char* shaderName) {
    const std::string rel = std::string("Engine/Graphics/Vulkan/Shaders/Lumen/")
                            + shaderName + ".comp.spv";
    const std::string resolved = utils::ShaderRegistry::ResolveShaderPath(rel);
    for (const std::string& path : { rel, resolved }) {
        if (path.empty()) continue;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytecode(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) continue;
        return bytecode;
    }
    PRIMAL_LOG_STREAM(Error, "Render") << "[WRC] Failed to load SPIR-V: " << shaderName << std::endl;
    return {};
}

struct WRCRGData {
    RGResourceHandle sc_atlas{};    ///< SC atlas 写读屏障声明（vi==0 时有效）
};

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

LumenWRCPass::~LumenWRCPass() { Shutdown(); }

bool LumenWRCPass::Initialize(rhi::RHIDeviceBase* device, const WRCRuntimeParams& params) {
    // A 线 Vulkan 先行（设计 §1.1）；Metal 上 Initialize 失败，接线层跳过
    //（Metal parity 按 Docs/RHI/GraphicsAPI_Parity_Matrix.md 跟进）。
    if (device->GetPlatform() != RHIPlatform::Vulkan) return false;
    if (GetProbeCountTotal() == 0) return false;

    device_ = device;
    params_ = params;

    const u32 probeTotal = GetProbeCountTotal();
    const u32 K = kWRCDirsPerProbe;

    // 探针 radiance storage（单缓冲持久；stride 见 WRCTypes.h；内存预算
    // 复核 ~8.5 MB @ 16k probes，见 WRCTypes.h 注释）。
    {
        BufferDesc desc{};
        desc.size = u64(probeTotal) * kWRCProbeStrideU32 * sizeof(u32);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = probeTotal;
        desc.structured.elementStride = kWRCProbeStrideU32 * sizeof(u32);
        probe_data_ = device->CreateBuffer(desc);
        if (probe_data_ == handles::INVALID_RESOURCE) return false;
        if (void* mapped = device->MapBuffer(probe_data_)) {
            std::memset(mapped, 0, static_cast<size_t>(desc.size));
            device->UnmapBuffer(probe_data_);
        }
    }
    // 逐射线 scratch（绝对探针索引 × K × vec4）。
    {
        BufferDesc desc{};
        desc.size = u64(probeTotal) * K * sizeof(math::v4);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = probeTotal * K;
        desc.structured.elementStride = sizeof(math::v4);
        ray_data_ = device->CreateBuffer(desc);
        if (ray_data_ == handles::INVALID_RESOURCE) return false;
    }
    // 命中距离（update-slot × K + 尾部 per-slot 起点 SDF 距离）。
    {
        BufferDesc desc{};
        desc.size = u64(probeTotal) * (K + 1) * sizeof(f32);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = probeTotal * (K + 1);
        desc.structured.elementStride = sizeof(f32);
        hit_distance_ = device->CreateBuffer(desc);
        if (hit_distance_ == handles::INVALID_RESOURCE) return false;
    }
    // 消费计数（A3 SPGI 原子累加；A5 预算优先级数据源）。
    {
        BufferDesc desc{};
        desc.size = u64(probeTotal) * sizeof(u32);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = probeTotal;
        desc.structured.elementStride = sizeof(u32);
        consume_counter_ = device->CreateBuffer(desc);
        if (consume_counter_ == handles::INVALID_RESOURCE) return false;
    }
    // prefilter SH9 输出（调试/降级档）。
    {
        BufferDesc desc{};
        desc.size = u64(probeTotal) * 9u * sizeof(math::v4);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        desc.structured.elementCount = probeTotal * 9u;
        desc.structured.elementStride = sizeof(math::v4);
        prefilter_sh_ = device->CreateBuffer(desc);
        if (prefilter_sh_ == handles::INVALID_RESOURCE) return false;
    }
    // update list（Dynamic——CPU 每帧 Map 填充轮转窗口）+ volume UBO（三缓冲）。
    const u32 budget = std::min(params_.update_budget_probes_per_frame, probeTotal);
    for (u32 i = 0; i < 3; ++i) {
        BufferDesc listDesc{};
        listDesc.size = u64(budget) * sizeof(u32);
        listDesc.type = BufferType::Structured;
        listDesc.usage = GPUMemoryUsage::Dynamic;
        listDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        listDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        listDesc.structured.elementCount = budget;
        listDesc.structured.elementStride = sizeof(u32);
        update_list_[i] = device->CreateBuffer(listDesc);

        BufferDesc cbDesc{};
        cbDesc.size = sizeof(WRCVolumeData);
        cbDesc.type = BufferType::Constant;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        volume_cb_[i] = device->CreateBuffer(cbDesc);

        if (update_list_[i] == handles::INVALID_RESOURCE ||
            volume_cb_[i] == handles::INVALID_RESOURCE) return false;
    }

    // SC 未启用时的占位资源（finalize 的 binding 仍需有效；shader 由
    // SCParams.z=0 短路，永不采样占位内容）。2D 纹理占位必须与 shader 的
    // OpTypeImage Dim=2D 匹配——绑 3D SDF 会触发 viewType 验证错误。
    {
        TextureDesc dummyDesc{};
        dummyDesc.size = {1, 1, 1};
        dummyDesc.format = DataFormat::RGBA16_Float;
        dummyDesc.type = TextureType::Texture2D;
        dummyDesc.usage = TextureUsage::ShaderResource;
        dummy_2d_ = device->CreateTexture(dummyDesc);
        BufferDesc dummyBuf{};
        dummyBuf.size = 16;
        dummyBuf.type = BufferType::Structured;
        dummyBuf.usage = GPUMemoryUsage::Dynamic;
        dummyBuf.memoryUsage = GPUMemoryUsage::Dynamic;
        dummyBuf.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        dummyBuf.structured.elementCount = 4;
        dummyBuf.structured.elementStride = sizeof(u32);
        dummy_buffer_ = device->CreateBuffer(dummyBuf);
    }

    // GIDebug wrc 通道读回环（PRIMAL_WRC_DUMP=1 时经它做 radiance dump）。
    if (WRCDumpEnabled()) {
        dump_ring_.initialize(device_, u64(probeTotal) * kWRCProbeStrideU32 * sizeof(u32), 3);
    }
    // A5 消费计数读回环。
    consume_ring_.initialize(device_, u64(probeTotal) * sizeof(u32), 3);
    consume_snapshot_.assign(probeTotal, 0u);
    // [Q2 排查] 射线去向构成读回环（PRIMAL_WRC_TRACE_STATS=1）。
    trace_stats_enabled_ = [] {
        const char* e = std::getenv("PRIMAL_WRC_TRACE_STATS");
        return e && e[0] == '1';
    }();
    // [R1] 拒绝位置分箱 dump（PRIMAL_WRC_BOUNDS_DUMP=1）。
    bounds_dump_enabled_ = [] {
        const char* e = std::getenv("PRIMAL_WRC_BOUNDS_DUMP");
        return e && e[0] == '1';
    }();
    if (trace_stats_enabled_ || bounds_dump_enabled_) {
        rays_ring_.initialize(device_, u64(probeTotal) * kWRCDirsPerProbe * sizeof(math::v4), 3);
        BufferDesc dbgDesc{};
        dbgDesc.size = 64;
        dbgDesc.type = BufferType::Structured;
        dbgDesc.usage = GPUMemoryUsage::Dynamic;
        dbgDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        dbgDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        dbgDesc.structured.elementCount = 16;
        dbgDesc.structured.elementStride = sizeof(u32);
        dbg_buf_ = device->CreateBuffer(dbgDesc);
        // 拒绝位置 8³ 分箱（512 uvec4）——binding 恒有效；读回仅 dump 门开启。
        BufferDesc binsDesc{};
        binsDesc.size = 512u * 16u;
        binsDesc.type = BufferType::Structured;
        binsDesc.usage = GPUMemoryUsage::Dynamic;
        binsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        binsDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        binsDesc.structured.elementCount = 512u;
        binsDesc.structured.elementStride = 16u;
        bins_buf_ = device->CreateBuffer(binsDesc);
        // [F4 2026-09-20] 逐卡 boundsFail 计数（4096 u32）——binding 恒有效
        //（shader 侧 reject==1 且 cardIdx<4096 才写）；读回仅 dump 门开启。
        BufferDesc cfDesc{};
        cfDesc.size = 4096u * sizeof(u32);
        cfDesc.type = BufferType::Structured;
        cfDesc.usage = GPUMemoryUsage::Dynamic;
        cfDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        cfDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        cfDesc.structured.elementCount = 4096u;
        cfDesc.structured.elementStride = sizeof(u32);
        card_fail_buf_ = device->CreateBuffer(cfDesc);
    }
    if (bounds_dump_enabled_) {
        bins_ring_.initialize(device_, u64(512) * 16u, 3);
        card_fail_ring_.initialize(device_, u64(4096) * sizeof(u32), 3);   // [F4]
    }
    // [A2 验收] prefilter SH9 读回环（prefilter 开启时启用）。Initialize 早于
    // 管线侧 SetPrefilterEnabled 调用——此处直接读 env 同源判定。
    prefilter_enabled_ = prefilter_enabled_ || [] {
        const char* e = std::getenv("PRIMAL_WRC_PREFILTER");
        return e && e[0] == '1';
    }();
    if (prefilter_enabled_ || WRCDumpEnabled()) {
        // [P4 2026-09-21] DUMP=1 即标准启用（指标不再依赖 PREFILTER 附属开关）。
        prefilter_ring_.initialize(device_, u64(probeTotal) * 9u * sizeof(math::v4), 3);
    }

    CreatePipelines();
    if (trace_pipeline_ == handles::INVALID_PIPELINE ||
        finalize_pipeline_ == handles::INVALID_PIPELINE ||
        update_pipeline_ == handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[WRC] pipeline creation failed" << std::endl;
        return false;
    }

    initialized_ = true;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[WRC] initialized: probes " << params_.probe_count_x << "x"
        << params_.probe_count_y << "x" << params_.probe_count_z << " @ "
        << params_.probe_spacing << " (total " << probeTotal << ", K=" << K
        << ", budget=" << budget << "/frame), probe storage ~"
        << (u64(probeTotal) * kWRCProbeStrideU32 * 4ull / (1024 * 1024)) << " MB"
        << std::endl;
    return true;
}

void LumenWRCPass::Shutdown() {
    if (!initialized_) return;
    dump_ring_.shutdown(device_);
    consume_ring_.shutdown(device_);
    prefilter_ring_.shutdown(device_);
    rays_ring_.shutdown(device_);
    bins_ring_.shutdown(device_);
    initialized_ = false;
    device_ = nullptr;
    // 句柄为 POD 类型，由 RHI device 的垃圾回收管理（同 LumenDDGIPass 约定）。
}

void LumenWRCPass::CreatePipelines() {
    auto Compile = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadWrcSpv(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };
    auto traceShader     = Compile("WRCTraceSDF", "wrc_trace_sdf");
    auto finalizeShader  = Compile("WRCTraceFinalize", "wrc_trace_finalize");
    auto updateShader    = Compile("WRCUpdateRadiance", "wrc_update_radiance");
    auto prefilterShader = Compile("WRCPrefilterIrradiance", "wrc_prefilter_irradiance");

    // --- descriptor layouts（与 shader 绑定表逐项对应）---
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf0
            {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf1
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf2
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // volume
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit (write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // list (ro)
        };
        b[0].is3D = true; b[1].is3D = true; b[2].is3D = true;
        b[5].readonly = true;
        trace_set_layout_ = device_->CreateDescriptorSetLayout({6, b});
        PipelineLayoutDesc pl{}; pl.setLayoutCount = 1; pl.setLayouts = &trace_set_layout_;
        trace_layout_ = device_->CreatePipelineLayout(pl);
    }
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sc lighting atlas
            {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sc depth atlas
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // volume
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit (ro)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_data (write)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // list (ro)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc lookups (ro)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc cards (ro)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc grid offsets (ro)
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc grid cards (ro)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [Q2] 查询漏斗计数
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [R1] 拒绝位置分箱
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [F4] 逐卡 boundsFail 计数
        };
        b[1].readonly = true; b[3].readonly = true;
        b[5].readonly = true; b[6].readonly = true;
        b[7].readonly = true; b[8].readonly = true; b[9].readonly = true;
        finalize_set_layout_ = device_->CreateDescriptorSetLayout({13, b});
        PipelineLayoutDesc pl{}; pl.setLayoutCount = 1; pl.setLayouts = &finalize_set_layout_;
        finalize_layout_ = device_->CreatePipelineLayout(pl);
    }
    {
        DescriptorSetLayoutBinding b[] = {
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // volume
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_data (ro)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe_data (rw)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // list (ro)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit (ro)
        };
        // Array indices are descriptor-list positions, not shader binding
        // numbers (bindings here start at 2).
        b[1].readonly = true;  // binding 3: ray_data
        b[3].readonly = true;  // binding 5: update list
        b[4].readonly = true;  // binding 6: hit distance
        update_set_layout_ = device_->CreateDescriptorSetLayout({5, b});
        PipelineLayoutDesc pl{}; pl.setLayoutCount = 1; pl.setLayouts = &update_set_layout_;
        update_layout_ = device_->CreatePipelineLayout(pl);
    }
    {
        DescriptorSetLayoutBinding b[] = {
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // volume
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe_data (ro)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // sh9 (write)
        };
        b[1].readonly = true;  // binding 4: probe_data
        prefilter_set_layout_ = device_->CreateDescriptorSetLayout({3, b});
        PipelineLayoutDesc pl{}; pl.setLayoutCount = 1; pl.setLayouts = &prefilter_set_layout_;
        prefilter_layout_ = device_->CreatePipelineLayout(pl);
    }

    auto MakePipeline = [&](ShaderHandle sh, PipelineLayoutHandle layout, u32 x, u32 y) {
        if (sh == handles::INVALID_SHADER) return handles::INVALID_PIPELINE;
        ComputePipelineDesc d{};
        d.computeShader = sh;
        d.layout = layout;
        d.threadGroupSize = {x, y, 1};
        return device_->CreateComputePipeline(d);
    };
    trace_pipeline_     = MakePipeline(traceShader, trace_layout_, 8, 8);
    finalize_pipeline_  = MakePipeline(finalizeShader, finalize_layout_, 8, 8);
    update_pipeline_    = MakePipeline(updateShader, update_layout_, 64, 1);
    prefilter_pipeline_ = MakePipeline(prefilterShader, prefilter_layout_, 64, 1);

    // [反弹修复] 卡片→探针注入（binding 2 UBO / 3 probe_data rw / 4 cards /
    // 5 lighting atlas / 6 card list / 7..9 SDF 可见性步进）。
    {
        DescriptorSetLayoutBinding b[] = {
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
        };
        b[2].readonly = true;  // binding 4: cards
        b[4].readonly = true;  // binding 6: card list
        b[5].is3D = true;      // binding 7: SDF cascade 0
        b[6].is3D = true;      // binding 8: SDF cascade 1
        b[7].is3D = true;      // binding 9: SDF cascade 2
        inject_set_layout_ = device_->CreateDescriptorSetLayout({8, b});
        PipelineLayoutDesc pl{}; pl.setLayoutCount = 1; pl.setLayouts = &inject_set_layout_;
        inject_layout_ = device_->CreatePipelineLayout(pl);
        inject_pipeline_ = MakePipeline(Compile("WRCCardInject", "wrc_card_inject"),
                                        inject_layout_, 64, 1);
        for (u32 i = 0; i < 3; ++i)
            inject_ds_[i] = device_->CreateDescriptorSet({inject_set_layout_});
    }
    for (u32 i = 0; i < 3; ++i) {
        trace_ds_[i] = device_->CreateDescriptorSet({trace_set_layout_});
        finalize_ds_[i] = device_->CreateDescriptorSet({finalize_set_layout_});
        update_ds_[i] = device_->CreateDescriptorSet({update_set_layout_});
        prefilter_ds_[i] = device_->CreateDescriptorSet({prefilter_set_layout_});
    }
}

// ============================================================================
// External data supply
// ============================================================================

void LumenWRCPass::SetSurfaceCacheResources(const SurfaceCacheView& view) {
    sc_lighting_atlas_ = view.lighting_atlas;
    sc_depth_atlas_    = view.depth_atlas;
    sc_card_lookup_    = view.card_lookup;
    sc_card_data_      = view.card_data;
    sc_grid_offsets_   = view.grid_offsets;
    sc_grid_cards_     = view.grid_cards;
    sc_atlas_size_     = view.atlas_size;
    sc_lookup_count_   = view.lookup_count;
    sc_enabled_ = view.IsValid() && view.card_data != rhi::handles::INVALID_RESOURCE;
}

void LumenWRCPass::ClearSurfaceCacheResources() {
    sc_lighting_atlas_ = rhi::handles::INVALID_RESOURCE;
    sc_depth_atlas_ = rhi::handles::INVALID_RESOURCE;
    sc_card_lookup_ = rhi::handles::INVALID_RESOURCE;
    sc_card_data_ = rhi::handles::INVALID_RESOURCE;
    sc_grid_offsets_ = rhi::handles::INVALID_RESOURCE;
    sc_grid_cards_ = rhi::handles::INVALID_RESOURCE;
    sc_enabled_ = false;
}

void LumenWRCPass::ClearProbeField() {
    // Clear both the CPU-side seed state and the GPU validity/radiance
    // storage.  Otherwise SPGI may observe a random/stale valid bit before a
    // probe receives its first update after initialization or a scene cut.
    probe_field_seeded_ = false;
    if (device_ && probe_data_ != handles::INVALID_RESOURCE) {
        if (void* mapped = device_->MapBuffer(probe_data_)) {
            const u64 bytes = u64(GetProbeCountTotal()) * kWRCProbeStrideU32 * sizeof(u32);
            std::memset(mapped, 0, static_cast<size_t>(bytes));
            device_->UnmapBuffer(probe_data_);
        }
    }
}

WRCCacheView LumenWRCPass::GetCacheView(u32 frame_idx) const {
    (void)frame_idx;
    WRCCacheView view;
    view.probe_data = probe_data_;
    view.origin = probe_origin_;
    view.counts = math::v3{ float(params_.probe_count_x),
                            float(params_.probe_count_y),
                            float(params_.probe_count_z) };
    view.spacing = params_.probe_spacing;
    view.dirs_per_probe = kWRCDirsPerProbe;
    view.probe_count_total = GetProbeCountTotal();
    return view;
}

// ============================================================================
// [反弹修复] 卡片→探针邻域表构建（CPU，卡片数变化时重建）。
// 每探针收集半径 R = 2.5×spacing 内的卡片，按 faceArea/d² 权重降序取 ≤16，
// 打包为扁平 u32 表：[count, (cardIdx, wBits) × 16] × probeTotal。
// ============================================================================
void LumenWRCPass::UpdateProbeCardList(const SurfaceCacheCard* cards, u32 cardCount) {
    // 反弹专项 v4（能量守恒聚合注入）默认开：聚合后单次 mix，trace 测量
    // 保留 (1-w) 份额——v3 的逐卡独立 mix 洗平问题消除。env=0 回滚。
    card_inject_enabled_ = card_inject_enabled_ &&
        [] { const char* e = std::getenv("PRIMAL_WRC_CARD_INJECT"); return !(e && e[0] == '0'); }();
    if (!card_inject_enabled_ || cardCount == 0 ||
        cardCount == card_list_card_count_) return;
    card_list_card_count_ = cardCount;

    const u32 probeTotal = GetProbeCountTotal();
    const float spacing = params_.probe_spacing;
    const float radius = spacing * 1.8f;   // 反弹专项 v2: 7m→3.6m——远卡属更近探针
    probe_card_list_cpu_.assign(size_t(probeTotal) * 17u, 0u);

    for (u32 pi = 0; pi < probeTotal; ++pi) {
        const u32 pz = pi / (params_.probe_count_x * params_.probe_count_y);
        const u32 rem = pi % (params_.probe_count_x * params_.probe_count_y);
        const u32 py = rem / params_.probe_count_x;
        const u32 px = rem % params_.probe_count_x;
        const math::v3 probePos{
            probe_origin_.x + float(px) * spacing,
            probe_origin_.y + float(py) * spacing,
            probe_origin_.z + float(pz) * spacing };

        std::vector<std::pair<float, u32>> picked;   // (weight, cardIdx)
        for (u32 ci = 0; ci < cardCount; ++ci) {
            const SurfaceCacheCard& c = cards[ci];
            if (c.resolution == 0) continue;
            const math::v3 d{
                c.center.x - probePos.x, c.center.y - probePos.y, c.center.z - probePos.z };
            const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            const float ex = c.extent.x, ey = c.extent.y, ez = c.extent.z;
            const uint axis = c.axis_direction & 0xFFu;
            const float hu = (axis == 0u) ? ey : ex;
            const float hv = (axis == 0u) ? ez : (axis == 1u ? ez : ey);
            const float faceArea = 4.0f * hu * hv;
            if (faceArea <= 0.0f) continue;
            // 可达半径 = 卡片包围球 + 探针影响半径
            const float bound = std::sqrt(ex * ex + ey * ey + ez * ez) + radius;
            if (d2 > bound * bound) continue;
            // 反弹专项 v2: 可见卡片权重上限 PRIMAL_WRC_INJECT_CAP（默认
            // 0.6）——可见反弹压过天空填充；shader 端 clamp 放宽到 1.0，
            // 夹紧责任在 CPU。
            static const float kInjectCap = [] {
                const char* e = std::getenv("PRIMAL_WRC_INJECT_CAP");
                if (!e) return 0.6f;
                const float v = (float)std::atof(e);
                return (v > 0.0f && v <= 1.0f) ? v : 0.6f;
            }();
            const float w = std::min(faceArea / std::max(d2, 1.0f) * 0.3f, kInjectCap);
            if (w <= 0.01f) continue;
            picked.emplace_back(w, ci);
        }
        if (picked.empty()) continue;
        std::sort(picked.begin(), picked.end(),
                  [](const std::pair<float, u32>& a, const std::pair<float, u32>& b) {
                      return a.first > b.first;
                  });
        const u32 take = std::min<u32>((u32)picked.size(), 16u);
        u32* out = &probe_card_list_cpu_[size_t(pi) * 17u];
        out[0] = take;
        for (u32 i = 0; i < take; ++i) {
            u32 bits;
            std::memcpy(&bits, &picked[i].first, 4);
            out[1 + i * 2 + 0] = picked[i].second;
            out[1 + i * 2 + 1] = bits;
        }
    }

    // 上传（缓冲按需建/重建）。
    const u64 bytes = u64(probeTotal) * 17u * sizeof(u32);
    if (card_list_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(card_list_);
    BufferDesc desc{};
    desc.size = bytes;
    desc.type = BufferType::Structured;
    desc.usage = GPUMemoryUsage::Dynamic;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;
    desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    desc.structured.elementCount = probeTotal * 17u;
    desc.structured.elementStride = sizeof(u32);
    card_list_ = device_->CreateBuffer(desc);
    if (card_list_ != handles::INVALID_RESOURCE) {
        if (auto* m = static_cast<u32*>(device_->MapBuffer(card_list_))) {
            std::memcpy(m, probe_card_list_cpu_.data(), bytes);
            device_->UnmapBuffer(card_list_);
        }
    }
    u32 probesWithCards = 0;
    for (u32 pi = 0; pi < probeTotal; ++pi)
        if (probe_card_list_cpu_[size_t(pi) * 17u] != 0u) ++probesWithCards;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[WRC] card-inject list rebuilt: cards=" << cardCount
        << " probes_with_cards=" << probesWithCards << "/" << probeTotal
        << std::endl;
}

// ============================================================================
// AddPass — trace → finalize → update（→ prefilter 调试档）
// ============================================================================

void LumenWRCPass::FillVolumeData(WRCVolumeData& vd, const WRCCameraData& cam, u32 frame) {
    vd.ProbeOrigin = math::v4{ probe_origin_.x, probe_origin_.y, probe_origin_.z,
                               params_.probe_spacing };
    vd.ProbeCounts[0] = params_.probe_count_x;
    vd.ProbeCounts[1] = params_.probe_count_y;
    vd.ProbeCounts[2] = params_.probe_count_z;
    // 零穿越判定旗标：signed 资产置位；PRIMAL_WRC_UNSIGNED_TRACE=1 回退。
    bool signedField = coarse_sdf_signed_;
    if (const char* e = std::getenv("PRIMAL_WRC_UNSIGNED_TRACE")) {
        if (e[0] == '1') signedField = false;
    }
    vd.ProbeCounts[3] = signedField ? 1u : 0u;
    vd.DirsPerProbe = kWRCDirsPerProbe;
    vd.ProbeCountTotal = GetProbeCountTotal();
    vd.RaysPerProbe = kWRCDirsPerProbe;
    vd.RayMaxDistance = params_.ray_max_distance;
    vd.TemporalAlpha = kWRCRadianceAlpha;
    vd.FrameIndex = frame;
    vd.WarmupFrames = GIWarmupFrames();   // §6-2：暖启动期 EMA 冻结（单一门控）

    if (has_offline_sdf_) {
        vd.SdfOrigins[0] = math::v4{ offline_sdf_origin_.x, offline_sdf_origin_.y, offline_sdf_origin_.z, 0.0f };
        vd.SdfExtents[0] = math::v4{ offline_sdf_extent_.x, offline_sdf_extent_.y, offline_sdf_extent_.z, 0.0f };
        vd.SdfVoxelSizes[0] = math::v4{ offline_sdf_extent_.x / float(offline_sdf_resolution_), 0.0f, 0.0f, 0.0f };
        for (u32 c = 1; c < 3; ++c) {
            vd.SdfOrigins[c] = vd.SdfOrigins[0];
            vd.SdfExtents[c] = vd.SdfExtents[0];
            vd.SdfVoxelSizes[c] = vd.SdfVoxelSizes[0];
        }
        vd.SdfResolutions[0] = vd.SdfResolutions[1] = vd.SdfResolutions[2] = offline_sdf_resolution_;
        vd.SdfCascadeCount = 1;
    } else {
        auto& sdf = nanite::GlobalSDF::Get();
        vd.SdfCascadeCount = 0;
        if (sdf.IsInitialized()) {
            const u32 cc = std::min(3u, sdf.GetConfig().cascade_count);
            for (u32 c = 0; c < 3; ++c) {
                if (c < cc) {
                    const auto& cascade = sdf.GetCascade(c);
                    vd.SdfOrigins[c] = math::v4{ cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f };
                    vd.SdfExtents[c] = math::v4{ cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f };
                    vd.SdfVoxelSizes[c] = math::v4{ cascade.voxel_size, 0.0f, 0.0f, 0.0f };
                    vd.SdfResolutions[c] = cascade.resolution;
                } else {
                    vd.SdfOrigins[c] = math::v4{};
                    vd.SdfExtents[c] = math::v4{};
                    vd.SdfVoxelSizes[c] = math::v4{};
                    vd.SdfResolutions[c] = 0;
                }
            }
            vd.SdfCascadeCount = cc;
        }
    }

    vd.LightDirection = math::v4{ cam.light_direction.x, cam.light_direction.y,
                                  cam.light_direction.z, 0.0f };
    vd.LightColor = math::v4{ cam.light_color.x, cam.light_color.y, cam.light_color.z, 0.0f };
    // 天空契约：共享 EvaluateSkyBase × SkyColor.xyz × 档位（kWRCSkyTier 独立
    // 常量源，随 w 下发；w=0 关闭天空注入）。
    vd.SkyColor = math::v4{ cam.sky_color.x, cam.sky_color.y, cam.sky_color.z, WRCSkyTier() };
    vd.SCLookupCount = sc_lookup_count_;
    vd.SCAtlasSize = sc_atlas_size_;
    vd.SCGridValid = (sc_grid_offsets_ != rhi::handles::INVALID_RESOURCE &&
                      sc_grid_cards_ != rhi::handles::INVALID_RESOURCE) ? 1u : 0u;
    vd.DebugChannel = (WRCDumpEnabled() || trace_stats_enabled_ || bounds_dump_enabled_) ? 1u : 0u;
    // [R0/R1 2026-09-20] SC 查询容差与查询侧 UV pad 经 std140 尾部下发：
    //   x = PRIMAL_WRC_QUERY_TOL（0 = shader 编译期缺省 max(voxel*1.25, 0.9)，
    //       三档 0.45/0.9/1.5 A/B 逐位可复现——路径确定性不受运行时影响）；
    //   y = PRIMAL_SC_QPAD_M（默认 0.45 = SDF 命中带单侧；0 = 关，回滚位）。
    vd.SCQueryParams = math::v4{ 0.0f, 0.45f, 0.0f, 0.0f };
    if (const char* e = std::getenv("PRIMAL_WRC_QUERY_TOL")) {
        const f32 v = static_cast<f32>(std::atof(e));
        if (v > 0.0f && v <= 8.0f) vd.SCQueryParams.x = v;
    }
    if (const char* e = std::getenv("PRIMAL_SC_QPAD_M")) {
        const f32 v = static_cast<f32>(std::atof(e));
        if (v >= 0.0f && v <= 4.0f) vd.SCQueryParams.y = v;
    }
    // [F1-a 2026-09-20 能量不对称修复] z = 查询失败低频回退限幅 k
    //（PRIMAL_WRC_FALLBACK_CLAMP，默认 0.3；0 = 回旧黑语义）。回滚位同 env。
    vd.SCQueryParams.z = 0.3f;
    if (const char* e = std::getenv("PRIMAL_WRC_FALLBACK_CLAMP")) {
        const f32 v = static_cast<f32>(std::atof(e));
        if (v >= 0.0f && v <= 1.0f) vd.SCQueryParams.z = v;
    }
}

void LumenWRCPass::AddPass(
    rendergraph::RenderGraph& graph,
    const WRCCameraData& camera_data,
    u32 current_frame_index,
    rendergraph::RGResourceHandle sc_lighting_atlas_rg)
{
    if (!initialized_) return;

    const auto slots = WRCSlotsForFrame(current_frame_index);
    const u32 frameIdx = slots.writeIndex;
    const u32 probeTotal = GetProbeCountTotal();
    const u32 budget = std::min(params_.update_budget_probes_per_frame, probeTotal);
    const std::string tag = rg_name_prefix_.empty() ? std::string{} : (rg_name_prefix_ + "_");

    graph.AddPass<WRCRGData>(tag + "LumenWRC",
        RGPassType::Compute, RGPassCategory::Lighting,
        [sc_lighting_atlas_rg](WRCRGData& data, RenderGraphBuilder& builder) {
            builder.SideEffect();   // 纯 cache 无像素输出——防 RG 裁剪
            // Vulkan InsertBarrier 当前只实现 image barrier；ResourceHandle
            // 的 buffer/texture 注册表又是独立编号空间。把这三个
            // 持久 buffer 作为 Unknown RG 资源 Write 导入，会让同号
            // texture 被错当成 UA image 转到 GENERAL（实测命中
            // AlbedoTextureArray）。WRC 子通道已有显式 MemoryBarrier，
            // 且本 pass 以 SideEffect 保活，因此 buffer 不进图像状态机；
            // 只保留真正的 SC atlas texture 读依赖。
            // SC atlas 写读屏障声明（cache 填充路径的合法性前提）。
            if (sc_lighting_atlas_rg.IsValid()) {
                builder.Read(sc_lighting_atlas_rg, ResourceState::ShaderResource);
            }
            data.sc_atlas = sc_lighting_atlas_rg;
        },
        [this, camera_data, current_frame_index, frameIdx, probeTotal, budget](
            const WRCRGData& data, RenderGraphContext& context) {
            (void)data;
            auto cmd = context.cmdBuffer;
            if (!cmd) return;
            if (!GIDebug().wrc_trace) return;

            // ---- SDF 数据源解析（离线优先，回退 GlobalSDF 运行时级联）----
            ResourceHandle sdfTextures[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE };
            if (has_offline_sdf_) {
                sdfTextures[0] = sdfTextures[1] = sdfTextures[2] = offline_sdf_texture_;
            } else {
                auto& sdf = nanite::GlobalSDF::Get();
                if (sdf.IsInitialized()) {
                    for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                        const auto& cascade = sdf.GetCascade(c);
                        if (cascade.sdf_texture != handles::INVALID_RESOURCE)
                            sdfTextures[c] = cascade.sdf_texture;
                    }
                }
            }
            if (sdfTextures[0] == handles::INVALID_RESOURCE) return;

            // 占位 2D 纹理一次性 UNDEFINED→SRO 转换——descriptor 以 SRO 写入，
            // 未转换的占位在 submit 校验报 "expects SRO -- instead UNDEFINED"
            //（WRC 路径早期帧告警根因）。
            if (!dummy_2d_transitioned_ && dummy_2d_ != handles::INVALID_RESOURCE) {
                ResourceBarrier b{};
                b.resource = dummy_2d_;
                b.beforeState = ResourceState::Unknown;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                b.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
                dummy_2d_transitioned_ = true;
            }

            // SDF 布局守卫（同 DDGI：强制 SHADER_READ_ONLY 收敛）。
            for (u32 c = 0; c < 3; ++c) {
                if (sdfTextures[c] != handles::INVALID_RESOURCE) {
                    ResourceBarrier b{};
                    b.resource = sdfTextures[c];
                    b.beforeState = ResourceState::ShaderResource;
                    b.afterState = ResourceState::ShaderResource;
                    b.subresource = 0xFFFFFFFF;
                    b.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);
                }
            }

            // ---- volume UBO + 预算 update list（A5 需求驱动优先级窗口）----
            const u32 updateCount = budget;
            {
                WRCVolumeData vd{};
                FillVolumeData(vd, camera_data, current_frame_index);
                vd.ProbeUpdateCount = updateCount;
                if (auto* mapped = static_cast<WRCVolumeData*>(device_->MapBuffer(volume_cb_[frameIdx]))) {
                    *mapped = vd;
                    device_->UnmapBuffer(volume_cb_[frameIdx]);
                }
            }
            {
                // A5 需求驱动预算（UE NumProbesToTraceBudget 精神）：
                //   1) 消费计数 > 0 的探针按计数降序优先入窗（近期被 SPGI
                //      消费 = 下帧大概率仍被消费）；
                //   2) 剩余名额按轮转游标从 0 计数探针中补齐（新探针/冷探针
                //      保底进度）。
                // 消费计数每 256 帧读回一次（ring copy + 2 帧 map，同 dump
                // ring 语义）；计数累计制——排序用相对量，无需重置。
                const u32 copySlot = consume_ring_.slot_for_frame(current_frame_index);
                if (current_frame_index % 256u == 0u && consume_copies_ <= current_frame_index) {
                    cmd->CopyBuffer(consume_counter_, consume_ring_.buffer(copySlot), 0, 0,
                                    u64(probeTotal) * sizeof(u32));
                    consume_copies_ = current_frame_index + 1u;
                }
                if (consume_copies_ > 2u && current_frame_index >= 2u &&
                    last_consume_read_frame_ + 256u <= current_frame_index) {
                    const u32 rb = consume_ring_.slot_for_frame(current_frame_index - 2u);
                    if (void* p = consume_ring_.map(device_, rb)) {
                        const u32* src = static_cast<const u32*>(p);
                        consume_snapshot_.assign(src, src + probeTotal);
                        consume_ring_.unmap(device_, rb);
                        last_consume_read_frame_ = current_frame_index;
                    }
                }

                if (u32* list = static_cast<u32*>(device_->MapBuffer(update_list_[frameIdx]))) {
                    u32 filled = 0;
                    if (!consume_snapshot_.empty() && updateCount < probeTotal) {
                        // (1) 活跃消费探针优先（部分排序，count 降序）。
                        static thread_local std::vector<std::pair<u32, u32>> ranked;
                        ranked.clear();
                        for (u32 i = 0; i < probeTotal; ++i) {
                            if (consume_snapshot_[i] != 0u) ranked.emplace_back(consume_snapshot_[i], i);
                        }
                        if (!ranked.empty()) {
                            const u32 take = std::min<u32>(updateCount, (u32)ranked.size());
                            std::partial_sort(ranked.begin(), ranked.begin() + take, ranked.end(),
                                [](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) {
                                    return a.first > b.first;
                                });
                            static thread_local std::vector<u8> picked;
                            picked.assign(probeTotal, 0u);
                            for (u32 i = 0; i < take; ++i) {
                                list[filled++] = ranked[i].second;
                                picked[ranked[i].second] = 1u;
                            }
                            // (2) 0 计数轮转保底。
                            u32 scanned = 0;
                            while (filled < updateCount && scanned < probeTotal) {
                                const u32 idx = (update_offset_ + scanned) % probeTotal;
                                if (!picked[idx]) list[filled++] = idx;
                                ++scanned;
                            }
                            update_offset_ = (update_offset_ + scanned) % probeTotal;
                        }
                    }
                    if (filled < updateCount) {
                        // 无快照 / 全员活跃 / 预算=全量：纯轮转（A1 起步语义）。
                        for (u32 i = filled; i < updateCount; ++i) {
                            list[i] = (update_offset_ + (i - filled)) % probeTotal;
                        }
                        update_offset_ = (update_offset_ + updateCount) % probeTotal;
                    }
                    device_->UnmapBuffer(update_list_[frameIdx]);
                }
            }

            if (dbg_buf_ != handles::INVALID_RESOURCE) {
                if (auto* d = static_cast<u32*>(device_->MapBuffer(dbg_buf_))) {
                    for (u32 i = 0; i < 16; ++i) d[i] = 0u;
                    device_->UnmapBuffer(dbg_buf_);
                }
            }
            if (bins_buf_ != handles::INVALID_RESOURCE) {
                if (auto* d = static_cast<u32*>(device_->MapBuffer(bins_buf_))) {
                    std::memset(d, 0, 512u * 16u);
                    device_->UnmapBuffer(bins_buf_);
                }
            }
            if (card_fail_buf_ != handles::INVALID_RESOURCE) {
                if (auto* d = static_cast<u32*>(device_->MapBuffer(card_fail_buf_))) {
                    std::memset(d, 0, 4096u * sizeof(u32));
                    device_->UnmapBuffer(card_fail_buf_);
                }
            }

            // ---- dispatch 1: WRCTraceSDF（1 group/probe，64 lanes = K 方向）----
            {
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage, sdfTextures[0]},
                    {1, DescriptorType::SampledImage, sdfTextures[1]},
                    {2, DescriptorType::SampledImage, sdfTextures[2]},
                    {3, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {4, DescriptorType::StorageBuffer, hit_distance_},
                    {5, DescriptorType::StorageBuffer, update_list_[frameIdx]},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], params, 6);
                DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
cmd->BindComputePipeline(trace_pipeline_);
                                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch(updateCount, 1, 1);
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // ---- dispatch 2: WRCTraceFinalize（SC 采样 / 天空契约）----
            if (finalize_pipeline_ != handles::INVALID_PIPELINE) {
                // SC 未启用：binding 仍需有效——占位绑定 1x1 dummy 2D 纹理 /
                // dummy 缓冲（shader 在 SCParams.z=0 时跳过卡片查询，永不读
                // 占位内容；绑 3D SDF 会触发 viewType 验证错误）。
                ResourceHandle atlas = (sc_enabled_ && sc_lighting_atlas_ != handles::INVALID_RESOURCE)
                    ? sc_lighting_atlas_ : dummy_2d_;
                ResourceHandle depthA = (sc_enabled_ && sc_depth_atlas_ != handles::INVALID_RESOURCE)
                    ? sc_depth_atlas_ : dummy_2d_;
                ResourceHandle lookups = sc_card_lookup_ != handles::INVALID_RESOURCE
                    ? sc_card_lookup_ : dummy_buffer_;
                ResourceHandle cards = sc_card_data_ != handles::INVALID_RESOURCE
                    ? sc_card_data_ : dummy_buffer_;
                ResourceHandle gOff = sc_grid_offsets_ != handles::INVALID_RESOURCE
                    ? sc_grid_offsets_ : dummy_buffer_;
                ResourceHandle gCards = sc_grid_cards_ != handles::INVALID_RESOURCE
                    ? sc_grid_cards_ : dummy_buffer_;
                DescriptorData params[] = {
                    {0, DescriptorType::SampledImage, atlas},
                    {1, DescriptorType::SampledImage, depthA},
                    {2, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, hit_distance_},
                    {4, DescriptorType::StorageBuffer, ray_data_},
                    {5, DescriptorType::StorageBuffer, update_list_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, lookups},
                    {7, DescriptorType::StorageBuffer, cards},
                    {8, DescriptorType::StorageBuffer, gOff},
                    {9, DescriptorType::StorageBuffer, gCards},
                    {10, DescriptorType::StorageBuffer,
                        dbg_buf_ != handles::INVALID_RESOURCE ? dbg_buf_ : hit_distance_},
                    {11, DescriptorType::StorageBuffer,
                        bins_buf_ != handles::INVALID_RESOURCE ? bins_buf_ : hit_distance_},
                    {12, DescriptorType::StorageBuffer,
                        card_fail_buf_ != handles::INVALID_RESOURCE ? card_fail_buf_ : hit_distance_},
                };
                UpdateDescriptorSet(device_, finalize_ds_[frameIdx], params, 13);
                DescriptorSetHandle sets[] = { finalize_ds_[frameIdx] };
cmd->BindComputePipeline(finalize_pipeline_);
                                cmd->BindDescriptorSets(PipelineBindPoint::Compute, finalize_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch(updateCount, 1, 1);
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // ---- dispatch 3: WRCUpdateRadiance（EMA + Chebyshev + 入几何排除）----
            {
                DescriptorData params[] = {
                    {2, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, ray_data_},
                    {4, DescriptorType::StorageBuffer, probe_data_},
                    {5, DescriptorType::StorageBuffer, update_list_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, hit_distance_},
                };
                UpdateDescriptorSet(device_, update_ds_[frameIdx], params, 5);
                DescriptorSetHandle sets[] = { update_ds_[frameIdx] };
cmd->BindComputePipeline(update_pipeline_);
                                cmd->BindDescriptorSets(PipelineBindPoint::Compute, update_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((updateCount + 63) / 64, 1, 1);
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // [Q2 排查] SC 查询漏斗：GPU 侧拷贝进 consume_ring（+2 帧读回，
            // 同 rays/dump ring 的 fence 语义；槽位互不冲突见注）。
            if (dbg_buf_ != handles::INVALID_RESOURCE && consume_ring_.is_initialized()) {
                cmd->CopyBuffer(dbg_buf_, consume_ring_.buffer(consume_ring_.slot_for_frame(current_frame_index)), 0, 0, 64);
                cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                                   AccessFlag::TransferWrite, AccessFlag::HostRead);
            }

            // [Q2 排查] SC 查询漏斗读回（+2 帧）。六段细分（R0 2026-09-20）：
            //   boundsFail = boundsFar(卡外) + 贴边获救；queryFail 细分 =
            //   along/perp；kMaxReads 饱和独立计数。计数自洽：
            //   cands = bounds + orient + along + perp + depth + nan + candOK。
            if (dbg_buf_ != handles::INVALID_RESOURCE && consume_ring_.is_initialized() &&
                current_frame_index >= 2u && (current_frame_index % 120u) == 0u) {
                const u32 fRb = consume_ring_.slot_for_frame(current_frame_index - 2u);
                if (void* fp = consume_ring_.map(device_, fRb)) {
                    const u32* d = static_cast<const u32*>(fp);
                    const u64 cands = d[4], total = d[4] ? cands : 1;
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[WRC_SC_FUNNEL] tag="
                        << (rg_name_prefix_.empty() ? "near" : rg_name_prefix_)
                        << " hits=" << d[0] << " gridOk=" << d[1]
                        << " cellFound=" << d[2] << " found=" << d[3]
                        << " | cands=" << d[4] << " boundsFail=" << d[5]
                        << " (" << (100.0 * d[5] / total) << "%)"
                        << " orientFail=" << d[6]
                        << " (" << (100.0 * d[6] / total) << "%)"
                        << " foundRays=" << d[7]
                        << " | bFar=" << d[8] << " bEdgeSaved=" << d[9]
                        << " qAlong=" << d[10] << " qPerp=" << d[11]
                        << " | depthInvalid=" << d[12] << " nanReject=" << d[13]
                        << " readSat=" << d[14] << " candOK=" << d[15]
                        << std::endl;
                    consume_ring_.unmap(device_, fRb);
                }
            }

            // [R1] 拒绝位置分箱读回（PRIMAL_WRC_BOUNDS_DUMP=1；120 帧周期，
            // 独立 ring 避免与漏斗槽位互踩）：打印热点格世界坐标 + 拒绝构成
            // ——区分"未卡化几何"（拒绝聚集在无卡区域）vs"掠射带"（聚集在
            // 卡片边缘/平行面带）。
            if (bounds_dump_enabled_ && bins_buf_ != handles::INVALID_RESOURCE &&
                bins_ring_.is_initialized()) {
                const u32 bSlot = bins_ring_.slot_for_frame(current_frame_index);
                cmd->CopyBuffer(bins_buf_, bins_ring_.buffer(bSlot), 0, 0, 512u * 16u);
                cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                                   AccessFlag::TransferWrite, AccessFlag::HostRead);
                ++bins_copies_;
                if (bins_copies_ > 2u && current_frame_index >= 2u &&
                    (current_frame_index % 120u) == 0u) {
                    const u32 rb = bins_ring_.slot_for_frame(current_frame_index - 2u);
                    if (void* bp = bins_ring_.map(device_, rb)) {
                        struct HotBin { u32 total, bounds, alongperp, idx; };
                        HotBin hot[4] = {};
                        for (u32 b = 0; b < 512u; ++b) {
                            const u32* e = static_cast<const u32*>(bp) + size_t(b) * 4u;
                            if (e[0] == 0u) continue;
                            if (e[0] > hot[3].total) { hot[3] = {e[0], e[1], e[2], b};
                                for (u32 k = 3; k > 0 && hot[k].total > hot[k-1].total; --k)
                                    std::swap(hot[k], hot[k-1]); }
                        }
                        // grid 包围盒从 SC grid offsets header 解出（CPU 侧
                        // 保存一份近场 grid 拷贝代价高——直接以探针场 bounds
                        // 近似：分箱只用于空间定位，绝对框够用）。
                        std::string tops;
                        for (int k = 0; k < 4; ++k) {
                            if (!hot[k].total) break;
                            const u32 b = hot[k].idx;
                            const u32 bx = b & 7u, by = (b >> 3) & 7u, bz = (b >> 6) & 7u;
                            tops += " [bin " + std::to_string(bx) + "," + std::to_string(by)
                                  + "," + std::to_string(bz) + "] total=" + std::to_string(hot[k].total)
                                  + " bounds=" + std::to_string(hot[k].bounds)
                                  + " alongperp=" + std::to_string(hot[k].alongperp);
                        }
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[WRC_BOUNDS_DUMP] tag="
                            << (rg_name_prefix_.empty() ? "near" : rg_name_prefix_)
                            << " top-reject-bins(8³ over SC grid AABB):" << tops
                            << std::endl;
                        bins_ring_.unmap(device_, rb);
                    }
                    // [F4 2026-09-20] 逐卡 boundsFail top-N（同周期读回）：
                    // 回答"哪张卡失败"——卡Idx 经 SC_DUMP_CARDS/SC_CARDVIEW
                    // 交叉定位（atlas 原点/朝向/分辨率）。
                    if (card_fail_buf_ != handles::INVALID_RESOURCE &&
                        card_fail_ring_.is_initialized()) {
                        const u32 cSlot = card_fail_ring_.slot_for_frame(current_frame_index);
                        cmd->CopyBuffer(card_fail_buf_, card_fail_ring_.buffer(cSlot), 0, 0, 4096u * sizeof(u32));
                        cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                                           AccessFlag::TransferWrite, AccessFlag::HostRead);
                        ++card_fail_copies_;
                        if (card_fail_copies_ > 2u && current_frame_index >= 2u &&
                            (current_frame_index % 120u) == 0u) {
                            const u32 crb = card_fail_ring_.slot_for_frame(current_frame_index - 2u);
                            if (void* cp = card_fail_ring_.map(device_, crb)) {
                                struct HotCard { u32 fails, idx; };
                                HotCard hot[8] = {};
                                const u32* cf = static_cast<const u32*>(cp);
                                for (u32 c = 0; c < 4096u; ++c) {
                                    if (cf[c] == 0u) continue;
                                    if (cf[c] > hot[7].fails) {
                                        hot[7] = {cf[c], c};
                                        for (u32 k = 7; k > 0 && hot[k].fails > hot[k-1].fails; --k)
                                            std::swap(hot[k], hot[k-1]);
                                    }
                                }
                                std::string cardTops;
                                for (int k = 0; k < 8; ++k) {
                                    if (!hot[k].fails) break;
                                    cardTops += " card" + std::to_string(hot[k].idx)
                                              + "=" + std::to_string(hot[k].fails);
                                }
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[WRC_CARD_FAIL_DUMP] tag="
                                    << (rg_name_prefix_.empty() ? "near" : rg_name_prefix_)
                                    << " top-boundsFail-cards:" << cardTops
                                    << std::endl;
                                card_fail_ring_.unmap(device_, crb);
                            }
                        }
                    }
                }
            }

            // ---- [Q2 排查] 射线去向构成读回：命中(w>=0)/耗尽(w=-1)/
            // 逃逸天空(w<=-1.5) 逐探针统计（PRIMAL_WRC_TRACE_STATS=1）。
            if (trace_stats_enabled_ && rays_ring_.is_initialized() &&
                rays_copies_ > 2u && current_frame_index >= 2u &&
                (current_frame_index % 120u) == 0u) {
                const u32 rRb = rays_ring_.slot_for_frame(current_frame_index - 2u);
                if (void* rp = rays_ring_.map(device_, rRb)) {
                    const auto* rays = static_cast<const math::v4*>(rp);
                    const u32 K = kWRCDirsPerProbe;
                    u64 nHit = 0, nExh = 0, nEsc = 0, nTot = 0;
                    u32 skyHi = 0, probesValid = 0;   // 逃逸占比 >50% 的探针数
                    double hitLum = 0.0, escLum = 0.0;
                    u64 hitN = 0, escN = 0;
                    for (u32 pi = 0; pi < probeTotal; ++pi) {
                        // 独立有效性判定：从未更新探针的全零射线（w=0 且亮度 0）。
                        u64 esc = 0, zero = 0;
                        for (u32 k = 0; k < K; ++k) {
                            const math::v4 r = rays[pi * K + k];
                            if (r.w == 0.0f && r.x == 0.0f && r.y == 0.0f && r.z == 0.0f) ++zero;
                        }
                        if (zero == K) continue;
                        ++probesValid;
                        for (u32 k = 0; k < K; ++k) {
                            const math::v4 r = rays[pi * K + k];
                            ++nTot;
                            if (r.w >= 0.0f) {
                                ++nHit; ++hitN;
                                hitLum += 0.2126 * r.x + 0.7152 * r.y + 0.0722 * r.z;
                            } else if (r.w <= -1.5f) {
                                ++nEsc; ++escN; ++esc;
                                escLum += 0.2126 * r.x + 0.7152 * r.y + 0.0722 * r.z;
                            } else { ++nExh; }
                        }
                        if (esc * 2u > K) ++skyHi;
                    }
                    rays_ring_.unmap(device_, rRb);
                    if (nTot) {
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[WRC_TRACE_STATS] tag=" << (rg_name_prefix_.empty() ? "near" : rg_name_prefix_)
                            << " probes=" << probesValid
                            << " hit=" << (100.0 * nHit / nTot) << "% exhausted="
                            << (100.0 * nExh / nTot) << "% escape_sky="
                            << (100.0 * nEsc / nTot) << "% | hitLum="
                            << (hitN ? hitLum / hitN : 0.0) << " escLum="
                            << (escN ? escLum / escN : 0.0)
                            << " | probes_escape_gt50%=" << skyHi
                            << "/" << probesValid << std::endl;
                    }
                }
            }

            // ---- dispatch 3.5: WRCCardInject 卡片→探针直投影（反弹修复，
            // SDF 可见性门控需要 sdfTextures 就位）----
            if (card_inject_enabled_ && inject_pipeline_ != handles::INVALID_PIPELINE &&
                card_list_ != handles::INVALID_RESOURCE &&
                sdfTextures[0] != handles::INVALID_RESOURCE &&
                sc_enabled_ && sc_lighting_atlas_ != handles::INVALID_RESOURCE) {
                DescriptorData params[] = {
                    {2, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_data_},
                    {4, DescriptorType::StorageBuffer, sc_card_data_},
                    {5, DescriptorType::SampledImage, sc_lighting_atlas_},
                    {6, DescriptorType::StorageBuffer, card_list_},
                    {7, DescriptorType::SampledImage, sdfTextures[0]},
                    {8, DescriptorType::SampledImage, sdfTextures[1]},
                    {9, DescriptorType::SampledImage, sdfTextures[2]},
                };
                UpdateDescriptorSet(device_, inject_ds_[frameIdx], params, 8);
                cmd->BindComputePipeline(inject_pipeline_);
                DescriptorSetHandle sets[] = { inject_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, inject_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((probeTotal + 63) / 64, 1, 1);
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // ---- dispatch 4（调试/降级 + [P4] DUMP=1 标准指标）----
            if ((prefilter_enabled_ || WRCDumpEnabled()) && prefilter_pipeline_ != handles::INVALID_PIPELINE) {
                DescriptorData params[] = {
                    {2, DescriptorType::UniformBuffer, volume_cb_[frameIdx]},
                    {4, DescriptorType::StorageBuffer, probe_data_},
                    {10, DescriptorType::StorageBuffer, prefilter_sh_},
                };
                UpdateDescriptorSet(device_, prefilter_ds_[frameIdx], params, 3);
                DescriptorSetHandle sets[] = { prefilter_ds_[frameIdx] };
cmd->BindComputePipeline(prefilter_pipeline_);
                                cmd->BindDescriptorSets(PipelineBindPoint::Compute, prefilter_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((probeTotal + 63) / 64, 1, 1);
            }

            // ---- [A2 验收] prefilter SH9 copy（+2 帧读回对照）----
            if ((prefilter_enabled_ || WRCDumpEnabled()) && prefilter_ring_.is_initialized()) {
                const u32 pfCopySlot = prefilter_ring_.slot_for_frame(current_frame_index);
                cmd->CopyBuffer(prefilter_sh_, prefilter_ring_.buffer(pfCopySlot), 0, 0,
                                u64(probeTotal) * 9u * sizeof(math::v4));
                ++prefilter_copies_;
            }

            // ---- [Q2 排查] 射线去向构成 copy（+2 帧读回）----
            if (trace_stats_enabled_ && rays_ring_.is_initialized()) {
                const u32 raysSlot = rays_ring_.slot_for_frame(current_frame_index);
                cmd->CopyBuffer(ray_data_, rays_ring_.buffer(raysSlot), 0, 0,
                                u64(probeTotal) * kWRCDirsPerProbe * sizeof(math::v4));
                ++rays_copies_;
            }

            // ---- GIDebug wrc 通道：PRIMAL_WRC_DUMP=1 radiance dump ----
            // 读回环模式（同 DDGI 深度 ring）：本帧 copy，2 帧后 map——
            // BeginFrame fence 保证同槽写者完成。
            if (WRCDumpEnabled() && dump_ring_.is_initialized()) {
                const u64 bytes = u64(probeTotal) * kWRCProbeStrideU32 * sizeof(u32);
                const u32 copySlot = dump_ring_.slot_for_frame(current_frame_index);
                cmd->CopyBuffer(probe_data_, dump_ring_.buffer(copySlot), 0, 0, bytes);
                ++dump_copies_;
                if (dump_copies_ > 2 && (current_frame_index % 120u) == 0u) {
                    const u32 rbSlot = dump_ring_.slot_for_frame(current_frame_index - 2u);
                    if (void* p = dump_ring_.map(device_, rbSlot)) {
                        const u32* pd = static_cast<const u32*>(p);
                        DumpStats stats;
                        stats.total_probes = probeTotal;
                        double lumSum = 0.0;
                        u32 counted = 0;
                        // [P4 2026-09-21] 角向方差分布（全 64 方向亮度 var/mean）。
                        std::vector<float> angVarRatios;
                        u32 monoCount = 0;
                        auto h16 = [](u32 h) {
                            u32 sign = (h >> 15) & 1u, e2 = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
                            if (e2 == 0u) return 0.0f;
                            u32 bits = (sign << 31) | ((e2 + 112u) << 23) | (m << 13);
                            float f; std::memcpy(&f, &bits, 4); return f;
                        };
                        // [P2 SoA] dump 读回按布局 v2 寻址（radiance/meta 双区域）。
                        const u32 radStride = kWRCProbeRadianceStrideU32;
                        for (u32 pi = 0; pi < probeTotal; ++pi) {
                            const u32 rbase = pi * radStride;
                            const u32 metaBase = probeTotal * radStride + pi * kWRCProbeMetaStrideU32;
                            const u32 state = pd[metaBase + 2u];
                            if ((state & kWRCProbeStateValid) == 0u) continue;
                            if ((state & kWRCProbeStateInGeometry) != 0u) { ++stats.in_geometry_probes; continue; }
                            ++stats.valid_probes;
                            // 8 方向子采样亮度（dump 只为诊断；保持原口径——
                            // meanLum(sub) 是 P2 布局重构的逐位对照基线）。
                            for (u32 d = 0; d < kWRCDirsPerProbe; d += 8u) {
                                u32 lo = pd[rbase + d * 2u];
                                u32 hi = pd[rbase + d * 2u + 1u];
                                float r = h16(lo & 0xFFFFu), g = h16(lo >> 16), b = h16(hi & 0xFFFFu);
                                lumSum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                                ++counted;
                            }
                            // 全 64 方向亮度 mean/var → 归一化比率。
                            float m = 0.0f;
                            float lums[kWRCDirsPerProbe];
                            for (u32 d = 0; d < kWRCDirsPerProbe; ++d) {
                                u32 lo = pd[rbase + d * 2u];
                                u32 hi = pd[rbase + d * 2u + 1u];
                                lums[d] = 0.2126f * h16(lo & 0xFFFFu)
                                        + 0.7152f * h16(lo >> 16)
                                        + 0.0722f * h16(hi & 0xFFFFu);
                                m += lums[d];
                            }
                            m /= float(kWRCDirsPerProbe);
                            if (m > 1e-4f) {
                                float v = 0.0f;
                                for (u32 d = 0; d < kWRCDirsPerProbe; ++d) {
                                    const float dv = lums[d] - m;
                                    v += dv * dv;
                                }
                                v /= float(kWRCDirsPerProbe);
                                const float ratio = v / (m * m);
                                angVarRatios.push_back(ratio);
                                if (ratio < 0.01f) ++monoCount;
                            }
                        }
                        stats.mean_lum = counted ? float(lumSum / counted) : 0.0f;
                        if (!angVarRatios.empty()) {
                            std::sort(angVarRatios.begin(), angVarRatios.end());
                            stats.ang_var_p50 = angVarRatios[angVarRatios.size() / 2];
                            stats.ang_var_p95 = angVarRatios[std::min(
                                (size_t)(angVarRatios.size() * 0.95), angVarRatios.size() - 1)];
                            stats.ang_var_max = angVarRatios.back();
                            stats.mono_fraction = float(monoCount) / float(angVarRatios.size());
                        }
                        dump_ring_.unmap(device_, rbSlot);
                        dump_stats_ = stats;
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[WRC_DUMP] frame=" << current_frame_index
                            << " valid=" << stats.valid_probes << "/" << stats.total_probes
                            << " inGeometry=" << stats.in_geometry_probes
                            << " meanLum(sub)=" << stats.mean_lum << std::endl;
                        // [P4] 角向方差（各向异性健康度）标准输出。
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[WRC_ANGVAR] frame=" << current_frame_index
                            << " probes=" << angVarRatios.size()
                            << " var/mean p50=" << stats.ang_var_p50
                            << " p95=" << stats.ang_var_p95
                            << " max=" << stats.ang_var_max
                            << " mono(<1%)=" << (100.0 * stats.mono_fraction) << "%"
                            << std::endl;
                    }
                }
            }

            // ---- [A2 验收] prefilter SH9 vs 消费端 cos 加权积分——探针级
            // 误差统计（+2 帧读回；[P4] PRIMAL_WRC_DUMP=1 即标准输出，
            // PRIMAL_WRC_PREFILTER 保留为独立 A/B 位）。
            if ((prefilter_enabled_ || WRCDumpEnabled()) && prefilter_ring_.is_initialized() &&
                WRCDumpEnabled() && dump_ring_.is_initialized() &&
                prefilter_copies_ > 2u && current_frame_index >= 2u &&
                (current_frame_index % 120u) == 0u) {
                const u32 pfRb = prefilter_ring_.slot_for_frame(current_frame_index - 2u);
                const u32 dumpSlot = dump_ring_.slot_for_frame(current_frame_index - 2u);
                void* shP = prefilter_ring_.map(device_, pfRb);
                void* pdP = dump_ring_.map(device_, dumpSlot);
                if (shP && pdP) {
                    const auto* sh9 = static_cast<const math::v4*>(shP);
                    const u32* pd = static_cast<const u32*>(pdP);
                    const u32 K = kWRCDirsPerProbe;
                    auto shBasis = [](const math::v3& d, float Y[9]) {
                        const float C0 = 0.282095f, C1 = 0.488603f;
                        Y[0] = C0;
                        Y[1] = -C1 * d.y; Y[2] = C1 * d.z; Y[3] = -C1 * d.x;
                        const float A = 1.092548f, B = 0.315392f, C = 0.546274f;
                        Y[4] = A * d.x * d.y;
                        Y[5] = -A * d.y * d.z;
                        Y[6] = B * (3.0f * d.z * d.z - 1.0f);
                        Y[7] = -A * d.x * d.z;
                        Y[8] = C * (d.x * d.x - d.y * d.y);
                    };
                    auto unpack = [](u32 lo, u32 hi) {
                        auto h16 = [](u32 h) {
                            u32 sign = (h >> 15) & 1u, e2 = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
                            if (e2 == 0u) return 0.0f;
                            u32 bits = (sign << 31) | ((e2 + 112u) << 23) | (m << 13);
                            float f; std::memcpy(&f, &bits, 4); return f;
                        };
                        return math::v3{ h16(lo & 0xFFFFu), h16(lo >> 16), h16(hi & 0xFFFFu) };
                    };
                    std::vector<float> relErrs;
                    u32 worstProbe = 0xFFFFFFFFu;
                    float worstErr = -1.0f;
                    // [P2 SoA] 布局 v2 寻址。
                    const u32 radStride = kWRCProbeRadianceStrideU32;
                    for (u32 pi = 0; pi < probeTotal; ++pi) {
                        const u32 base = pi * radStride;
                        const u32 metaBase = probeTotal * radStride + pi * kWRCProbeMetaStrideU32;
                        const u32 state = pd[metaBase + 2u];
                        if ((state & 1u) == 0u || (state & 2u) != 0u) continue;
                        std::vector<float> lobe(K);
                        float meanLum = 0.0f;
                        for (u32 k = 0; k < K; ++k) {
                            auto dirOf = [&](u32 j) {
                                float y = 1.0f - 2.0f * (float(j) + 0.5f) / float(K);
                                float r = std::sqrt(std::max(1.0f - y * y, 0.0f));
                                float a = 2.399963229728653f * float(j);
                                return math::v3{ std::cos(a) * r, y, std::sin(a) * r };
                            };
                            const math::v3 dk = dirOf(k);
                            float acc0 = 0, acc1 = 0, acc2 = 0, wSum = 0.0f;
                            for (u32 j = 0; j < K; ++j) {
                                const math::v3 dj = dirOf(j);
                                const float c = dk.x * dj.x + dk.y * dj.y + dk.z * dj.z;
                                if (c <= 0.25f) continue;
                                const float w = c * c * c * c * c * c;
                                const math::v3 Lj = unpack(pd[base + j * 2u], pd[base + j * 2u + 1u]);
                                acc0 += Lj.x * w; acc1 += Lj.y * w; acc2 += Lj.z * w;
                                wSum += w;
                            }
                            lobe[k] = (wSum > 0.0f)
                                ? (0.2126f * acc0 + 0.7152f * acc1 + 0.0722f * acc2) / wSum : 0.0f;
                            const math::v3 Lk = unpack(pd[base + k * 2u], pd[base + k * 2u + 1u]);
                            meanLum += 0.2126f * Lk.x + 0.7152f * Lk.y + 0.0722f * Lk.z;
                        }
                        meanLum /= float(K);
                        if (meanLum <= 1e-4f) continue;   // 零能量探针不入统计
                        float errSum = 0.0f;
                        for (u32 k = 0; k < K; ++k) {
                            auto dirOf = [&](u32 j) {
                                float y = 1.0f - 2.0f * (float(j) + 0.5f) / float(K);
                                float r = std::sqrt(std::max(1.0f - y * y, 0.0f));
                                float a = 2.399963229728653f * float(j);
                                return math::v3{ std::cos(a) * r, y, std::sin(a) * r };
                            };
                            const math::v3 dk = dirOf(k);
                            float Y[9];
                            shBasis(dk, Y);
                            float lum = 0.0f;
                            for (u32 i = 0; i < 9u; ++i) {
                                const math::v4& c = sh9[pi * 9u + i];
                                lum += (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z) * Y[i];
                            }
                            errSum += std::abs(lum - lobe[k]);
                        }
                        relErrs.push_back(errSum / (float(K) * std::max(meanLum, 1e-4f)));
                        // [P4] 记录最差探针（坐标可回溯定位：咔嚓面/方向塌缩）。
                        const float relErr = relErrs.back();
                        if (relErr > worstErr) { worstErr = relErr; worstProbe = pi; }
                    }
                    prefilter_ring_.unmap(device_, pfRb);
                    dump_ring_.unmap(device_, dumpSlot);
                    if (!relErrs.empty()) {
                        std::sort(relErrs.begin(), relErrs.end());
                        const float med = relErrs[relErrs.size() / 2];
                        const float p95 = relErrs[std::min(
                            (size_t)(relErrs.size() * 0.95), relErrs.size() - 1)];
                        const float mx = relErrs.back();
                        prefilter_check_.probes_checked = (u32)relErrs.size();
                        prefilter_check_.median_rel_err = med;
                        prefilter_check_.p95_rel_err = p95;
                        prefilter_check_.max_rel_err = mx;
                        // [P4 2026-09-21] 最差探针网格坐标 + P95>15% 阈值门。
                        prefilter_check_.worst_probe = worstProbe;
                        prefilter_check_.p95_over_threshold = p95 > 0.15f;
                        const u32 cx = params_.probe_count_x;
                        const u32 cy = params_.probe_count_y;
                        if (worstProbe != 0xFFFFFFFFu) {
                            prefilter_check_.worst_coords[0] = worstProbe % cx;
                            prefilter_check_.worst_coords[1] = (worstProbe / cx) % cy;
                            prefilter_check_.worst_coords[2] = worstProbe / (cx * cy);
                        }
                        const bool pass = med < 0.05f;
                        // P4: 阈值门 P95>15% 告警（独立于中位 PASS/FAIL 线）。
                        const bool warn95 = prefilter_check_.p95_over_threshold;
                        if (pass && !warn95) {
                            PRIMAL_LOG_STREAM(Info, "Render")
                                << "[WRC_PF_CHECK] frame=" << current_frame_index
                                << " probes=" << relErrs.size()
                                << " median=" << med << " p95=" << p95 << " max=" << mx
                                << "  PASS(<5%)" << std::endl;
                        } else {
                            PRIMAL_LOG_STREAM(Error, "Render")
                                << "[WRC_PF_CHECK] frame=" << current_frame_index
                                << " probes=" << relErrs.size()
                                << " median=" << med << " p95=" << p95 << " max=" << mx
                                << (pass ? "  PASS(<5%)" : "  FAIL(>=5%)")
                                << (warn95 ? "  WARN(P95>15%)" : "")
                                << " worstProbe=" << worstProbe
                                << " coords=(" << prefilter_check_.worst_coords[0]
                                << "," << prefilter_check_.worst_coords[1]
                                << "," << prefilter_check_.worst_coords[2] << ")"
                                << std::endl;
                        }
                    }
                } else {
                    if (shP) prefilter_ring_.unmap(device_, pfRb);
                    if (pdP) dump_ring_.unmap(device_, dumpSlot);
                }
            }
        });
}

} // namespace primal::graphics::lumen
