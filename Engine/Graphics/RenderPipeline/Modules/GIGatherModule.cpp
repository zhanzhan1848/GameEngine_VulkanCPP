#include "GIGatherModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

#include <cstring>
#include <iostream>

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

struct GIGatherCB {
    math::m4x4 inv_view_projection;
    math::v4   probe_origin_spacing;
    math::v4   probe_counts;
    math::v4   camera_pos;  // T4.3: world-space camera for reflection vectors
    // T6.3 sub-A: probe_vis 数据面。debug_vis.x = GIDebug().probe_vis 模式
    // （0=Off）。经 binding 6（48B range，第三 vec4 = offset 112）上传；
    // camera_pos 槽位（offset 96）在 shader 内是 binding 6 的占位镜像
    // （未读），布局保持前四字段逐字节不变。
    math::v4   debug_vis;
};

// T6.3 sub-B: N-volume 参数 CB（binding 18，std140 布局与
// DDGIGIGather.comp 的 NVolumeCB 一致：3 个 vec4 数组 + meta）。
struct NVolumeCB {
    math::v4 origin_spacing[4];   // xyz = origin, w = spacing
    math::v4 counts_enabled[4];   // xyz = counts, w = enabled
    math::v4 meta;                // x = 有效 volume 数（0 = legacy 路径）
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
                                 u32 render_width, u32 render_height) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

    if (shader == handles::INVALID_SHADER) return false;

    // 15 bindings: 6 textures + 9 buffers.
    // Metal: overlapping texture/buffer namespaces (idiomatic Metal).
    // Vulkan: flat 0..14 (depth as SampledDepthImage) — matches GLSL shader.
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;
    // T6.3 sub-B: descriptor indexing 能力位（VulkanDevice 查询并启用了
    // descriptorBindingPartiallyBound + shaderStorageBufferArrayNonUniformIndexing）。
    // Metal/Dawn 继承默认 false → N-volume 分支不存在，两槽路径不变。
    nvolume_supported_ = isVk && device->SupportsDescriptorIndexing();
    if (isVk) {
        DescriptorSetLayoutBinding vkBindings[] = {
            {0,  DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // depth
            {1,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // normal
            {2,  DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // output
            {3,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // history
            {4,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // invViewProj+cameraPos
            {5,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {6,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeCounts (+debugVis, 48B)
            {7,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkySH
            {8,  DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // staticSkyFactor
            {9,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // staticProbeParams
            {10, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // confidenceBuffer
            {11, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // irradianceBuffer
            {12, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // depthBuffer
            {13, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // gbuffer_orm (T4.3)
            {14, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // specular output (T4.3)
            {15, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // far volume CB (T4.4)
            {16, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // far irradiance (T4.4)
            // T6.3 sub-B: N-volume irradiance 数组（4 槽）。能力可用时
            // PartiallyBound（volume 数 < 4 允许未写槽——本模块仍全写
            // dummy 保平局安全）；不可用时普通数组（shader 静态使用存在，
            // 全槽必写，meta.x=0 永不进入数组访问分支）。
            {17, DescriptorType::StorageBuffer,     4, ShaderStage::Compute, nullptr,
             nvolume_supported_ ? DescriptorBindingFlags::PartiallyBound
                                : DescriptorBindingFlags::None},
            {18, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // NVolumeCB (T6.3 sub-B)
        };
        set_layout_ = device->CreateDescriptorSetLayout({19, vkBindings});
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
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradianceBuffer
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // depthBuffer
        };
        set_layout_ = device->CreateDescriptorSetLayout({15, bindings});
    }
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    ComputePipelineDesc pd{};
    pd.computeShader = shader;
    pd.layout = layout_;
    pd.threadGroupSize = {8, 8, 1};
    pipeline_ = device->CreateComputePipeline(pd);
    if (pipeline_ == handles::INVALID_PIPELINE) return false;

    descriptor_set_ = device->CreateDescriptorSet({set_layout_});

    // Half-res output textures (RGBA16_Float)
    u32 halfW = render_width / 2;
    u32 halfH = render_height / 2;
    TextureDesc texDesc{};
    texDesc.size = {halfW, halfH, 1};
    texDesc.format = DataFormat::RGBA16_Float;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    gi_halfres_texture_ = device->CreateTexture(texDesc);
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    gi_halfres_history_ = device->CreateTexture(texDesc);
    // T4.3: half-res glossy specular output (RGB = SH(R) radiance, A = NdotV)
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    gi_specular_texture_ = device->CreateTexture(texDesc);

    // Constant buffers — need UniformBuffer bind flag for UBO descriptors.
    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.type = BufferType::Constant;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    cbDesc.usage = GPUMemoryUsage::Dynamic;
    cbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    probe_cb_ = device->CreateBuffer(cbDesc);
    // T4.4: far-volume params (origin+spacing, counts) — same 256B shape.
    far_cb_ = device->CreateBuffer(cbDesc);
    // T6.3 sub-B: N-volume 参数 CB（144B 实际用量，256B 槽位与 far_cb_ 同形）。
    nvolumes_cb_ = device->CreateBuffer(cbDesc);

    BufferDesc spCbDesc{};
    spCbDesc.size = 512;
    spCbDesc.type = BufferType::Constant;
    spCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    spCbDesc.usage = GPUMemoryUsage::Dynamic;
    spCbDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
    static_probe_cb_ = device->CreateBuffer(spCbDesc);

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

void GIGatherModule::Shutdown() {}

GIGatherOutputs GIGatherModule::AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs) {
    GIGatherOutputs outputs{};
    if (!inputs.ddgi_pass || pipeline_ == handles::INVALID_PIPELINE) return outputs;

    outputs.gi_output_tex = gi_halfres_texture_;
    outputs.gi_output_rg = graph.ImportResource("DDGIHalfResGI", gi_halfres_texture_);
    outputs.gi_specular_tex = gi_specular_texture_;
    outputs.gi_specular_rg = graph.ImportResource("DDGIHalfResGlossy", gi_specular_texture_);

    u32 ddgiReadIdx = (inputs.current_buffer_index + 2) % 3;

    // Import DDGI buffers
    auto irrHandle = graph.ImportResource("DDGIIrrRead", inputs.ddgi_pass->GetIrradianceBuffer(ddgiReadIdx));
    auto depthHandle = graph.ImportResource("DDGIDepthRead", inputs.ddgi_pass->GetDepthBuffer(ddgiReadIdx));
    // T4.4: far volume read dependency (ordering vs. its trace pass).
    if (inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
        graph.ImportResource("DDGIFarIrrRead", inputs.ddgi_far_pass->GetIrradianceBuffer(ddgiReadIdx));
    }
    // T6.3 sub-B: 附加 volume（索引 ≥2）的读依赖注册——与 far import 同模式，
    // 把第 3/4 个 volume 的 irradiance 纳入本帧图（别名注册 + 与其 trace pass
    // 的次序约束）。volume[0]/[1] 已由上方 near/far import 覆盖。
    if (nvolume_supported_ && inputs.volume_count > 2) {
        for (u32 vi = 2; vi < inputs.volume_count && vi < GIGatherInputs::kMaxVolumes; ++vi) {
            if (inputs.volume_views[vi].IsValid())
                graph.ImportResource("DDGINvolIrrRead" + std::to_string(vi),
                                     inputs.volume_views[vi].irradiance);
        }
    }

    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<PassData>("DDGIGIGather",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [outRG = outputs.gi_output_rg, specRG = outputs.gi_specular_rg,
         depthRG = inputs.gbuffer_depth_rg, normalRG = inputs.gbuffer_normal_rg, ormRG = inputs.gbuffer_orm_rg]
        (PassData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling — outputs go to imported textures
            if (depthRG.IsValid()) builder.Read(depthRG, ResourceState::ShaderResource);
            if (normalRG.IsValid()) builder.Read(normalRG, ResourceState::ShaderResource);
            if (ormRG.IsValid()) builder.Read(ormRG, ResourceState::ShaderResource);
            data.output = builder.Write(outRG, ResourceState::UnorderedAccess);
            builder.Write(specRG, ResourceState::UnorderedAccess);
        },
        [this, inputs, ddgiReadIdx](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            auto ddgi = inputs.ddgi_pass;

            // X2/A3: 远场 volume 数据优先走 view；view 未填时回退旧 pass 指针
            // 路径。两条来源字段一致（origin/counts/spacing/irradiance 同源同
            // 槽位），CB 上传与 barrier/descriptor 取数逐字节等价。
            lumen::DDGIVolumeView farView = inputs.far_volume_view;
            if (!farView.IsValid() && inputs.ddgi_far_pass && inputs.ddgi_far_pass->IsInitialized()) {
                farView = inputs.ddgi_far_pass->GetVolumeView(ddgiReadIdx);
            }

            // T6.3 sub-B: 组装 N-volume 视图表。inputs.volume_views 优先（管线
            // 侧把 ddgi_volumes_[0..count) 全量传入）；[0]/[1] 缺省时回退
            // near pass / farView——与 legacy 槽同源同槽位，N-volume 激活时
            // 输出与两槽路径一致。nvolumeActive 仅在能力位开 + ≥2 个有效
            // volume 时为真；否则 CB meta.x = 0，shader 走 legacy 分支。
            lumen::DDGIVolumeView nvolViews[GIGatherInputs::kMaxVolumes]{};
            u32 nvolCount = 0;
            {
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi) {
                    if (vi < inputs.volume_count && inputs.volume_views[vi].IsValid())
                        nvolViews[vi] = inputs.volume_views[vi];
                }
                if (!nvolViews[0].IsValid())
                    nvolViews[0] = ddgi->GetVolumeView(ddgiReadIdx);
                if (!nvolViews[1].IsValid() && farView.IsValid())
                    nvolViews[1] = farView;
                for (u32 vi = 0; vi < GIGatherInputs::kMaxVolumes; ++vi)
                    if (nvolViews[vi].IsValid()) nvolCount = vi + 1;
            }
            const bool nvolumeActive = nvolume_supported_ && nvolCount >= 2;
            // 路径选择一次性日志（SDFViz 先例）——N-volume 激活/回退可观测。
            static bool loggedGatherPath = false;
            if (!loggedGatherPath) {
                loggedGatherPath = true;
                std::cerr << "[GIGather] path = "
                          << (nvolumeActive ? "N-volume (descriptor indexing, "
                                             : "legacy two-slot (")
                          << (nvolume_supported_ ? "capability on" : "capability off")
                          << ", volumes=" << nvolCount << ")" << std::endl;
            }

            // Upload GIGatherCB
            {
                math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
                math::m4x4 invView = Inverse(inputs.view_matrix);
                auto* cb = static_cast<GIGatherCB*>(device_->MapBuffer(probe_cb_));
                if (cb) {
                    cb->inv_view_projection = Inverse(vp);
                    const auto& vol = ddgi->GetVolumeData();
                    const auto& params = ddgi->GetParams();
                    cb->probe_origin_spacing = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, params.probe_spacing};
                    cb->probe_counts = math::v4{(float)params.probe_count_x, (float)params.probe_count_y, (float)params.probe_count_z, 9.0f};
                    cb->camera_pos = math::v4{invView.columns[3][0], invView.columns[3][1], invView.columns[3][2], 1.0f};
                    // T6.3 sub-A: probe_vis 数据面——GIDebug() 热调旋钮进 CB，
                    // shader binding 6 第三 vec4（offset 112）。Off（默认）时
                    // x=0，gather 输出零变化。
                    cb->debug_vis = math::v4{static_cast<float>(
                        static_cast<u32>(lumen::GIDebug().probe_vis)), 0.0f, 0.0f, 0.0f};
                    device_->UnmapBuffer(probe_cb_);
                }
            }

            // T4.4: upload far-volume params (origin+spacing / counts / flags)
            // X2/A3: 数据源改为 farView（view 优先、pass 兜底），上传值不变。
            {
                auto* fb = static_cast<math::v4*>(device_->MapBuffer(far_cb_));
                if (fb) {
                    memset(fb, 0, 64);
                    if (farView.IsValid()) {
                        fb[0] = math::v4{farView.origin.x, farView.origin.y, farView.origin.z, farView.spacing};
                        fb[1] = math::v4{farView.counts.x, farView.counts.y, farView.counts.z, 1.0f}; // w = enabled
                    }
                    device_->UnmapBuffer(far_cb_);
                }
            }

            // T6.3 sub-B: upload N-volume params（binding 18）。nvolumeActive
            // 时写入 volume[0..nvolCount) 的 origin/spacing/counts（w=enabled）
            // + meta.x = nvolCount；否则整块清零（meta.x=0 → legacy 路径）。
            // 无效中段槽位（counts=0/w=0）被 shader 的 enabled 检查跳过。
            {
                auto* nv = static_cast<NVolumeCB*>(device_->MapBuffer(nvolumes_cb_));
                if (nv) {
                    memset(nv, 0, sizeof(NVolumeCB));
                    if (nvolumeActive) {
                        for (u32 vi = 0; vi < nvolCount; ++vi) {
                            const auto& v = nvolViews[vi];
                            nv->origin_spacing[vi] = math::v4{v.origin.x, v.origin.y, v.origin.z, v.spacing};
                            nv->counts_enabled[vi] = math::v4{v.counts.x, v.counts.y, v.counts.z, 1.0f};
                        }
                        nv->meta = math::v4{(float)nvolCount, 0.0f, 0.0f, 0.0f};
                    }
                    device_->UnmapBuffer(nvolumes_cb_);
                }
            }

            // Upload static probe params
            {
                auto* sp = static_cast<GPUStaticProbeData*>(device_->MapBuffer(static_probe_cb_));
                if (sp) {
                    memset(sp, 0, sizeof(GPUStaticProbeData));
                    if (inputs.static_probe_volume) {
                        const auto& vol = ddgi->GetVolumeData();
                        sp->ProbeOrigin = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, 0.0f};
                        sp->ProbeSpacing = ddgi->GetParams().probe_spacing;
                    }
                    device_->UnmapBuffer(static_probe_cb_);
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
                ResourceBarrier barriers[8]{};  // near irr/depth/conf + far + nvol[2..3]
                int bc = 0;
                auto irrBuf = ddgi->GetIrradianceBuffer(ddgiReadIdx);
                auto depthBuf = ddgi->GetDepthBuffer(ddgiReadIdx);
                auto confBuf = ddgi->GetConfidenceBuffer(ddgiReadIdx);
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
                // T4.4: far volume irradiance (X2/A3: 取数改走 farView，句柄同源)
                if (farView.IsValid() && farView.irradiance != handles::INVALID_RESOURCE) {
                    barriers[bc].resource = farView.irradiance;
                    barriers[bc].beforeState = ResourceState::UnorderedAccess;
                    barriers[bc].afterState = ResourceState::ShaderResource;
                    barriers[bc].subresource = 0xFFFFFFFF;
                    bc++;
                }
                // T6.3 sub-B: 附加 volume（vi ≥ 2）irradiance 同样 UAV→SRV。
                // volume[0]/[1] 已由上方 near/far 路径覆盖（同句柄）。
                if (nvolumeActive) {
                    for (u32 vi = 2; vi < nvolCount && vi < GIGatherInputs::kMaxVolumes; ++vi) {
                        if (nvolViews[vi].irradiance == handles::INVALID_RESOURCE) continue;
                        barriers[bc].resource = nvolViews[vi].irradiance;
                        barriers[bc].beforeState = ResourceState::UnorderedAccess;
                        barriers[bc].afterState = ResourceState::ShaderResource;
                        barriers[bc].subresource = 0xFFFFFFFF;
                        bc++;
                    }
                }
                if (bc > 0) cmd->InsertBarrier(barriers, bc);
            }

            // Update descriptor set
            ResourceHandle skySH = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkySH() : handles::INVALID_RESOURCE;
            ResourceHandle skyFactor = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkyFactor() : handles::INVALID_RESOURCE;

            // Texture bindings (6: depth/normal/output/history/orm/specular)
            WriteDescriptorSet texWrites[6];
            DescriptorImageInfo imgInfos[6];
            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 ormBinding = isVk ? 13u : 4u;
            const u32 specBinding = isVk ? 14u : 5u;
            {
                // texture(0): depth (SampledDepthImage on Vulkan/Dawn, SampledImage on Metal)
                texWrites[0].dstSet = descriptor_set_;
                texWrites[0].dstBinding = 0;
                texWrites[0].dstArrayElement = 0;
                texWrites[0].descriptorCount = 1;
                texWrites[0].descriptorType = isVk ? DescriptorType::SampledDepthImage : DescriptorType::SampledImage;
                texWrites[0].imageInfo = &imgInfos[0];
                imgInfos[0].sampler = handles::INVALID_SAMPLER;
                imgInfos[0].imageView = inputs.gbuffer_depth;
                imgInfos[0].imageLayout = ResourceState::ShaderResource;
                // texture(1): normal
                texWrites[1].dstSet = descriptor_set_;
                texWrites[1].dstBinding = 1;
                texWrites[1].dstArrayElement = 0;
                texWrites[1].descriptorCount = 1;
                texWrites[1].descriptorType = DescriptorType::SampledImage;
                texWrites[1].imageInfo = &imgInfos[1];
                imgInfos[1].sampler = handles::INVALID_SAMPLER;
                imgInfos[1].imageView = inputs.gbuffer_normal;
                imgInfos[1].imageLayout = ResourceState::ShaderResource;
                // texture(2): output (UAV)
                texWrites[2].dstSet = descriptor_set_;
                texWrites[2].dstBinding = 2;
                texWrites[2].dstArrayElement = 0;
                texWrites[2].descriptorCount = 1;
                texWrites[2].descriptorType = DescriptorType::StorageImage;
                texWrites[2].imageInfo = &imgInfos[2];
                imgInfos[2].sampler = handles::INVALID_SAMPLER;
                imgInfos[2].imageView = gi_halfres_texture_;
                imgInfos[2].imageLayout = ResourceState::UnorderedAccess;
                // texture(3): history
                texWrites[3].dstSet = descriptor_set_;
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
                texWrites[4].dstSet = descriptor_set_;
                texWrites[4].dstBinding = ormBinding;
                texWrites[4].dstArrayElement = 0;
                texWrites[4].descriptorCount = 1;
                texWrites[4].descriptorType = DescriptorType::SampledImage;
                texWrites[4].imageInfo = &imgInfos[4];
                imgInfos[4].sampler = handles::INVALID_SAMPLER;
                imgInfos[4].imageView = ormTex;
                imgInfos[4].imageLayout = ResourceState::ShaderResource;
                // texture(5/14): glossy specular output (T4.3, UAV)
                texWrites[5].dstSet = descriptor_set_;
                texWrites[5].dstBinding = specBinding;
                texWrites[5].dstArrayElement = 0;
                texWrites[5].descriptorCount = 1;
                texWrites[5].descriptorType = DescriptorType::StorageImage;
                texWrites[5].imageInfo = &imgInfos[5];
                imgInfos[5].sampler = handles::INVALID_SAMPLER;
                imgInfos[5].imageView = gi_specular_texture_;
                imgInfos[5].imageLayout = ResourceState::UnorderedAccess;
                device_->UpdateDescriptorSets(6, texWrites);
            }

            // T4.4: far-volume bindings (Vulkan flat 15/16; Metal deferred with
            // the Metal catch-up round — engine Metal gather shader not present).
            if (isVk) {
                WriteDescriptorSet farWrites[2];
                DescriptorBufferInfo farInfos[2];
                farWrites[0].dstSet = descriptor_set_;
                farWrites[0].dstBinding = 15;
                farWrites[0].dstArrayElement = 0;
                farWrites[0].descriptorCount = 1;
                farWrites[0].descriptorType = DescriptorType::UniformBuffer;
                farWrites[0].bufferInfo = &farInfos[0];
                farInfos[0].buffer = far_cb_; farInfos[0].offset = 0; farInfos[0].range = 64;
                farWrites[1].dstSet = descriptor_set_;
                farWrites[1].dstBinding = 16;
                farWrites[1].dstArrayElement = 0;
                farWrites[1].descriptorCount = 1;
                farWrites[1].descriptorType = DescriptorType::StorageBuffer;
                farWrites[1].bufferInfo = &farInfos[1];
                // X2/A3: 远场 irradiance 句柄改走 farView（view 优先、pass 兜底）。
                ResourceHandle farIrr = (farView.IsValid() && farView.irradiance != handles::INVALID_RESOURCE)
                    ? farView.irradiance : dummy_buffer_;
                if (farIrr == handles::INVALID_RESOURCE) farIrr = dummy_buffer_;
                farInfos[1].buffer = farIrr; farInfos[1].offset = 0; farInfos[1].range = ~0ull;
                device_->UpdateDescriptorSets(2, farWrites);
            }

            // T6.3 sub-B: N-volume bindings (17 = SSBO 数组×4, 18 = 参数 CB)。
            // shader 静态使用这两个 binding → 即使 legacy 路径也必须写满
            // （数组 4 槽全写：有效 view 或 dummy；meta.x=0 保证不进入访问）。
            // 能力不可用设备的 DSL binding 17 无 PartiallyBound 标志，全写
            // dummy 同样合法。Metal 分支不带这两个 binding（shader 未声明）。
            if (isVk) {
                // binding 17: 数组元素 [0..4) —— dstArrayElement 定位槽位。
                WriteDescriptorSet nvolWrites[5];
                DescriptorBufferInfo nvolInfos[5];
                for (u32 i = 0; i < 4; ++i) {
                    nvolWrites[i].dstSet = descriptor_set_;
                    nvolWrites[i].dstBinding = 17;
                    nvolWrites[i].dstArrayElement = i;
                    nvolWrites[i].descriptorCount = 1;
                    nvolWrites[i].descriptorType = DescriptorType::StorageBuffer;
                    nvolWrites[i].imageInfo = nullptr;
                    nvolWrites[i].bufferInfo = &nvolInfos[i];
                    ResourceHandle irr = (nvolumeActive && i < nvolCount &&
                                          nvolViews[i].irradiance != handles::INVALID_RESOURCE)
                                         ? nvolViews[i].irradiance : dummy_buffer_;
                    if (irr == handles::INVALID_RESOURCE) irr = dummy_buffer_;
                    nvolInfos[i].buffer = irr; nvolInfos[i].offset = 0; nvolInfos[i].range = ~0ull;
                }
                // binding 18: 参数 CB。
                nvolWrites[4].dstSet = descriptor_set_;
                nvolWrites[4].dstBinding = 18;
                nvolWrites[4].dstArrayElement = 0;
                nvolWrites[4].descriptorCount = 1;
                nvolWrites[4].descriptorType = DescriptorType::UniformBuffer;
                nvolWrites[4].imageInfo = nullptr;
                nvolWrites[4].bufferInfo = &nvolInfos[4];
                nvolInfos[4].buffer = nvolumes_cb_; nvolInfos[4].offset = 0;
                nvolInfos[4].range = sizeof(NVolumeCB);
                device_->UpdateDescriptorSets(5, nvolWrites);
            }

            // Buffer bindings with sub-allocation from probe_cb_
            WriteDescriptorSet bufWrites[9];
            DescriptorBufferInfo bufInfos[9];
            {
                for (int i = 0; i < 9; ++i) {
                    bufWrites[i].dstSet = descriptor_set_;
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
                bufInfos[0].buffer = probe_cb_; bufInfos[0].offset = 0; bufInfos[0].range = 80;
                bufWrites[1].dstBinding = bufBase + 1;
                bufInfos[1].buffer = probe_cb_; bufInfos[1].offset = 64; bufInfos[1].range = 16;
                bufWrites[2].dstBinding = bufBase + 2;
                bufInfos[2].buffer = probe_cb_; bufInfos[2].offset = 80; bufInfos[2].range = 48;  // T6.3 sub-A: +debugVis(48B)
                bufWrites[3].dstBinding = bufBase + 3; bufWrites[3].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[3].buffer = (skySH != handles::INVALID_RESOURCE) ? skySH : dummy_buffer_;
                bufInfos[3].offset = 0; bufInfos[3].range = ~0ull;
                bufWrites[4].dstBinding = bufBase + 4; bufWrites[4].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[4].buffer = (skyFactor != handles::INVALID_RESOURCE) ? skyFactor : dummy_buffer_;
                bufInfos[4].offset = 0; bufInfos[4].range = ~0ull;
                bufWrites[5].dstBinding = bufBase + 5;
                bufInfos[5].buffer = static_probe_cb_; bufInfos[5].offset = 0; bufInfos[5].range = ~0ull;
                bufWrites[6].dstBinding = bufBase + 6; bufWrites[6].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[6].buffer = ddgi->GetConfidenceBuffer(ddgiReadIdx); bufInfos[6].offset = 0; bufInfos[6].range = ~0ull;
                bufWrites[7].dstBinding = bufBase + 7; bufWrites[7].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[7].buffer = ddgi->GetIrradianceBuffer(ddgiReadIdx); bufInfos[7].offset = 0; bufInfos[7].range = ~0ull;
                bufWrites[8].dstBinding = bufBase + 8; bufWrites[8].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[8].buffer = ddgi->GetDepthBuffer(ddgiReadIdx); bufInfos[8].offset = 0; bufInfos[8].range = ~0ull;
                device_->UpdateDescriptorSets(9, bufWrites);
            }

            // Dispatch
            cmd->BindComputePipeline(pipeline_);
            const DescriptorSetHandle sets[] = { descriptor_set_ };
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, layout_, 0, 1, sets, 0, nullptr);
            u32 halfW = render_width_ / 2;
            u32 halfH = render_height_ / 2;
            cmd->Dispatch((halfW + 7) / 8, (halfH + 7) / 8, 1);

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
        }
    );

    return outputs;
}

} // namespace primal::graphics
