#include "SDFVisualizationModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

namespace primal::graphics {

using namespace rhi;
using namespace rendergraph;

// ---------------------------------------------------------------------------
// GPU UBO layout — matches SDFVisualization.comp (std140).
// ---------------------------------------------------------------------------
struct SDFVizUniforms {
    math::v4 SdfOrigins[3];            //   0
    math::v4 SdfExtents[3];            //  48
    math::v4 SdfVoxelSizes[3];         //  96
    math::v4 SdfResolutionsAndCount;   // 144
    math::v4 CameraPosAndMaxDist;      // 160
    math::v4 CameraForward;            // 176
    math::v4 CameraRight;              // 192
    math::v4 CameraUp;                 // 208
    math::v4 LightDirAndIntensity;     // 224
    math::v4 FovAndAspect;             // 240  x=tanHalfFovY, y=aspect
};
static_assert(sizeof(SDFVizUniforms) == 256, "UBO layout mismatch");

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool SDFVisualizationModule::Initialize(RHIDeviceBase* device,
                                         ShaderHandle blit_vertex_shader,
                                         ShaderHandle sdf_viz_pixel_shader) {
    (void)sdf_viz_pixel_shader; // unused — compute shader loaded from .spv
    device_ = device;
    std::cerr << "[SDFViz] Initialize() called" << std::endl;

    // Triple-buffered UBO.
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = sizeof(SDFVizUniforms);
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = static_cast<u32>(BufferUsageFlags::Uniform);
        ubo_[i] = device->CreateBuffer(desc);
    }

    // ---- Load compute shader ----
    const char* compPath = "Engine/Graphics/Vulkan/shaders/Debug/SDFVisualization.comp.spv";
    auto tryPath = [](const std::string& p) -> std::vector<u8> {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<u8> b(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(b.data()), sz);
        return b;
    };
    std::vector<u8> compBytes = tryPath(compPath);
    for (int i = 1; i <= 6 && compBytes.empty(); ++i) {
        std::string pfx;
        for (int j = 0; j < i; ++j) pfx += "../";
        compBytes = tryPath(pfx + compPath);
    }
    ShaderHandle compShader = handles::INVALID_SHADER;
    if (!compBytes.empty())
        compShader = device->CreateShader(compBytes.data(), compBytes.size(),
                                           ShaderStage::Compute, "main");

    // ---- Compute pipeline ----
    //   0,1,2 = SampledImage (samplerless texture3D)  SDF cascade 0/1/2
    //   3     = StorageImage (image2D, RGBA8)         output
    //   4     = UniformBuffer                          SDFVizUniforms
    DescriptorSetLayoutBinding compBindings[5];
    for (int c = 0; c < 3; ++c) {
        compBindings[c] = { static_cast<u32>(c), DescriptorType::SampledImage,
                             1, ShaderStage::Compute, nullptr };
        compBindings[c].is3D = true;
        compBindings[c].unfilterableFloat = true;
    }
    compBindings[3] = { 3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr };
    compBindings[4] = { 4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr };
    compute_set_layout_ = device->CreateDescriptorSetLayout({5, compBindings});
    compute_layout_ = device->CreatePipelineLayout({1, &compute_set_layout_});
    for (int i = 0; i < 3; ++i)
        compute_sets_[i] = device->CreateDescriptorSet({compute_set_layout_});

    if (compShader != handles::INVALID_SHADER) {
        ComputePipelineDesc cpd{};
        cpd.computeShader = compShader;
        cpd.layout = compute_layout_;
        cpd.threadGroupSize = {8, 8, 1};
        compute_pipeline_ = device->CreateComputePipeline(cpd);
    }

    // ---- Offline SDF compute pipeline (dense ray march for unsigned SDF) ----
    const char* offlineCompPath = "Engine/Graphics/Vulkan/shaders/Debug/OfflineSDFViz.comp.spv";
    std::vector<u8> offlineCompBytes = tryPath(offlineCompPath);
    for (int i = 1; i <= 6 && offlineCompBytes.empty(); ++i) {
        std::string pfx;
        for (int j = 0; j < i; ++j) pfx += "../";
        offlineCompBytes = tryPath(pfx + offlineCompPath);
    }
    if (!offlineCompBytes.empty()) {
        std::cerr << "[SDFViz] OfflineSDFViz.comp.spv loaded (" << offlineCompBytes.size() << " bytes)" << std::endl;
        ShaderHandle offlineShader = device->CreateShader(
            offlineCompBytes.data(), offlineCompBytes.size(),
            ShaderStage::Compute, "main");
        if (offlineShader != handles::INVALID_SHADER) {
            ComputePipelineDesc cpd2{};
            cpd2.computeShader = offlineShader;
            cpd2.layout = compute_layout_;
            cpd2.threadGroupSize = {8, 8, 1};
            offline_compute_pipeline_ = device->CreateComputePipeline(cpd2);
            std::cerr << "[SDFViz] Offline compute pipeline: " << offline_compute_pipeline_ << std::endl;
        } else {
            std::cerr << "[SDFViz] Failed to create offline shader" << std::endl;
        }
    } else {
        std::cerr << "[SDFViz] OfflineSDFViz.comp.spv NOT FOUND" << std::endl;
    }

    // ---- Blit pipeline (SampledImage + Sampler → backbuffer) ----
    DescriptorSetLayoutBinding blitBindings[] = {
        {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {1, DescriptorType::Sampler,      1, ShaderStage::Pixel, nullptr}
    };
    blit_set_layout_ = device->CreateDescriptorSetLayout({2, blitBindings});
    blit_layout_ = device->CreatePipelineLayout({1, &blit_set_layout_});
    for (int i = 0; i < 3; ++i)
        blit_sets_[i] = device->CreateDescriptorSet({blit_set_layout_});

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    blit_sampler_ = device->CreateSampler(samplerDesc);

    // Load SDFVizBlit.frag.spv — passthrough blit (no tonemapping).
    const char* blitPath = "Engine/Graphics/Vulkan/shaders/Debug/SDFVizBlit.frag.spv";
    std::vector<u8> blitBytes = tryPath(blitPath);
    for (int i = 1; i <= 6 && blitBytes.empty(); ++i) {
        std::string pfx;
        for (int j = 0; j < i; ++j) pfx += "../";
        blitBytes = tryPath(pfx + blitPath);
    }
    ShaderHandle blitPs = handles::INVALID_SHADER;
    if (!blitBytes.empty())
        blitPs = device->CreateShader(blitBytes.data(), blitBytes.size(),
                                       ShaderStage::Pixel, "main");

    if (blit_vertex_shader != handles::INVALID_SHADER &&
        blitPs != handles::INVALID_SHADER) {
        GraphicsPipelineDesc pd{};
        pd.layout = blit_layout_;
        pd.vertexShader = blit_vertex_shader;
        pd.pixelShader = blitPs;
        pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        pd.renderTargetCount = 1;
        pd.depthStencilFormat = DataFormat::Unknown;
        pd.enableDepthTest = false;
        pd.enableDepthWrite = false;
        pd.cullMode = CullMode::None;
        pd.vertexAttributes.clear();
        pd.vertexBindings.clear();
        blit_pipeline_ = device->CreateGraphicsPipeline(pd);
    }

    return compute_pipeline_ != handles::INVALID_PIPELINE;
}

void SDFVisualizationModule::Shutdown() {
    if (device_ == nullptr) return;
    if (blit_sampler_ != handles::INVALID_SAMPLER) {
        device_->DestroySampler(blit_sampler_);
        blit_sampler_ = handles::INVALID_SAMPLER;
    }
    for (int i = 0; i < 3; ++i) {
        if (ubo_[i] != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(ubo_[i]);
            ubo_[i] = handles::INVALID_RESOURCE;
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: fill the SDFVizUniforms UBO.
// ---------------------------------------------------------------------------
static void FillUBO(RHIDeviceBase* device, ResourceHandle ubo,
                    const SDFVisualizationInputs& inputs,
                    nanite::GlobalSDF& gsdf, u32 cascadeCount) {
    math::v3 camPos = {inputs.camera_position.x, inputs.camera_position.y,
                       inputs.camera_position.z};

    // Extract camera basis vectors from the view matrix.
    // CreateLookAtMatrix stores column-major: column j = (row0[j], row1[j], row2[j], row3[j]).
    // Row 0 = right, row 1 = up, row 2 = -forward.
    const math::m4x4& vm = inputs.view_matrix;
    math::v3 right   = {vm.columns[0][0], vm.columns[1][0], vm.columns[2][0]};
    math::v3 up      = {vm.columns[0][1], vm.columns[1][1], vm.columns[2][1]};
    math::v3 forward = {-vm.columns[0][2], -vm.columns[1][2], -vm.columns[2][2]};
    forward = rhi::math::Normalize(forward);
    right   = rhi::math::Normalize(right);
    up      = rhi::math::Normalize(up);

    // Extract tan(halfFovY) and aspect from the projection matrix.
    const math::m4x4& pm = inputs.proj_matrix;
    float tanHalfFovY = (pm.columns[1][1] != 0.0f) ? 1.0f / pm.columns[1][1] : 1.0f;
    float aspect = 1.0f;
    if (inputs.render_height > 0) {
        aspect = static_cast<float>(inputs.render_width) / inputs.render_height;
    }

    float maxDist = 500.0f;
    if (cascadeCount > 0) {
        const auto& lc = gsdf.GetCascade(cascadeCount - 1);
        math::v3 ev{lc.extent.x, lc.extent.y, lc.extent.z};
        maxDist = rhi::math::Length(ev) * 2.0f;
        if (maxDist < 10.0f) maxDist = 500.0f;
    }

    SDFVizUniforms* mapped = static_cast<SDFVizUniforms*>(device->MapBuffer(ubo));
    if (!mapped) return;
    SDFVizUniforms u{};
    for (u32 c = 0; c < cascadeCount; ++c) {
        const auto& cas = gsdf.GetCascade(c);
        u.SdfOrigins[c] = {cas.origin.x, cas.origin.y, cas.origin.z, 0.0f};
        u.SdfExtents[c] = {cas.extent.x, cas.extent.y, cas.extent.z, 0.0f};
        u.SdfVoxelSizes[c] = {cas.voxel_size, 0.0f, 0.0f, 0.0f};
        u.SdfResolutionsAndCount[c] = static_cast<float>(cas.resolution);
    }
    u.SdfResolutionsAndCount.w = static_cast<float>(cascadeCount);
    u.CameraPosAndMaxDist = {camPos.x, camPos.y, camPos.z, maxDist};
    u.CameraForward = {forward.x, forward.y, forward.z, 0.0f};
    u.CameraRight   = {right.x, right.y, right.z, 0.0f};
    u.CameraUp      = {up.x, up.y, up.z, 0.0f};
    u.LightDirAndIntensity = {
        inputs.light_direction.x, inputs.light_direction.y,
        inputs.light_direction.z, inputs.light_intensity};
    u.FovAndAspect = {tanHalfFovY, aspect, 0.0f, 0.0f};
    *mapped = u;
    device->UnmapBuffer(ubo);
}

// ---------------------------------------------------------------------------
// Helper: fill UBO for offline SDF mode (single volume, cascade count = 1).
// ---------------------------------------------------------------------------
static void FillOfflineUBO(RHIDeviceBase* device, ResourceHandle ubo,
                           const SDFVisualizationInputs& inputs) {
    math::v3 camPos = {inputs.camera_position.x, inputs.camera_position.y,
                       inputs.camera_position.z};

    const math::m4x4& vm = inputs.view_matrix;
    math::v3 right   = {vm.columns[0][0], vm.columns[1][0], vm.columns[2][0]};
    math::v3 up      = {vm.columns[0][1], vm.columns[1][1], vm.columns[2][1]};
    math::v3 forward = {-vm.columns[0][2], -vm.columns[1][2], -vm.columns[2][2]};
    forward = rhi::math::Normalize(forward);
    right   = rhi::math::Normalize(right);
    up      = rhi::math::Normalize(up);

    const math::m4x4& pm = inputs.proj_matrix;
    float tanHalfFovY = (pm.columns[1][1] != 0.0f) ? 1.0f / pm.columns[1][1] : 1.0f;
    float aspect = (inputs.render_height > 0)
                   ? static_cast<float>(inputs.render_width) / inputs.render_height
                   : 1.0f;

    float maxDist = rhi::math::Length(inputs.offline_sdf_extent) * 2.0f;
    if (maxDist < 10.0f) maxDist = 500.0f;

    float voxelSize = 0.0f;
    if (inputs.offline_sdf_resolution > 0) {
        float maxExtent = std::max({inputs.offline_sdf_extent.x,
                                    inputs.offline_sdf_extent.y,
                                    inputs.offline_sdf_extent.z});
        voxelSize = maxExtent / static_cast<float>(inputs.offline_sdf_resolution);
    }

    SDFVizUniforms* mapped = static_cast<SDFVizUniforms*>(device->MapBuffer(ubo));
    if (!mapped) return;
    SDFVizUniforms u{};

    // Fill cascade 0 with offline SDF metadata.
    u.SdfOrigins[0] = {inputs.offline_sdf_origin.x, inputs.offline_sdf_origin.y,
                       inputs.offline_sdf_origin.z, 0.0f};
    u.SdfExtents[0] = {inputs.offline_sdf_extent.x, inputs.offline_sdf_extent.y,
                       inputs.offline_sdf_extent.z, 0.0f};
    u.SdfVoxelSizes[0] = {voxelSize, 0.0f, 0.0f, 0.0f};
    u.SdfResolutionsAndCount[0] = static_cast<float>(inputs.offline_sdf_resolution);

    // Cascade count = 1 (only one volume).
    u.SdfResolutionsAndCount.w = 1.0f;

    u.CameraPosAndMaxDist = {camPos.x, camPos.y, camPos.z, maxDist};
    u.CameraForward = {forward.x, forward.y, forward.z, 0.0f};
    u.CameraRight   = {right.x, right.y, right.z, 0.0f};
    u.CameraUp      = {up.x, up.y, up.z, 0.0f};
    u.LightDirAndIntensity = {
        inputs.light_direction.x, inputs.light_direction.y,
        inputs.light_direction.z, inputs.light_intensity};
    u.FovAndAspect = {tanHalfFovY, aspect, 0.0f, 0.0f};
    *mapped = u;
    device->UnmapBuffer(ubo);
}

// ---------------------------------------------------------------------------
// AddPass
// ---------------------------------------------------------------------------
void SDFVisualizationModule::AddPass(RenderGraph& graph,
                                     const SDFVisualizationInputs& inputs) {
    if (compute_pipeline_ == handles::INVALID_PIPELINE) return;

    const u32 cbIdx = inputs.current_buffer_index % 3;

    // ================================================================
    // Pass 1: Compute ray-march → RGBA8 StorageImage
    // ================================================================
    struct ComputeData {
        RGResourceHandle outputTex;
    };

    const ComputeData& cout = graph.AddPass<ComputeData>(
        "SDFViz_Compute",
        RGPassType::Compute,
        RGPassCategory::PostProcess,
        [inputs](ComputeData& data, RenderGraphBuilder& builder) {
            builder.SideEffect();
            TextureDesc td{};
            td.size = {inputs.render_width, inputs.render_height, 1};
            td.format = DataFormat::BGRA8_UNorm;
            td.usage = TextureUsage::UnorderedAccess | TextureUsage::ShaderResource;
            td.mipLevels = 1;
            data.outputTex = builder.CreateTexture("SDFViz_Output", td,
                                                     ResourceState::UnorderedAccess);
        },
        [this, inputs, cbIdx](const ComputeData& data, RenderGraphContext& ctx) {
            auto cmd = ctx.cmdBuffer;

            ResourceHandle sdfTex[3] = {handles::INVALID_RESOURCE,
                                        handles::INVALID_RESOURCE,
                                        handles::INVALID_RESOURCE};
            u32 cascadeCount = 0;

            if (inputs.use_offline_sdf && inputs.offline_sdf_texture != handles::INVALID_RESOURCE) {
                // Offline SDF mode: bind the single global texture to all 3 cascade slots.
                sdfTex[0] = inputs.offline_sdf_texture;
                sdfTex[1] = inputs.offline_sdf_texture;
                sdfTex[2] = inputs.offline_sdf_texture;
                cascadeCount = 1;
            } else {
                auto& gsdf = nanite::GlobalSDF::Get();
                if (gsdf.IsInitialized()) {
                    cascadeCount = std::min(3u, gsdf.GetConfig().cascade_count);
                    for (u32 c = 0; c < cascadeCount; ++c)
                        sdfTex[c] = gsdf.GetCascade(c).sdf_texture;
                }
            }
            if (sdfTex[0] == handles::INVALID_RESOURCE) return;

            // Resolve the output StorageImage handle.
            auto* outRes = ctx.graph->GetResource(data.outputTex);
            ResourceHandle outTex = outRes ? outRes->GetPhysicalHandle()
                                           : handles::INVALID_RESOURCE;
            if (outTex == handles::INVALID_RESOURCE) return;

            // SRV barrier on SDF texture(s).
            for (u32 c = 0; c < cascadeCount; ++c) {
                ResourceBarrier b{};
                b.resource = sdfTex[c];
                b.beforeState = ResourceState::ShaderResource;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                b.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            // Fill UBO.
            if (inputs.use_offline_sdf) {
                FillOfflineUBO(device_, ubo_[cbIdx], inputs);
            } else {
                auto& gsdf = nanite::GlobalSDF::Get();
                FillUBO(device_, ubo_[cbIdx], inputs, gsdf, cascadeCount);
            }

            // Update descriptor set.
            DescriptorImageInfo sdfInfo[3];
            for (u32 c = 0; c < 3; ++c) {
                sdfInfo[c].imageView = sdfTex[c] != handles::INVALID_RESOURCE
                                       ? sdfTex[c] : sdfTex[0];
                sdfInfo[c].imageLayout = ResourceState::ShaderResource;
            }
            DescriptorImageInfo outInfo;
            outInfo.imageView = outTex;
            outInfo.imageLayout = ResourceState::UnorderedAccess;
            DescriptorBufferInfo uboInfo;
            uboInfo.buffer = ubo_[cbIdx];
            uboInfo.offset = 0;
            uboInfo.range = sizeof(SDFVizUniforms);

            WriteDescriptorSet writes[5];
            for (u32 c = 0; c < 3; ++c) {
                writes[c].dstSet = compute_sets_[cbIdx];
                writes[c].dstBinding = c;
                writes[c].descriptorCount = 1;
                writes[c].descriptorType = DescriptorType::SampledImage;
                writes[c].imageInfo = &sdfInfo[c];
            }
            writes[3].dstSet = compute_sets_[cbIdx];
            writes[3].dstBinding = 3;
            writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::StorageImage;
            writes[3].imageInfo = &outInfo;
            writes[4].dstSet = compute_sets_[cbIdx];
            writes[4].dstBinding = 4;
            writes[4].descriptorCount = 1;
            writes[4].descriptorType = DescriptorType::UniformBuffer;
            writes[4].bufferInfo = &uboInfo;
            device_->UpdateDescriptorSets(5, writes);

            // Dispatch — choose pipeline based on offline vs cascade mode.
            PipelineHandle activePipeline = (inputs.use_offline_sdf && HasOfflinePipeline())
                                           ? offline_compute_pipeline_ : compute_pipeline_;
            // Log which pipeline and data source is used (first frame only).
            static bool loggedViz = false;
            if (!loggedViz) {
                loggedViz = true;
                std::cerr << "[SDFViz] Dispatch: use_offline=" << inputs.use_offline_sdf
                          << " hasOffline=" << HasOfflinePipeline()
                          << " pipeline=" << activePipeline
                          << " sdfTex=" << sdfTex[0]
                          << " resolution=" << inputs.offline_sdf_resolution
                          << std::endl;
            }
            cmd->BindComputePipeline(activePipeline);
            const DescriptorSetHandle sets[] = {compute_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Compute, compute_layout_,
                                    0, 1, sets, 0, nullptr);
            cmd->Dispatch((inputs.render_width + 7) / 8,
                          (inputs.render_height + 7) / 8, 1);
        }
    );

    // ================================================================
    // Pass 2: Blit to backbuffer
    // ================================================================
    struct BlitData {
        RGResourceHandle output;
    };

    graph.AddPass<BlitData>(
        "SDFViz_Blit",
        RGPassType::Graphics,
        RGPassCategory::PostProcess,
        [inputs, loTex = cout.outputTex](BlitData& data, RenderGraphBuilder& builder) {
            if (loTex.IsValid())
                builder.Read(loTex, ResourceState::ShaderResource);
            data.output = builder.Write(inputs.backbuffer_rg,
                                        ResourceState::RenderTarget);
            RGRenderPassDesc rp;
            rp.colors.push_back({.texture = data.output,
                                 .loadOp = LoadAction::DontCare,
                                 .storeOp = StoreAction::Store,
                                 .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rp);
        },
        [this, inputs, loTex = cout.outputTex, cbIdx](const BlitData&,
                                                       RenderGraphContext& ctx) {
            if (blit_pipeline_ == handles::INVALID_PIPELINE) return;
            auto cmd = ctx.cmdBuffer;

            ResourceHandle loPhys = handles::INVALID_RESOURCE;
            if (loTex.IsValid()) {
                auto* res = ctx.graph->GetResource(loTex);
                if (res) loPhys = res->GetPhysicalHandle();
            }
            if (loPhys == handles::INVALID_RESOURCE) return;

            // Manual UAV→SRV barrier: the compute pass wrote to this texture
            // as a StorageImage; we need it in ShaderResource state to sample.
            // RG may not emit this automatically because the compute pass used
            // SideEffect() rather than declaring a Write on the texture.
            {
                ResourceBarrier b{};
                b.resource = loPhys;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState = ResourceState::ShaderResource;
                b.subresource = 0xFFFFFFFF;
                b.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            DescriptorImageInfo texInfo;
            texInfo.imageView = loPhys;
            texInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo sampInfo;
            sampInfo.sampler = blit_sampler_;

            WriteDescriptorSet writes[2];
            writes[0].dstSet = blit_sets_[cbIdx];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage;
            writes[0].imageInfo = &texInfo;
            writes[1].dstSet = blit_sets_[cbIdx];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::Sampler;
            writes[1].imageInfo = &sampInfo;
            device_->UpdateDescriptorSets(2, writes);

            cmd->SetViewport({{0, 0},
                              {(float)inputs.render_width, (float)inputs.render_height},
                              0, 1});
            cmd->SetScissor({{0, 0}, {inputs.render_width, inputs.render_height}});
            cmd->BindGraphicsPipeline(blit_pipeline_);
            const DescriptorSetHandle sets[] = {blit_sets_[cbIdx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blit_layout_,
                                    0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );
}

} // namespace primal::graphics
