#include "ReflSpecularTrace.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Lumen/GIWarmup.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

namespace {

struct DescData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

void UpdateDescSet(RHIDeviceBase* device, DescriptorSetHandle set,
                   const DescData* params, uint32_t count) {
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

// 与 shader 侧 ReflTraceParams 逐字节镜像（std140）。
struct ReflTraceParamsGPU {
    math::m4x4 invViewProj;     //   0
    math::v4 camPos;            //  64
    math::v4 screenSize;        //  80
    math::v4 reflParams;        //  96
    math::v4 queryParams;       // 112
    math::v4 SdfOrigins[3];     // 128
    math::v4 SdfVoxelSizes[3];  // 176
    math::v4 SdfExtents[3];     // 224
    u32 SdfRes[3];              // 272
    u32 SdfCount;               // 284
    math::v4 probeOrigin;       // 288
    u32 probeCounts[4];         // 304
    math::v4 skyColor;          // 320
    u32 scParams[4];            // 336
};
static_assert(sizeof(ReflTraceParamsGPU) == 352, "ReflTraceParams std140 layout drift");

} // anonymous namespace

ReflSpecularTracePass::~ReflSpecularTracePass() { Shutdown(); }

bool ReflSpecularTracePass::Initialize(RHIDeviceBase* device, u32 width, u32 height) {
    if (device->GetPlatform() != RHIPlatform::Vulkan) return false;
    device_ = device;
    render_width_ = width;
    render_height_ = height;

    // SPIR-V 直载（与 WRC 同路径——build_spv.sh LUMEN_GLSL 清单已收录）。
    const std::string rel = "Engine/Graphics/Vulkan/Shaders/Lumen/ReflSpecularTrace.comp.spv";
    std::vector<u8> code;
    for (const std::string& path : { rel, utils::ShaderRegistry::ResolveShaderPath(rel) }) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        code.resize(static_cast<size_t>(size));
        if (file.read(reinterpret_cast<char*>(code.data()), size)) break;
        code.clear();
    }
    if (code.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[REFL] Failed to load ReflSpecularTrace.comp.spv" << std::endl;
        return false;
    }
    const ShaderHandle shader = device->CreateShader(code.data(), code.size(),
                                                     ShaderStage::Compute,
                                                     "refl_specular_trace");
    if (shader == handles::INVALID_SHADER) return false;

    {
        DescriptorSetLayoutBinding b[] = {
            {0,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // depth
            {1,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // orm
            {2,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // albedo
            {3,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // ssr
            {4,  DescriptorType::StorageImage,   1, ShaderStage::Compute, nullptr},  // output
            {5,  DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // wrc probes
            {6,  DescriptorType::UniformBuffer,  1, ShaderStage::Compute, nullptr},  // params
            {7,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // sdf0
            {8,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // sdf1
            {9,  DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // sdf2
            {10, DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // sc lighting
            {11, DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // sc depth
            {12, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // sc cards
            {13, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // sc grid offsets
            {14, DescriptorType::StorageBuffer,  1, ShaderStage::Compute, nullptr},  // sc grid cards
            {15, DescriptorType::SampledImage,   1, ShaderStage::Compute, nullptr},  // gbuffer normal
        };
        b[0].readonly = true; b[1].readonly = true; b[2].readonly = true; b[3].readonly = true;
        b[5].readonly = true;
        b[7].is3D = true; b[8].is3D = true; b[9].is3D = true;
        b[10].readonly = true; b[11].readonly = true;
        b[12].readonly = true; b[13].readonly = true; b[14].readonly = true;
        b[15].readonly = true;
        set_layout_ = device->CreateDescriptorSetLayout({16, b});
        PipelineLayoutDesc pl{};
        pl.setLayoutCount = 1;
        pl.setLayouts = &set_layout_;
        layout_ = device->CreatePipelineLayout(pl);
        ComputePipelineDesc d{};
        d.computeShader = shader;
        d.layout = layout_;
        d.threadGroupSize = {8, 8, 1};
        pipeline_ = device->CreateComputePipeline(d);
    }
    if (pipeline_ == handles::INVALID_PIPELINE) return false;

    TextureDesc outDesc{};
    outDesc.size = {width, height, 1};
    outDesc.format = DataFormat::RGBA16_Float;
    outDesc.type = TextureType::Texture2D;
    outDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    for (u32 i = 0; i < 3; ++i) {
        output_tex_[i] = device->CreateTexture(outDesc);
        BufferDesc cbDesc{};
        cbDesc.size = sizeof(ReflTraceParamsGPU);
        cbDesc.type = BufferType::Constant;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_[i] = device->CreateBuffer(cbDesc);
        ds_[i] = device->CreateDescriptorSet({set_layout_});
        if (output_tex_[i] == handles::INVALID_RESOURCE ||
            params_cb_[i] == handles::INVALID_RESOURCE) return false;
    }

    // 占位资源（SC/WRC/SDF 缺席时 binding 恒有效；shader 端 scParams.x=0 /
    // probeCounts=0 / SdfCount=0 短路）。
    TextureDesc dummy2d{};
    dummy2d.size = {1, 1, 1};
    dummy2d.format = DataFormat::RGBA16_Float;
    dummy2d.type = TextureType::Texture2D;
    dummy2d.usage = TextureUsage::ShaderResource;
    dummy_2d_ = device->CreateTexture(dummy2d);
    TextureDesc dummy3d{};
    dummy3d.size = {1, 1, 1};
    dummy3d.format = DataFormat::R32_Float;
    dummy3d.type = TextureType::Texture3D;
    dummy3d.usage = TextureUsage::ShaderResource;
    dummy_3d_ = device->CreateTexture(dummy3d);
    BufferDesc dummyBuf{};
    dummyBuf.size = 16;
    dummyBuf.type = BufferType::Structured;
    dummyBuf.usage = GPUMemoryUsage::Dynamic;
    dummyBuf.memoryUsage = GPUMemoryUsage::Dynamic;
    dummyBuf.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    dummyBuf.structured.elementCount = 4;
    dummyBuf.structured.elementStride = sizeof(u32);
    dummy_buffer_ = device->CreateBuffer(dummyBuf);

    initialized_ = true;
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[REFL] ReflSpecularTrace initialized: " << width << "x" << height
        << " probe_mode=" << (ReflProbeEnabled() ? "on" : "off") << std::endl;
    return true;
}

void ReflSpecularTracePass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

RGResourceHandle ReflSpecularTracePass::AddPass(RenderGraph& graph, const FrameData& data, u32 frame_index) {
    if (!initialized_) return {};
    const u32 slot = frame_index % 3u;

    auto outRG = graph.ImportResource("ReflSpecular_Output", output_tex_[slot]);

    struct ReflRGData { RGResourceHandle output; };
    graph.AddPass<ReflRGData>("ReflSpecularTrace", RGPassType::Compute, RGPassCategory::Lighting,
        [outRG, &data](ReflRGData& rg, RenderGraphBuilder& builder) {
            builder.SideEffect();   // fusion 同帧采样——防 RG 裁剪
            rg.output = outRG;
            builder.Write(outRG, ResourceState::UnorderedAccess);
            // 输入读边（GBuffer/SSR 与本 pass 的定序；物理句柄绑描述符）。
            if (data.depth_rg.IsValid()) builder.Read(data.depth_rg, ResourceState::ShaderResource);
            if (data.albedo_rg.IsValid()) builder.Read(data.albedo_rg, ResourceState::ShaderResource);
            if (data.orm_rg.IsValid()) builder.Read(data.orm_rg, ResourceState::ShaderResource);
            if (data.ssr_rg.IsValid()) builder.Read(data.ssr_rg, ResourceState::ShaderResource);
        },
        [this, data, slot, frame_index](const ReflRGData&, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            // ---- SDF 数据源解析（离线优先，回退 GlobalSDF）----
            ResourceHandle sdfTextures[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE };
            math::v4 sdfOrigins[3] = {};
            math::v4 sdfVoxel[3] = {};
            math::v4 sdfExtents[3] = {};
            u32 sdfRes[3] = {0, 0, 0};
            u32 sdfCount = 0;
            if (data.offline_sdf != handles::INVALID_RESOURCE && data.offline_sdf_resolution > 0) {
                sdfTextures[0] = sdfTextures[1] = sdfTextures[2] = data.offline_sdf;
                for (u32 c = 0; c < 3; ++c) {
                    sdfOrigins[c] = math::v4{data.offline_sdf_origin.x, data.offline_sdf_origin.y,
                                             data.offline_sdf_origin.z, 0.0f};
                    sdfExtents[c] = math::v4{data.offline_sdf_extent.x, data.offline_sdf_extent.y,
                                             data.offline_sdf_extent.z, 0.0f};
                    const f32 voxel = data.offline_sdf_extent.x / float(data.offline_sdf_resolution);
                    sdfVoxel[c] = math::v4{voxel, 0.0f, 0.0f, 0.0f};
                    sdfRes[c] = data.offline_sdf_resolution;
                }
                sdfCount = 1;
            } else {
                auto& sdf = nanite::GlobalSDF::Get();
                if (sdf.IsInitialized()) {
                    const u32 cc = std::min(3u, sdf.GetConfig().cascade_count);
                    for (u32 c = 0; c < cc; ++c) {
                        const auto& cascade = sdf.GetCascade(c);
                        if (cascade.sdf_texture == handles::INVALID_RESOURCE) continue;
                        sdfTextures[c] = cascade.sdf_texture;
                        sdfOrigins[c] = math::v4{cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                        sdfExtents[c] = math::v4{cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                        sdfVoxel[c] = math::v4{cascade.voxel_size, 0.0f, 0.0f, 0.0f};
                        sdfRes[c] = cascade.resolution;
                    }
                    sdfCount = cc;
                }
            }
            if (sdfTextures[0] == handles::INVALID_RESOURCE) {
                // 无 SDF 可走：写零输出（fusion 落回 relay），不做 dispatch。
                return;
            }

            // 占位 2D/3D 一次性 UNDEFINED→SRO（同 WRC 首帧告警根因预防）。
            if (!dummy_transitioned_) {
                ResourceBarrier bars[2]{};
                u32 nb = 0;
                if (dummy_2d_ != handles::INVALID_RESOURCE) {
                    bars[nb].resource = dummy_2d_;
                    bars[nb].beforeState = ResourceState::Unknown;
                    bars[nb].afterState = ResourceState::ShaderResource;
                    bars[nb].subresource = 0xFFFFFFFF;
                    bars[nb].queueFamily = 0xFFFFFFFF;
                    ++nb;
                }
                if (dummy_3d_ != handles::INVALID_RESOURCE) {
                    bars[nb].resource = dummy_3d_;
                    bars[nb].beforeState = ResourceState::Unknown;
                    bars[nb].afterState = ResourceState::ShaderResource;
                    bars[nb].subresource = 0xFFFFFFFF;
                    bars[nb].queueFamily = 0xFFFFFFFF;
                    ++nb;
                }
                if (nb) cmd->InsertBarrier(bars, nb);
                dummy_transitioned_ = true;
            }
            // 输出纹理 GENERAL→(compute 写) 由 Write 声明定序；首帧
            // UNDEFINED→UnorderedAccess 由 RG tracked barrier 解析。

            // ---- params UBO ----
            {
                ReflTraceParamsGPU p{};
                p.invViewProj = data.inv_view_proj;
                p.camPos = math::v4{data.camera_pos.x, data.camera_pos.y, data.camera_pos.z, 0.0f};
                p.screenSize = math::v4{f32(data.width ? data.width : render_width_),
                                        f32(data.height ? data.height : render_height_),
                                        1.0f / f32(data.width ? data.width : render_width_),
                                        1.0f / f32(data.height ? data.height : render_height_)};
                const u32 mode = ReflProbeEnabled() ? 2u : 1u;   // 1=card only, 2=card+probe
                p.reflParams = math::v4{f32(mode), 0.7f /*ssrW gate*/, 0.85f /*maxRough*/,
                                        data.ray_max_distance};
                // 查询容差/pad 与 WRC finalize 同源 env（口径一致）。
                f32 tol = 0.0f, qpad = 0.45f;
                if (const char* e = std::getenv("PRIMAL_WRC_QUERY_TOL")) {
                    const f32 v = static_cast<f32>(std::atof(e));
                    if (v > 0.0f && v <= 8.0f) tol = v;
                }
                if (const char* e = std::getenv("PRIMAL_SC_QPAD_M")) {
                    const f32 v = static_cast<f32>(std::atof(e));
                    if (v >= 0.0f && v <= 4.0f) qpad = v;
                }
                p.queryParams = math::v4{tol, qpad, data.sky_tier, data.signed_sdf ? 1.0f : 0.0f};
                for (u32 c = 0; c < 3; ++c) {
                    p.SdfOrigins[c] = sdfOrigins[c];
                    p.SdfVoxelSizes[c] = sdfVoxel[c];
                    p.SdfExtents[c] = sdfExtents[c];
                }
                p.SdfRes[0] = sdfRes[0]; p.SdfRes[1] = sdfRes[1];
                p.SdfRes[2] = sdfRes[2]; p.SdfCount = sdfCount;
                p.probeOrigin = math::v4{data.wrc.origin.x, data.wrc.origin.y,
                                         data.wrc.origin.z, data.wrc.spacing};
                p.probeCounts[0] = u32(data.wrc.counts.x);
                p.probeCounts[1] = u32(data.wrc.counts.y);
                p.probeCounts[2] = u32(data.wrc.counts.z);
                p.probeCounts[3] = data.wrc.dirs_per_probe;
                p.skyColor = math::v4{data.sky_color.x, data.sky_color.y,
                                      data.sky_color.z, data.sky_tier};
                p.scParams[0] = (data.sc.IsValid() &&
                                 data.sc.card_data != handles::INVALID_RESOURCE) ? 1u : 0u;
                if (auto* m = static_cast<ReflTraceParamsGPU*>(
                        device_->MapBuffer(params_cb_[slot]))) {
                    *m = p;
                    device_->UnmapBuffer(params_cb_[slot]);
                }
            }

            // ---- 描述符（缺席源 → 占位；shader 短路）----
            const bool scOk = data.sc.IsValid() &&
                              data.sc.card_data != handles::INVALID_RESOURCE;
            const bool wrcOk = data.wrc.probe_data != handles::INVALID_RESOURCE &&
                               data.wrc.probe_count_total > 0;
            DescData params[] = {
                {0,  DescriptorType::SampledImage,   data.depth != handles::INVALID_RESOURCE
                    ? data.depth : dummy_2d_},
                {1,  DescriptorType::SampledImage,   data.orm != handles::INVALID_RESOURCE
                    ? data.orm : dummy_2d_},
                {2,  DescriptorType::SampledImage,   data.albedo != handles::INVALID_RESOURCE
                    ? data.albedo : dummy_2d_},
                {3,  DescriptorType::SampledImage,   data.ssr != handles::INVALID_RESOURCE
                    ? data.ssr : dummy_2d_},
                {4,  DescriptorType::StorageImage,   output_tex_[slot]},
                {5,  DescriptorType::StorageBuffer,  wrcOk ? data.wrc.probe_data : dummy_buffer_},
                {6,  DescriptorType::UniformBuffer,  params_cb_[slot]},
                {7,  DescriptorType::SampledImage,   sdfTextures[0]},
                {8,  DescriptorType::SampledImage,   sdfTextures[1] != handles::INVALID_RESOURCE
                    ? sdfTextures[1] : dummy_3d_},
                {9,  DescriptorType::SampledImage,   sdfTextures[2] != handles::INVALID_RESOURCE
                    ? sdfTextures[2] : dummy_3d_},
                {10, DescriptorType::SampledImage,   scOk ? data.sc.lighting_atlas : dummy_2d_},
                {11, DescriptorType::SampledImage,   scOk ? data.sc.depth_atlas : dummy_2d_},
                {12, DescriptorType::StorageBuffer,  scOk ? data.sc.card_data : dummy_buffer_},
                {13, DescriptorType::StorageBuffer,  scOk ? data.sc.grid_offsets : dummy_buffer_},
                {14, DescriptorType::StorageBuffer,  scOk ? data.sc.grid_cards : dummy_buffer_},
                {15, DescriptorType::SampledImage,   data.normal != handles::INVALID_RESOURCE
                    ? data.normal : dummy_2d_},
            };
            UpdateDescSet(device_, ds_[slot], params, 16);
            cmd->BindComputePipeline(pipeline_);
            const DescriptorSetHandle sets[] = { ds_[slot] };
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, layout_, 0, 1, sets, 0, nullptr);
            const u32 w = data.width ? data.width : render_width_;
            const u32 h = data.height ? data.height : render_height_;
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                               AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
        });
    return outRG;
}

} // namespace primal::graphics::lumen
