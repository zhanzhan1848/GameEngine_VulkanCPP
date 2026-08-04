#include "ShadowMapModule.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

// --- Descriptor update helper (same as TestPipeline's) ---
struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer || params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage || params[i].type == DescriptorType::StorageImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        } else if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// --- ShadowMapModule ---

bool ShadowMapModule::Initialize(RHIDeviceBase* device, nanite::GPUDrivenDrawPipeline* gpu_draw_pipeline,
                                 u32 render_width, u32 render_height,
                                 u32 num_instances, u32 max_clusters) {
    device_ = device;
    gpu_draw_pipeline_ = gpu_draw_pipeline;
    render_width_ = render_width;
    render_height_ = render_height;

    if (gpu_draw_pipeline_) {
        gpu_draw_pipeline_->InitializeShadowResources(num_instances, max_clusters);
    }
    return true;
}

void ShadowMapModule::Shutdown() {
    if (gpu_draw_pipeline_) {
        gpu_draw_pipeline_->ShutdownShadowResources();
    }
    shadow_cache_globally_valid_ = false;
    for (int b = 0; b < 3; ++b)
        for (int c = 0; c < 2; ++c)
            shadow_cache_valid_[b][c] = false;
}

bool ShadowMapModule::InitializeShadowFilter(ShaderHandle shadow_filter_shader) {
    if (!device_ || shadow_filter_shader == handles::INVALID_SHADER) return false;

    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
        {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
        {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
        {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
        {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
    };
    shadow_filter_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    shadow_filter_layout_ = device_->CreatePipelineLayout({1, &shadow_filter_set_layout_});

    for (int i = 0; i < 3; ++i)
        shadow_filter_ds_[i] = device_->CreateDescriptorSet({shadow_filter_set_layout_});

    ComputePipelineDesc pd{};
    pd.computeShader = shadow_filter_shader;
    pd.layout = shadow_filter_layout_;
    pd.threadGroupSize = {8, 8, 1};
    shadow_filter_pipeline_ = device_->CreateComputePipeline(pd);
    if (shadow_filter_pipeline_ == handles::INVALID_PIPELINE) return false;

    TextureDesc visDesc{};
    visDesc.size = {(render_width_ / 2), (render_height_ / 2), 1};
    visDesc.format = DataFormat::R8_UNorm;
    visDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::RenderTarget;
    shadow_visibility_tex_ = device_->CreateTexture(visDesc);
    if (shadow_visibility_tex_ == handles::INVALID_RESOURCE) return false;

    for (int i = 0; i < 3; ++i) {
        BufferDesc cbDesc{};
        cbDesc.size = 512;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        shadow_filter_cb_[i] = device_->CreateBuffer(cbDesc);
    }
    return true;
}

math::m4x4 ShadowMapModule::ComputeCascadeVP(math::v3 lightDir, math::v3 cameraPos,
                                              float orthoExtent, u32 shadowMapSize,
                                              const ShadowMapInputs& inputs, u32 cascade) {
    math::v3 lightUp = (std::abs(lightDir.y) < 0.99f)
        ? math::v3{0.0f, 1.0f, 0.0f} : math::v3{1.0f, 0.0f, 0.0f};

    // Use scene centroid instead of camera position for stable shadows
    math::v3 sceneCenter = {0.0f, 0.0f, 0.0f};
    u32 instanceCount = 0;
    if (inputs.scene_snapshot) {
        const auto& instances = inputs.scene_snapshot->GetInstanceData();
        for (const auto& inst : instances) {
            sceneCenter.x += inst.bounds_center.x;
            sceneCenter.y += inst.bounds_center.y;
            sceneCenter.z += inst.bounds_center.z;
            instanceCount++;
        }
        if (instanceCount > 0) {
            sceneCenter.x /= instanceCount;
            sceneCenter.y /= instanceCount;
            sceneCenter.z /= instanceCount;
        }
    }

    float cascadeDistance = 200.0f;
    math::v3 origin = {0.0f, 0.0f, 0.0f};
    math::v3 lightLookAt = {origin.x + lightDir.x, origin.y + lightDir.y, origin.z + lightDir.z};
    math::m4x4 tempView = CreateLookAtMatrix(origin, lightLookAt, lightUp);

    math::v4 centerLS4 = tempView * math::v4{sceneCenter.x, sceneCenter.y, sceneCenter.z, 1.0f};
    math::v3 centerLS = {centerLS4.x, centerLS4.y, centerLS4.z};

    float worldUnitsPerTexel = (orthoExtent * 2.0f) / (float)shadowMapSize;
    centerLS.x = std::floor(centerLS.x / worldUnitsPerTexel) * worldUnitsPerTexel;
    centerLS.y = std::floor(centerLS.y / worldUnitsPerTexel) * worldUnitsPerTexel;

    math::m4x4 tempViewInv = Inverse(tempView);
    math::v4 snapped4 = tempViewInv * math::v4{centerLS.x, centerLS.y, centerLS.z, 1.0f};
    math::v3 snappedWorld = {snapped4.x, snapped4.y, snapped4.z};

    math::v3 lightEye = {
        snappedWorld.x - lightDir.x * cascadeDistance,
        snappedWorld.y - lightDir.y * cascadeDistance,
        snappedWorld.z - lightDir.z * cascadeDistance
    };
    math::m4x4 lightView = CreateLookAtMatrix(lightEye, snappedWorld, lightUp);

    float minDist = 1e10f;
    float maxDist = 0.0f;
    if (inputs.scene_snapshot) {
        const auto& instances = inputs.scene_snapshot->GetInstanceData();
        for (const auto& inst : instances) {
            float dx = inst.bounds_center.x - lightEye.x;
            float dy = inst.bounds_center.y - lightEye.y;
            float dz = inst.bounds_center.z - lightEye.z;
            float dist = dx * lightDir.x + dy * lightDir.y + dz * lightDir.z;
            minDist = std::min(minDist, dist - inst.bounds_radius);
            maxDist = std::max(maxDist, dist + inst.bounds_radius);
        }
    }
    float depthSpan = maxDist - minDist;
    float margin = depthSpan * 0.1f;
    float nearPlane = std::max(minDist - margin, 0.5f);
    float farPlane = maxDist + margin;

    math::m4x4 lightProj = CreateOrthographicMatrix(
        -orthoExtent, orthoExtent, -orthoExtent, orthoExtent, nearPlane, farPlane);
    return lightProj * lightView;
}

ShadowMapOutputs ShadowMapModule::AddPasses(rendergraph::RenderGraph& graph, const ShadowMapInputs& inputs) {
    ShadowMapOutputs outputs{};
    if (!gpu_draw_pipeline_ || !inputs.scene_snapshot) return outputs;

    // T4.6.5 part 24.9 (B7 fix): skip shadow passes entirely on empty scene.
    // ExecuteShadowRaster + ExecuteShadowDepthBlit already self-guard via the
    // INVALID-buffer check, but the ShadowFilter dispatch at the tail still
    // references sm0/sm1 (R32_Float) which are never written in empty scene,
    // staying UNDEFINED → VUID-vkCmdDraw-None-09600 (color aspect UNDEFINED).
    if (inputs.scene_snapshot->GetInstanceCount() == 0 ||
        inputs.scene_snapshot->GetClusterRefCount() == 0) {
        return outputs;
    }

    u32 cbIdx = inputs.current_buffer_index % 3;
    math::v3 lightDir = Normalize(inputs.light_direction);

    rendergraph::RGResourceHandle shadowMapRG[2];
    for (u32 c = 0; c < 2; ++c) {
        auto smHandle = gpu_draw_pipeline_->GetShadowMap(c, inputs.current_buffer_index);
        if (smHandle != handles::INVALID_RESOURCE) {
            shadowMapRG[c] = graph.ImportResource(
                "ShadowMap_C" + std::to_string(c) + "_" + std::to_string(inputs.current_buffer_index),
                smHandle);
        }
    }

    auto vp_equal = [](const math::m4x4& a, const math::m4x4& b) -> bool {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a.columns[c][r] - b.columns[c][r]) > 1e-5f) return false;
        return true;
    };

    struct ShadowPassData {};

    for (u32 cascade = 0; cascade < 2; ++cascade) {
        float orthoExtent = (cascade == 0) ? 30.0f : 150.0f;
        math::m4x4 lightVP = ComputeCascadeVP(lightDir, inputs.camera_position, orthoExtent, 2048, inputs, cascade);

        bool cache_hit = shadow_cache_globally_valid_ &&
                         shadow_cache_valid_[cbIdx][cascade] &&
                         vp_equal(lightVP, cached_shadow_vp_[cbIdx][cascade]);

        if (cascade == 0) outputs.shadow_matrix0 = lightVP;
        else outputs.shadow_matrix1 = lightVP;

        if (cache_hit) continue;

        nanite::GPUDrivenDrawPipeline::DirectionalLightData lightData{};
        lightData.direction = {lightDir.x, lightDir.y, lightDir.z, 0.0f};
        lightData.color = {5.0f, 5.0f, 5.0f, 1.0f};
        lightData.viewPos = {inputs.camera_position.x, inputs.camera_position.y, inputs.camera_position.z, 1.0f};
        if (cascade == 0) lightData.shadowMatrix0 = lightVP;
        else lightData.shadowMatrix1 = lightVP;
        lightData.cascadeSplits = {600.0f, 2000.0f, 0.0f, 0.0f};

        // Shadow Culling
        graph.AddPass<ShadowPassData>("ShadowCulling_C" + std::to_string(cascade),
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) { builder.SideEffect(); },
            [this, lightData, cascade, bufIdx = inputs.current_buffer_index, snap = inputs.scene_snapshot]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                gpu_draw_pipeline_->ExecuteShadowCulling(context.cmdBuffer, *snap, lightData, cascade, bufIdx);
            }
        );

        // Shadow Raster
        graph.AddPass<ShadowPassData>("ShadowRaster_C" + std::to_string(cascade),
            rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::Lighting,
            [](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) { builder.SideEffect(); },
            [this, lightVP, cascade, bufIdx = inputs.current_buffer_index]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                gpu_draw_pipeline_->ExecuteShadowRaster(context.cmdBuffer, lightVP, cascade, bufIdx);
            }
        );

        // Shadow Depth Blit
        graph.AddPass<ShadowPassData>("ShadowBlit_C" + std::to_string(cascade),
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [shadowMapRG, cascade](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) {
                builder.SideEffect();
                if (shadowMapRG[cascade].IsValid())
                    builder.Write(shadowMapRG[cascade], ResourceState::UnorderedAccess);
            },
            [this, cascade, bufIdx = inputs.current_buffer_index]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                gpu_draw_pipeline_->ExecuteShadowDepthBlit(context.cmdBuffer, cascade, bufIdx);
            }
        );

        cached_shadow_vp_[cbIdx][cascade] = lightVP;
        shadow_cache_valid_[cbIdx][cascade] = true;
    }

    shadow_cache_globally_valid_ = true;

    // Shadow Filter (half-res compute)
    outputs.shadow_visibility_tex = shadow_visibility_tex_;
    if (shadow_filter_pipeline_ != handles::INVALID_PIPELINE && shadow_visibility_tex_ != handles::INVALID_RESOURCE) {
        outputs.shadow_visibility_rg = graph.ImportResource("ShadowVisibility", shadow_visibility_tex_);

        struct ShadowFilterData { rendergraph::RGResourceHandle output; };

        // Capture outputs by value for shadow matrices
        auto mat0 = outputs.shadow_matrix0;
        auto mat1 = outputs.shadow_matrix1;

        graph.AddPass<ShadowFilterData>("ShadowFilter",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Copy,
            [visRG = outputs.shadow_visibility_rg, shadowMapRG](ShadowFilterData& data, rendergraph::RenderGraphBuilder& builder) {
                data.output = builder.Write(visRG, ResourceState::UnorderedAccess);
                for (u32 c = 0; c < 2; ++c)
                    if (shadowMapRG[c].IsValid())
                        builder.Read(shadowMapRG[c], ResourceState::ShaderResource);
            },
            [this, inputs, cbIdx, mat0, mat1](const ShadowFilterData&, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                math::m4x4 invVP = Inverse(inputs.proj_matrix * inputs.view_matrix);
                math::v3 L = Normalize(inputs.light_direction);

                struct Params {
                    math::m4x4 inv_view_proj;
                    math::m4x4 shadow_vp[2];
                    math::v4   light_dir;
                    math::v2   texel_size;
                    math::v2   depth_texel_size;
                    u32 shadow_quality;
                    u32 render_width;
                    u32 render_height;
                    float _pad0;
                };

                auto* p = static_cast<Params*>(device_->MapBuffer(shadow_filter_cb_[cbIdx]));
                if (p) {
                    p->inv_view_proj = invVP;
                    p->shadow_vp[0] = mat0;
                    p->shadow_vp[1] = mat1;
                    p->light_dir = math::v4{L.x, L.y, L.z, 0.0f};
                    p->texel_size = math::v2{1.0f / 2048.0f, 1.0f / 2048.0f};
                    p->depth_texel_size = math::v2{1.0f / (float)render_width_, 1.0f / (float)render_height_};
                    p->shadow_quality = static_cast<u32>(inputs.shadow_quality);
                    p->render_width = render_width_;
                    p->render_height = render_height_;
                    p->_pad0 = 0.0f;
                    device_->UnmapBuffer(shadow_filter_cb_[cbIdx]);
                }

                ResourceHandle sm0 = gpu_draw_pipeline_->GetShadowMap(0, inputs.current_buffer_index);
                ResourceHandle sm1 = gpu_draw_pipeline_->GetShadowMap(1, inputs.current_buffer_index);

                // Manual barriers: shadow maps may be in UnorderedAccess state from a
                // previous frame's ShadowBlit (cache hit skips the blit this frame).
                // GBuffer depth/normal were written as RenderTarget by an earlier pass
                // that has no explicit RG dependency with this compute dispatch.
                {
                    ResourceBarrier barriers[4];
                    barriers[0].resource = sm0;
                    barriers[0].beforeState = ResourceState::Unknown;
                    barriers[0].afterState = ResourceState::ShaderResource;
                    barriers[1].resource = sm1;
                    barriers[1].beforeState = ResourceState::Unknown;
                    barriers[1].afterState = ResourceState::ShaderResource;
                    barriers[2].resource = gpu_draw_pipeline_->GetGBufferDepthSampleable();
                    barriers[2].beforeState = ResourceState::Unknown;
                    barriers[2].afterState = ResourceState::ShaderResource;
                    barriers[3].resource = gpu_draw_pipeline_->GetGBufferNormal();
                    barriers[3].beforeState = ResourceState::RenderTarget;
                    barriers[3].afterState = ResourceState::ShaderResource;
                    cmd->InsertBarrier(barriers, 4);
                }

                DescData params[] = {
                    {0, DescriptorType::UniformBuffer, shadow_filter_cb_[cbIdx]},
                    {0, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferDepthSampleable()},
                    {1, DescriptorType::SampledImage, sm0},
                    {2, DescriptorType::SampledImage, sm1},
                    {3, DescriptorType::StorageImage, shadow_visibility_tex_},
                    {4, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferNormal()},
                };
                UpdateDesc(device_, shadow_filter_ds_[cbIdx], params, 6);

                cmd->BindComputePipeline(shadow_filter_pipeline_);
                const DescriptorSetHandle sets[] = { shadow_filter_ds_[cbIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, shadow_filter_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((render_width_ / 2 + 7) / 8, (render_height_ / 2 + 7) / 8, 1);
            }
        );
    }

    return outputs;
}

} // namespace primal::graphics
