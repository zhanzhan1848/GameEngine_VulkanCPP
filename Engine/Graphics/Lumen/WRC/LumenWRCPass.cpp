#include "LumenWRCPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Lumen/GIWarmup.h"
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
    RGResourceHandle probe_data{};
    RGResourceHandle ray_data{};
    RGResourceHandle hit_distance{};
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
    // [A2 验收] prefilter SH9 读回环（prefilter 开启时启用）。Initialize 早于
    // 管线侧 SetPrefilterEnabled 调用——此处直接读 env 同源判定。
    prefilter_enabled_ = prefilter_enabled_ || [] {
        const char* e = std::getenv("PRIMAL_WRC_PREFILTER");
        return e && e[0] == '1';
    }();
    if (prefilter_enabled_) {
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
        };
        b[1].readonly = true; b[3].readonly = true;
        b[5].readonly = true; b[6].readonly = true;
        b[7].readonly = true; b[8].readonly = true; b[9].readonly = true;
        finalize_set_layout_ = device_->CreateDescriptorSetLayout({10, b});
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
        b[3].readonly = true; b[5].readonly = true; b[6].readonly = true;
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
        b[4].readonly = true;
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
    // valid 位全清：EMA 首帧改直写（A1 首填语义），场景切换不继承旧 GI。
    probe_field_seeded_ = false;
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
    vd.SkyColor = math::v4{ cam.sky_color.x, cam.sky_color.y, cam.sky_color.z, kWRCSkyTier };
    vd.SCLookupCount = sc_lookup_count_;
    vd.SCAtlasSize = sc_atlas_size_;
    vd.SCGridValid = (sc_grid_offsets_ != rhi::handles::INVALID_RESOURCE &&
                      sc_grid_cards_ != rhi::handles::INVALID_RESOURCE) ? 1u : 0u;
    vd.DebugChannel = WRCDumpEnabled() ? 1u : 0u;
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

    auto probeHandle = graph.ImportResource(tag + "LumenWRC_ProbeData", probe_data_);
    auto rayHandle = graph.ImportResource(tag + "LumenWRC_RayData", ray_data_);
    auto hitHandle = graph.ImportResource(tag + "LumenWRC_HitDistance", hit_distance_);

    graph.AddPass<WRCRGData>(tag + "LumenWRC",
        RGPassType::Compute, RGPassCategory::Lighting,
        [probeHandle, rayHandle, hitHandle, sc_lighting_atlas_rg](WRCRGData& data, RenderGraphBuilder& builder) {
            builder.SideEffect();   // 纯 cache 无像素输出——防 RG 裁剪
            data.probe_data = probeHandle;
            builder.Write(probeHandle, ResourceState::UnorderedAccess);
            data.ray_data = rayHandle;
            builder.Write(rayHandle, ResourceState::UnorderedAccess);
            data.hit_distance = hitHandle;
            builder.Write(hitHandle, ResourceState::UnorderedAccess);
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
                };
                UpdateDescriptorSet(device_, finalize_ds_[frameIdx], params, 10);
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

            // ---- dispatch 4（调试/降级，PRIMAL_WRC_PREFILTER=1）----
            if (prefilter_enabled_ && prefilter_pipeline_ != handles::INVALID_PIPELINE) {
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
            if (prefilter_enabled_ && prefilter_ring_.is_initialized()) {
                const u32 pfCopySlot = prefilter_ring_.slot_for_frame(current_frame_index);
                cmd->CopyBuffer(prefilter_sh_, prefilter_ring_.buffer(pfCopySlot), 0, 0,
                                u64(probeTotal) * 9u * sizeof(math::v4));
                ++prefilter_copies_;
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
                        for (u32 pi = 0; pi < probeTotal; ++pi) {
                            const u32 base = pi * kWRCProbeStrideU32;
                            const u32 state = pd[base + kWRCDirsPerProbe * 2u + 2u];
                            if ((state & kWRCProbeStateValid) == 0u) continue;
                            if ((state & kWRCProbeStateInGeometry) != 0u) { ++stats.in_geometry_probes; continue; }
                            ++stats.valid_probes;
                            // 8 方向子采样亮度（dump 只为诊断）。
                            for (u32 d = 0; d < kWRCDirsPerProbe; d += 8u) {
                                u32 lo = pd[base + d * 2u];
                                u32 hi = pd[base + d * 2u + 1u];
                                auto h16 = [](u32 h) {
                                    u32 sign = (h >> 15) & 1u, e2 = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
                                    if (e2 == 0u) return 0.0f;
                                    u32 bits = (sign << 31) | ((e2 + 112u) << 23) | (m << 13);
                                    float f; std::memcpy(&f, &bits, 4); return f;
                                };
                                float r = h16(lo & 0xFFFFu), g = h16(lo >> 16), b = h16(hi & 0xFFFFu);
                                lumSum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                                ++counted;
                            }
                        }
                        stats.mean_lum = counted ? float(lumSum / counted) : 0.0f;
                        dump_ring_.unmap(device_, rbSlot);
                        dump_stats_ = stats;
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[WRC_DUMP] frame=" << current_frame_index
                            << " valid=" << stats.valid_probes << "/" << stats.total_probes
                            << " inGeometry=" << stats.in_geometry_probes
                            << " meanLum(sub)=" << stats.mean_lum << std::endl;
                    }
                }
            }

            // ---- [A2 验收] prefilter SH9 vs 消费端 cos 加权积分——探针级
            // 误差统计（+2 帧读回；PRIMAL_WRC_PREFILTER=1 + PRIMAL_WRC_DUMP=1）。
            if (prefilter_enabled_ && prefilter_ring_.is_initialized() &&
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
                    for (u32 pi = 0; pi < probeTotal; ++pi) {
                        const u32 base = pi * kWRCProbeStrideU32;
                        const u32 state = pd[base + K * 2u + 2u];
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
                        const bool pass = med < 0.05f;
                        if (pass) {
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
                                << "  FAIL(>=5%)" << std::endl;
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
