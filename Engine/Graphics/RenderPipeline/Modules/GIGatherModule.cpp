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
    math::v4   camera_pos;  // T4.3: world-space camera for reflection vectors
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
    if (isVk) {
        DescriptorSetLayoutBinding vkBindings[] = {
            {0,  DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // depth
            {1,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // normal
            {2,  DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // output
            {3,  DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // history
            {4,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // invViewProj+cameraPos
            {5,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeOriginSpacing
            {6,  DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // probeCounts
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
        };
        set_layout_ = device->CreateDescriptorSetLayout({17, vkBindings});
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
                ResourceBarrier barriers[4]{};
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
                bufInfos[2].buffer = probe_cb_; bufInfos[2].offset = 80; bufInfos[2].range = 16;
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
