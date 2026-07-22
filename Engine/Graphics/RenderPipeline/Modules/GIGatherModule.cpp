#include "GIGatherModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

struct GIGatherCB {
    math::m4x4 inv_view_projection;
    math::v4   probe_origin_spacing;
    math::v4   probe_counts;
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

    // 13 bindings: 4 textures + 9 buffers
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
        {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
        {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output
        {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // history
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
    set_layout_ = device->CreateDescriptorSetLayout({13, bindings});
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
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    gi_halfres_texture_ = device->CreateTexture(texDesc);
    gi_halfres_history_ = device->CreateTexture(texDesc);

    // Constant buffers
    BufferDesc cbDesc{};
    cbDesc.size = 256;
    cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    probe_cb_ = device->CreateBuffer(cbDesc);

    BufferDesc spCbDesc{};
    spCbDesc.size = 512;
    spCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    static_probe_cb_ = device->CreateBuffer(spCbDesc);

    // Dummy buffer for missing static probe bindings
    BufferDesc dummyDesc{};
    dummyDesc.size = 64;
    dummyDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    dummy_buffer_ = device->CreateBuffer(dummyDesc);

    return true;
}

void GIGatherModule::Shutdown() {}

GIGatherOutputs GIGatherModule::AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs) {
    GIGatherOutputs outputs{};
    if (!inputs.ddgi_pass || pipeline_ == handles::INVALID_PIPELINE) return outputs;

    outputs.gi_output_tex = gi_halfres_texture_;
    outputs.gi_output_rg = graph.ImportResource("DDGIHalfResGI", gi_halfres_texture_);

    u32 ddgiReadIdx = (inputs.current_buffer_index + 2) % 3;

    // Import DDGI buffers
    auto irrHandle = graph.ImportResource("DDGIIrrRead", inputs.ddgi_pass->GetIrradianceBuffer(ddgiReadIdx));
    auto depthHandle = graph.ImportResource("DDGIDepthRead", inputs.ddgi_pass->GetDepthBuffer(ddgiReadIdx));

    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<PassData>("DDGIGIGather",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [outRG = outputs.gi_output_rg, depthRG = inputs.gbuffer_depth_rg, normalRG = inputs.gbuffer_normal_rg]
        (PassData& data, rendergraph::RenderGraphBuilder& builder) {
            if (depthRG.IsValid()) builder.Read(depthRG, ResourceState::ShaderResource);
            if (normalRG.IsValid()) builder.Read(normalRG, ResourceState::ShaderResource);
            data.output = builder.Write(outRG, ResourceState::UnorderedAccess);
        },
        [this, inputs, ddgiReadIdx](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            auto ddgi = inputs.ddgi_pass;

            // Upload GIGatherCB
            {
                math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
                auto* cb = static_cast<GIGatherCB*>(device_->MapBuffer(probe_cb_));
                if (cb) {
                    cb->inv_view_projection = Inverse(vp);
                    const auto& vol = ddgi->GetVolumeData();
                    const auto& params = ddgi->GetParams();
                    cb->probe_origin_spacing = math::v4{vol.ProbeOrigin.x, vol.ProbeOrigin.y, vol.ProbeOrigin.z, params.probe_spacing};
                    cb->probe_counts = math::v4{(float)params.probe_count_x, (float)params.probe_count_y, (float)params.probe_count_z, 9.0f};
                    device_->UnmapBuffer(probe_cb_);
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

            // Barrier DDGI buffers from UAV to ShaderResource
            {
                ResourceBarrier barriers[3]{};
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
                if (bc > 0) cmd->InsertBarrier(barriers, bc);
            }

            // Update descriptor set
            ResourceHandle skySH = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkySH() : handles::INVALID_RESOURCE;
            ResourceHandle skyFactor = inputs.static_probe_volume ? inputs.static_probe_volume->GetStaticSkyFactor() : handles::INVALID_RESOURCE;

            // Texture bindings
            WriteDescriptorSet texWrites[4];
            DescriptorImageInfo imgInfos[4];
            {
                // texture(0): depth
                texWrites[0].dstSet = descriptor_set_;
                texWrites[0].dstBinding = 0;
                texWrites[0].dstArrayElement = 0;
                texWrites[0].descriptorCount = 1;
                texWrites[0].descriptorType = DescriptorType::SampledImage;
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
                imgInfos[3].imageView = gi_halfres_history_;
                imgInfos[3].imageLayout = ResourceState::ShaderResource;
                device_->UpdateDescriptorSets(4, texWrites);
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
                bufWrites[0].dstBinding = 0;
                bufInfos[0].buffer = probe_cb_; bufInfos[0].offset = 0; bufInfos[0].range = 64;
                bufWrites[1].dstBinding = 1;
                bufInfos[1].buffer = probe_cb_; bufInfos[1].offset = 64; bufInfos[1].range = 16;
                bufWrites[2].dstBinding = 2;
                bufInfos[2].buffer = probe_cb_; bufInfos[2].offset = 80; bufInfos[2].range = 16;
                bufWrites[3].dstBinding = 3; bufWrites[3].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[3].buffer = (skySH != handles::INVALID_RESOURCE) ? skySH : dummy_buffer_;
                bufInfos[3].offset = 0; bufInfos[3].range = ~0ull;
                bufWrites[4].dstBinding = 4; bufWrites[4].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[4].buffer = (skyFactor != handles::INVALID_RESOURCE) ? skyFactor : dummy_buffer_;
                bufInfos[4].offset = 0; bufInfos[4].range = ~0ull;
                bufWrites[5].dstBinding = 5;
                bufInfos[5].buffer = static_probe_cb_; bufInfos[5].offset = 0; bufInfos[5].range = ~0ull;
                bufWrites[6].dstBinding = 6; bufWrites[6].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[6].buffer = ddgi->GetConfidenceBuffer(ddgiReadIdx); bufInfos[6].offset = 0; bufInfos[6].range = ~0ull;
                bufWrites[7].dstBinding = 7; bufWrites[7].descriptorType = DescriptorType::StorageBuffer;
                bufInfos[7].buffer = ddgi->GetIrradianceBuffer(ddgiReadIdx); bufInfos[7].offset = 0; bufInfos[7].range = ~0ull;
                bufWrites[8].dstBinding = 8; bufWrites[8].descriptorType = DescriptorType::StorageBuffer;
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

            // Barrier output + blit to history
            {
                ResourceBarrier outBarrier{};
                outBarrier.resource = gi_halfres_texture_;
                outBarrier.beforeState = ResourceState::UnorderedAccess;
                outBarrier.afterState = ResourceState::ShaderResource;
                outBarrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&outBarrier, 1);

                TextureBlitRegion blitRegion{};
                blitRegion.srcOffsets[0] = {0, 0, 0};
                blitRegion.srcOffsets[1] = {(int)halfW, (int)halfH, 1};
                blitRegion.dstOffsets[0] = {0, 0, 0};
                blitRegion.dstOffsets[1] = {(int)halfW, (int)halfH, 1};
                cmd->BlitTexture(gi_halfres_texture_, gi_halfres_history_, &blitRegion, 1, FilterMode::Nearest);
            }
        }
    );

    return outputs;
}

} // namespace primal::graphics
