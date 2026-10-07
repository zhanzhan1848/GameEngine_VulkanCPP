#include "ShadowMapModule.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/Passes/BlurPass.h"
#include "Engine/Common/CoreLogStream.h"
#include <vector>

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

    // VSM blur infrastructure (harmless on PCSS — only dispatched when VSM is
    // enabled). BlurPass loads its own platform-aware shader.
    if (!moments_blur_) {
        moments_blur_ = std::make_unique<BlurPass>();
        if (!moments_blur_->Initialize(device_)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ShadowMapModule] BlurPass init failed — VSM unavailable" << std::endl;
            moments_blur_.reset();
        }
    }
    if (moments_temp_ == handles::INVALID_RESOURCE && device_) {
        TextureDesc tempDesc{};
        tempDesc.size = {2048, 2048, 1};
        tempDesc.format = DataFormat::RG32_Float;
        tempDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        tempDesc.memoryUsage = GPUMemoryUsage::Static;
        moments_temp_ = device_->CreateTexture(tempDesc);
    }
    return true;
}

void ShadowMapModule::Shutdown() {
    if (gpu_draw_pipeline_) {
        gpu_draw_pipeline_->ShutdownShadowResources();
    }
    shadow_cache_globally_valid_ = false;
    for (int b = 0; b < 3; ++b)
        for (int c = 0; c < 4; ++c)
            shadow_cache_valid_[b][c] = false;

    // T4.6.5 part 40: sampler cleanup. Other resources (pipeline/layout/DS/
    // CB/texture) are owned by gpu_draw_pipeline_ via ShutdownShadowResources
    // or by the device GC; sampler is module-local.
    if (shadow_filter_sampler_ != handles::INVALID_SAMPLER && device_) {
        device_->DestroySampler(shadow_filter_sampler_);
        shadow_filter_sampler_ = handles::INVALID_SAMPLER;
    }

    if (moments_blur_) {
        moments_blur_->Shutdown();
        moments_blur_.reset();
    }
    if (moments_temp_ != handles::INVALID_RESOURCE && device_) {
        device_->DestroyTexture(moments_temp_);
        moments_temp_ = handles::INVALID_RESOURCE;
    }
}

bool ShadowMapModule::InitializeShadowFilter(ShaderHandle shadow_filter_shader) {
    if (!device_) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ShadowMapModule] InitializeShadowFilter: device is null" << std::endl;
        return false;
    }
    if (shadow_filter_shader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ShadowMapModule] InitializeShadowFilter: shadow_filter_shader is INVALID_SHADER "
                  << "(real shadows disabled — DeferredLighting binding 6 falls back to 1x1 white)"
                  << std::endl;
        return false;
    }

    // T4.6.5 part 40: layout mirrors ShadowFilter.comp bindings 0-6. The
    // previous layout declared binding 0 twice (UBO + SampledImage overlap)
    // which is invalid Vulkan and silently masked by InitializeShadowFilter
    // never running until Part 40.
    // Metal: the kernel's depthTex lives at texture(0) — same binding number
    // as the Params UBO but a different namespace. MetalDescriptorSet matches
    // by (binding, type), so the Metal layout declares binding 0 twice.
    std::vector<DescriptorSetLayoutBinding> bindings = {
        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // Params UBO
        {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadowMap0
        {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // shadowMap1
        {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // P4.2: shadowMap2
        {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // P4.2: shadowMap3
        {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // visibilityOut
        {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normalTex
        {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depthTex (Vulkan)
        {6, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr},  // linearSampler
        {9, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // P4.1: translucentOut
    };
    if (device_->GetPlatform() != RHIPlatform::Vulkan) {
        bindings.push_back(
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr});  // depthTex (Metal texture(0))
    }
    shadow_filter_set_layout_ = device_->CreateDescriptorSetLayout(
        {static_cast<u32>(bindings.size()), bindings.data()});
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

    // P4.1: 透光强度纹理（r8 半分辨率，与 vis 同生命周期）
    TextureDesc transDesc{};
    transDesc.size = {(render_width_ / 2), (render_height_ / 2), 1};
    transDesc.format = DataFormat::R8_UNorm;
    transDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::RenderTarget;
    shadow_translucent_tex_ = device_->CreateTexture(transDesc);
    if (shadow_translucent_tex_ == handles::INVALID_RESOURCE) return false;

    for (int i = 0; i < 3; ++i) {
        BufferDesc cbDesc{};
        cbDesc.size = 512;
        cbDesc.type = BufferType::Constant;  // T4.6.5 part 40: UBO usage flag
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        shadow_filter_cb_[i] = device_->CreateBuffer(cbDesc);
    }

    // T4.6.5 part 40: linear sampler for all ShadowFilter texture samples.
    // comparisonFunc MUST be Never — VulkanSampler treats != Never as
    // compareEnable=TRUE which produces a comparison sampler incompatible with
    // plain SampledImage (per memory vulkan-sampler-comparison-default-trap.md).
    SamplerDesc samplerDesc{};
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    shadow_filter_sampler_ = device_->CreateSampler(samplerDesc);
    if (shadow_filter_sampler_ == handles::INVALID_SAMPLER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ShadowMapModule] CreateSampler failed" << std::endl;
        return false;
    }
    return true;
}

math::m4x4 ShadowMapModule::ComputeCascadeVP(math::v3 lightDir, math::v3 cameraPos,
                                              float orthoExtent, u32 shadowMapSize,
                                              const ShadowMapInputs& inputs, u32 cascade,
                                              float* outNearPlane, float* outFarPlane) {
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

    // B17 诊断：级联深度跨度真值。0.003 NDC 的旧常数 bias 折算世界深度 =
    // 0.003 × (farPlane - nearPlane)。实测 span≈51wu → 0.003 NDC ≈ 0.15wu。
    // 注意（B17 最终归因）：拱腹暖光并非该 bias 的泄漏——纯 vis 比较在拱内
    // 框 raw-leak≈0，vis≡1 对照证明阴影链挡掉了全部可挡直射；此打印保留用
    // 于核对 bias/offset 的世界量级。
    if (std::getenv("PRIMAL_SHADOW_CULL_DIAG") && !cascade) {
        static bool spanDumped = false;
        if (!spanDumped) {
            spanDumped = true;
            std::fprintf(stderr,
                "[CullDiag17] cascade0 span: near=%.2f far=%.2f depthSpan=%.2f "
                "bias0.003->%.3fwu texel=%.4fwu (extent %.0f/2048)\n",
                nearPlane, farPlane, farPlane - nearPlane,
                0.003f * (farPlane - nearPlane),
                (orthoExtent * 2.0f) / (float)shadowMapSize, orthoExtent);
        }
    }

    math::m4x4 lightProj = CreateOrthographicMatrix(
        -orthoExtent, orthoExtent, -orthoExtent, orthoExtent, nearPlane, farPlane);
    if (outNearPlane) *outNearPlane = nearPlane;
    if (outFarPlane) *outFarPlane = farPlane;
    return lightProj * lightView;
}

ShadowMapOutputs ShadowMapModule::AddPasses(rendergraph::RenderGraph& graph, const ShadowMapInputs& inputs) {
    ShadowMapOutputs outputs{};
    // P0（sun-audit §7）: 尺寸 provenance 真值随输出走（与级联循环内的
    // 2048 常量同源，[SunContract] 直接读取，避免导出端复写常量）。
    outputs.shadow_map_resolution = 2048;
    outputs.shadow_cascade_count = 4;
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

    const bool vsm = vsm_enabled_ && moments_blur_ &&
                     moments_temp_ != handles::INVALID_RESOURCE;

    rendergraph::RGResourceHandle shadowMapRG[4];
    rendergraph::RGResourceHandle momentsRG[4];
    for (u32 c = 0; c < 4; ++c) {
        auto smHandle = gpu_draw_pipeline_->GetShadowMap(c, inputs.current_buffer_index);
        if (smHandle != handles::INVALID_RESOURCE) {
            shadowMapRG[c] = graph.ImportResource(
                "ShadowMap_C" + std::to_string(c) + "_" + std::to_string(inputs.current_buffer_index),
                smHandle);
            // P1（reaudit §4.4）: 把 PCSS 深度图导出给体积雾（mode 2 消费）
            outputs.shadow_depth_rg[c] = shadowMapRG[c];
        }
        if (vsm) {
            auto momHandle = gpu_draw_pipeline_->GetShadowMoments(c, inputs.current_buffer_index);
            if (momHandle != handles::INVALID_RESOURCE) {
                // 2026-09-05：ImportResource 注册 Unknown 类型——下游按类型
                // 守卫解析会回退创建无 UA 的纹理（usage 0x01），blurCS2D 写入
                // 即 assert/设备丢失。改用 ImportTexture 携带完整 desc。
                rhi::TextureDesc momDesc{};
                momDesc.size = {2048, 2048, 1};
                momDesc.format = rhi::DataFormat::RG32_Float;
                momDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource |
                                rhi::TextureUsage::UnorderedAccess;
                momDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
                momentsRG[c] = graph.ImportTexture(
                    "ShadowMoments_C" + std::to_string(c) + "_" + std::to_string(inputs.current_buffer_index),
                    momHandle, momDesc);
            }
        }
    }

    auto vp_equal = [](const math::m4x4& a, const math::m4x4& b) -> bool {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a.columns[c][r] - b.columns[c][r]) > 1e-5f) return false;
        return true;
    };

    struct ShadowPassData {};
    bool rasteredThisFrame[4] = {false, false, false, false};

    // B17（2026-09-11）薄遮挡器泄漏修复：ShadowFilter 需要每级联的世界尺度
    // 参数才能做 slope 自适应 normal-offset + 世界单位 bias——常数 0.003 NDC
    // bias 折算世界深度 = 0.003×depthSpan（≈0.12-0.21wu），比拱圈壁厚
    // （0.15-0.3wu）的光空间深度差还大，是拱腹暖光泄漏的主因。布局：
    //   x = texel_world = 2×orthoExtent/2048（一张 shadow map 纹素的世界尺寸）
    //   y = depth_span = farPlane - nearPlane（NDC z=1 对应的世界深度跨度）
    //   z = orthoExtent
    //   w = 保留
    float filterCascadeParams[4][4] = {};

    // P4.2 (2026-09-15): 2→4 级联——extent 12/30/90/270（近物半影分辨率
    // 显著提升；远档 270 覆盖原 150 的全场景范围）。档位边界（世界距离）
    // 由各 extent 对角近似推得，供 cascadeSplits/选择使用。
    const float kCascadeExtents[4] = { 12.0f, 30.0f, 90.0f, 270.0f };
    for (u32 c = 0; c < 4; ++c) outputs.shadow_cascade_extents[c] = kCascadeExtents[c];
    for (u32 cascade = 0; cascade < 4; ++cascade) {
        float orthoExtent = kCascadeExtents[cascade];
        float cascadeNear = 0.0f, cascadeFar = 0.0f;
        math::m4x4 lightVP = ComputeCascadeVP(lightDir, inputs.camera_position, orthoExtent, 2048, inputs, cascade,
                                              &cascadeNear, &cascadeFar);
        filterCascadeParams[cascade][0] = (orthoExtent * 2.0f) / 2048.0f;
        filterCascadeParams[cascade][1] = cascadeFar - cascadeNear;
        filterCascadeParams[cascade][2] = orthoExtent;
        filterCascadeParams[cascade][3] = 0.0f;

        bool cache_hit = shadow_cache_globally_valid_ &&
                         shadow_cache_valid_[cbIdx][cascade] &&
                         vp_equal(lightVP, cached_shadow_vp_[cbIdx][cascade]);

        outputs.shadow_matrix[cascade] = lightVP;

        // TEMP-DIAG: CPU 侧矩阵字节真值（PRIMAL_DUMP_MATRIX=1）
        if (cascade == 0 && std::getenv("PRIMAL_DUMP_MATRIX")) {
            std::fprintf(stderr, "[MATRIX] cascade0 columns:");
            for (int c = 0; c < 4; ++c)
                std::fprintf(stderr, " (%.4f,%.4f,%.4f,%.4f)",
                             lightVP.columns[c][0], lightVP.columns[c][1],
                             lightVP.columns[c][2], lightVP.columns[c][3]);
            std::fprintf(stderr, "\n");
        }

        if (cache_hit) continue;
        rasteredThisFrame[cascade] = true;

        nanite::GPUDrivenDrawPipeline::DirectionalLightData lightData{};
        lightData.direction = {lightDir.x, lightDir.y, lightDir.z, 0.0f};
        lightData.color = {5.0f, 5.0f, 5.0f, 1.0f};
        lightData.viewPos = {inputs.camera_position.x, inputs.camera_position.y, inputs.camera_position.z, 1.0f};
        // P4.2: culling 端只消费当前级联的 VP（CPU 按 cascade%2 选槽），
        // cascade≥2 时两个槽都写本 VP 保证取到正确矩阵。
        lightData.shadowMatrix0 = lightVP;
        lightData.shadowMatrix1 = lightVP;
        // 档位边界：取相邻 extent 的对角距离近似（正交盒对角 ×2）
        float split = orthoExtent * 2.0f * 1.4142f;
        lightData.cascadeSplits = {(cascade == 0) ? split : 0.0f,
                                   (cascade == 1) ? split : 0.0f,
                                   (cascade == 2) ? split : 0.0f,
                                   (cascade == 3) ? split : 0.0f};

        // Shadow Culling
        graph.AddPass<ShadowPassData>("ShadowCulling_C" + std::to_string(cascade),
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) { builder.SideEffect(); },
            [this, lightData, cascade, bufIdx = inputs.current_buffer_index, snap = inputs.scene_snapshot]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                gpu_draw_pipeline_->ExecuteShadowCulling(context.cmdBuffer, *snap, lightData, cascade, bufIdx);
            }
        );

        // Shadow Raster — VSM writes (z, z²) moments directly (RG32 color +
        // depth-tested D32); PCSS keeps the depth-only raster + D32→R32 blit.
        graph.AddPass<ShadowPassData>("ShadowRaster_C" + std::to_string(cascade),
            rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::Lighting,
            [](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) { builder.SideEffect(); },
            [this, lightVP, cascade, bufIdx = inputs.current_buffer_index, vsm]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                if (vsm) {
                    gpu_draw_pipeline_->ExecuteShadowMomentsRaster(context.cmdBuffer, lightVP, cascade, bufIdx);
                } else {
                    gpu_draw_pipeline_->ExecuteShadowRaster(context.cmdBuffer, lightVP, cascade, bufIdx);
                }
            }
        );

        if (!vsm) {
            // Shadow Depth Blit (PCSS only)
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
        }

        cached_shadow_vp_[cbIdx][cascade] = lightVP;
        shadow_cache_valid_[cbIdx][cascade] = true;
    }

    shadow_cache_globally_valid_ = true;

    if (vsm) {
        // VSM moments blur — H+V Gaussian ping-pong per cascade. T8.13:
        // radius 3 (≈0.3 m penumbra at this cascade coverage) read as hard
        // edges across the nave; 8 gives ~1 m soft penumbra. Env
        // SHADOW_BLUR overrides (set 3 to restore the old look).
        // Runs after both cascades rasterized; BlurPass leaves the output in
        // ShaderResource for DeferredLighting's Chebyshev sampling.
        graph.AddPass<ShadowPassData>("ShadowMomentsBlur",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Lighting,
            [momentsRG](ShadowPassData&, rendergraph::RenderGraphBuilder& builder) {
                builder.SideEffect();
                for (u32 c = 0; c < 4; ++c)
                    if (momentsRG[c].IsValid())
                        builder.Write(momentsRG[c], ResourceState::UnorderedAccess);
            },
            [this, bufIdx = inputs.current_buffer_index, frameIdx = inputs.current_buffer_index,
             rastered = rasteredThisFrame]
            (const ShadowPassData&, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                for (u32 c = 0; c < 4; ++c) {
                    // VP-cache hit: moments unchanged since last frame —
                    // re-blurring would soften them progressively.
                    if (!rastered[c]) continue;
                    u32 blurRadius = 8u;
                    if (const char* e = std::getenv("SHADOW_BLUR")) blurRadius = (u32)std::max(1, atoi(e));
                    ResourceHandle mom = gpu_draw_pipeline_->GetShadowMoments(c, bufIdx);
                    if (mom == handles::INVALID_RESOURCE) continue;

                    // Raster left the moments in RenderTarget state.
                    ResourceBarrier b{};
                    b.resource = mom;
                    b.beforeState = ResourceState::RenderTarget;
                    b.afterState = ResourceState::ShaderResource;
                    b.subresource = 0xFFFFFFFF;
                    b.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&b, 1);

                    moments_blur_->Execute(cmd, mom, mom, moments_temp_,
                                           2048, 2048, 1, frameIdx, blurRadius, 1.0f);
                }
            }
        );

        for (u32 c = 0; c < 4; ++c) {
            outputs.shadow_moments_rg[c] = momentsRG[c];
            outputs.shadow_moments_tex[c] = gpu_draw_pipeline_->GetShadowMoments(c, inputs.current_buffer_index);
        }
        outputs.vsm_enabled = true;
        return outputs;
    }

    // Shadow Filter (half-res compute, PCSS path)
    outputs.shadow_visibility_tex = shadow_visibility_tex_;
    if (shadow_filter_pipeline_ != handles::INVALID_PIPELINE && shadow_visibility_tex_ != handles::INVALID_RESOURCE) {
        outputs.shadow_visibility_rg = graph.ImportResource("ShadowVisibility", shadow_visibility_tex_);
        outputs.shadow_translucent_tex = shadow_translucent_tex_;
        outputs.shadow_translucent_rg = graph.ImportResource("ShadowTranslucent", shadow_translucent_tex_);

        struct ShadowFilterData { rendergraph::RGResourceHandle output; };

        // P4.2: capture 4 cascades 的 VP 与世界尺度参数
        math::m4x4 mats[4] = {outputs.shadow_matrix[0], outputs.shadow_matrix[1],
                              outputs.shadow_matrix[2], outputs.shadow_matrix[3]};
        const float cascadeParams[4][4] = {
            {filterCascadeParams[0][0], filterCascadeParams[0][1],
             filterCascadeParams[0][2], filterCascadeParams[0][3]},
            {filterCascadeParams[1][0], filterCascadeParams[1][1],
             filterCascadeParams[1][2], filterCascadeParams[1][3]},
            {filterCascadeParams[2][0], filterCascadeParams[2][1],
             filterCascadeParams[2][2], filterCascadeParams[2][3]},
            {filterCascadeParams[3][0], filterCascadeParams[3][1],
             filterCascadeParams[3][2], filterCascadeParams[3][3]},
        };

        graph.AddPass<ShadowFilterData>("ShadowFilter",
            rendergraph::RGPassType::Compute, rendergraph::RGPassCategory::Copy,
            [visRG = outputs.shadow_visibility_rg, transRG = outputs.shadow_translucent_rg,
             shadowMapRG](ShadowFilterData& data, rendergraph::RenderGraphBuilder& builder) {
                data.output = builder.Write(visRG, ResourceState::UnorderedAccess);
                if (transRG.IsValid()) builder.Write(transRG, ResourceState::UnorderedAccess);
                for (u32 c = 0; c < 4; ++c)
                    if (shadowMapRG[c].IsValid())
                        builder.Read(shadowMapRG[c], ResourceState::ShaderResource);
            },
            [this, inputs, cbIdx, mats, cascadeParams](const ShadowFilterData&, rendergraph::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                math::m4x4 invVP = Inverse(inputs.proj_matrix * inputs.view_matrix);
                // TEMP-DIAG: shadow 模块的 proj/view 真值
                if (std::getenv("PRIMAL_DUMP_MATRIX")) {
                    std::fprintf(stderr, "[SHADOW_V] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                                 inputs.view_matrix.columns[0].x, inputs.view_matrix.columns[0].y, inputs.view_matrix.columns[0].z, inputs.view_matrix.columns[0].w,
                                 inputs.view_matrix.columns[1].x, inputs.view_matrix.columns[1].y, inputs.view_matrix.columns[1].z, inputs.view_matrix.columns[1].w,
                                 inputs.view_matrix.columns[2].x, inputs.view_matrix.columns[2].y, inputs.view_matrix.columns[2].z, inputs.view_matrix.columns[2].w,
                                 inputs.view_matrix.columns[3].x, inputs.view_matrix.columns[3].y, inputs.view_matrix.columns[3].z, inputs.view_matrix.columns[3].w);
                    std::fprintf(stderr, "[SHADOW_P] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                                 inputs.proj_matrix.columns[0].x, inputs.proj_matrix.columns[0].y, inputs.proj_matrix.columns[0].z, inputs.proj_matrix.columns[0].w,
                                 inputs.proj_matrix.columns[1].x, inputs.proj_matrix.columns[1].y, inputs.proj_matrix.columns[1].z, inputs.proj_matrix.columns[1].w,
                                 inputs.proj_matrix.columns[2].x, inputs.proj_matrix.columns[2].y, inputs.proj_matrix.columns[2].z, inputs.proj_matrix.columns[2].w,
                                 inputs.proj_matrix.columns[3].x, inputs.proj_matrix.columns[3].y, inputs.proj_matrix.columns[3].z, inputs.proj_matrix.columns[3].w);
                }
                math::v3 L = Normalize(inputs.light_direction);

                struct Params {
                    math::m4x4 inv_view_proj;
                    math::m4x4 shadow_vp[4];   // P4.2: 4 级联
                    math::v4   light_dir;
                    math::v2   texel_size;
                    math::v2   depth_texel_size;
                    u32 shadow_quality;
                    u32 render_width;
                    u32 render_height;
                    // sun-audit §5.3: 复用原 _pad0 槽（float，std140 布局不变，
                    // Metal 孪生 kernel 读到的是无语义 pad——其有意分歧注释
                    // 继续成立）。值 = tan(角直径/2)；0 = 回退固定 0.012。
                    float sun_tan_half_angle;
                    // B17: per-cascade {texel_world, depth_span, ortho_extent, _}
                    math::v4   cascade_params[4];
                };

                // 0.5357° 物理日光（sun-audit §5.3）；夹在 [0.05°, 10°]。
                // 0（或负）= 显式关档：tan_half 记 0，shader 回退固定 0.012。
                const bool sunDiameterOn = inputs.sun_angular_diameter_deg > 0.0f;
                const float sunDiameterDeg = sunDiameterOn
                    ? std::min(std::max(inputs.sun_angular_diameter_deg, 0.05f), 10.0f)
                    : 0.0f;
                const float sunTanHalf =
                    sunDiameterOn
                        ? std::tan(sunDiameterDeg * (3.14159265f / 180.0f) * 0.5f)
                        : 0.0f;

                auto* p = static_cast<Params*>(device_->MapBuffer(shadow_filter_cb_[cbIdx]));
                if (p) {
                    p->inv_view_proj = invVP;
                    // TEMP-DIAG: filter UBO 的 invVP 真值
                    if (std::getenv("PRIMAL_DUMP_MATRIX")) {
                        std::fprintf(stderr, "[FILTER_INVP] (%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f) (%.4f,%.4f,%.4f,%.4f)\n",
                                     invVP.columns[0].x, invVP.columns[0].y, invVP.columns[0].z, invVP.columns[0].w,
                                     invVP.columns[1].x, invVP.columns[1].y, invVP.columns[1].z, invVP.columns[1].w,
                                     invVP.columns[2].x, invVP.columns[2].y, invVP.columns[2].z, invVP.columns[2].w,
                                     invVP.columns[3].x, invVP.columns[3].y, invVP.columns[3].z, invVP.columns[3].w);
                    }
                    for (u32 c = 0; c < 4; ++c) p->shadow_vp[c] = mats[c];
                    p->light_dir = math::v4{L.x, L.y, L.z, 0.0f};
                    p->texel_size = math::v2{1.0f / 2048.0f, 1.0f / 2048.0f};
                    p->depth_texel_size = math::v2{1.0f / (float)render_width_, 1.0f / (float)render_height_};
                    // 伪影1 探针：PRIMAL_SHADOW_WHITE=1 → shadow_quality=255 哨兵，
                    // ShadowFilter.metal kernel 直接写全白 vis（无阴影）。
                    // PRIMAL_SHADOW_DBG_DEPTH/RECV=1 → quality 3/4：直接可视化
                    // 比较两端（阴影图深度 / 接收器 sc0.z），定位投影错位。
                    p->shadow_quality = std::getenv("PRIMAL_SHADOW_WHITE")
                        ? 255u
                        : (std::getenv("PRIMAL_SHADOW_DBG_DEPTH") ? 3u
                           : (std::getenv("PRIMAL_SHADOW_DBG_RECV") ? 4u
                              : (std::getenv("PRIMAL_SHADOW_DBG_HARD") ? 5u
                                 : (std::getenv("PRIMAL_SHADOW_DBG_BLOCKER") ? 6u
                                    : (std::getenv("PRIMAL_SHADOW_DBG_NORMAL") ? 7u
                                       : static_cast<u32>(inputs.shadow_quality))))));
                    p->render_width = render_width_;
                    p->render_height = render_height_;
                    p->sun_tan_half_angle = sunTanHalf;
                    for (u32 c = 0; c < 4; ++c) {
                        p->cascade_params[c] = math::v4{cascadeParams[c][0], cascadeParams[c][1],
                                                        cascadeParams[c][2], cascadeParams[c][3]};
                    }
                    // B17: quality 解析真值 + 级联参数（一次性）
                    static bool filterParamLogged = false;
                    if (!filterParamLogged) {
                        filterParamLogged = true;
                        std::fprintf(stderr,
                            "[FilterParams] quality=%u cascade0(texel=%.4f span=%.1f) cascade1(texel=%.4f span=%.1f) sun_diameter=%.4fdeg tan_half=%.6f\n",
                            p->shadow_quality, cascadeParams[0][0], cascadeParams[0][1],
                            cascadeParams[1][0], cascadeParams[1][1],
                            sunDiameterDeg, sunTanHalf);
                    }
                    device_->UnmapBuffer(shadow_filter_cb_[cbIdx]);
                }

                ResourceHandle sm[4];
                for (u32 c = 0; c < 4; ++c)
                    sm[c] = gpu_draw_pipeline_->GetShadowMap(c, inputs.current_buffer_index);

                // Manual barriers: shadow maps may be in UnorderedAccess state from a
                // previous frame's ShadowBlit (cache hit skips the blit this frame).
                // GBuffer depth/normal were written as RenderTarget by an earlier pass
                // that has no explicit RG dependency with this compute dispatch.
                // 2026-09-07：vis 纹理先转入 GENERAL（imageStore 写入要求）——
                // 上一帧 sampled 读取后处于 SHADER_READ_ONLY，RG 未为 storage
                // 写入发转换，写入被丢弃 → vis 恒 0（直射全域死亡根因之二）。
                {
                    rhi::ResourceBarrier visToStorage{};
                    visToStorage.resource = shadow_visibility_tex_;
                    visToStorage.beforeState = rhi::ResourceState::ShaderResource;
                    visToStorage.afterState = rhi::ResourceState::UnorderedAccess;
                    visToStorage.subresource = 0xFFFFFFFF;
                    visToStorage.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&visToStorage, 1);
                    rhi::ResourceBarrier transToStorage{};
                    transToStorage.resource = shadow_translucent_tex_;
                    transToStorage.beforeState = rhi::ResourceState::ShaderResource;
                    transToStorage.afterState = rhi::ResourceState::UnorderedAccess;
                    transToStorage.subresource = 0xFFFFFFFF;
                    transToStorage.queueFamily = 0xFFFFFFFF;
                    cmd->InsertBarrier(&transToStorage, 1);
                }
                {
                    // P4.2: 4 张级联 shadow map 全部 Unknown→SR
                    ResourceBarrier barriers[6];
                    for (u32 c = 0; c < 4; ++c) {
                        barriers[c].resource = sm[c];
                        barriers[c].beforeState = ResourceState::Unknown;
                        barriers[c].afterState = ResourceState::ShaderResource;
                    }
                    barriers[4].resource = gpu_draw_pipeline_->GetGBufferDepthSampleable();
                    barriers[4].beforeState = ResourceState::Unknown;
                    barriers[4].afterState = ResourceState::ShaderResource;
                    barriers[5].resource = gpu_draw_pipeline_->GetGBufferNormal();
                    barriers[5].beforeState = ResourceState::RenderTarget;
                    barriers[5].afterState = ResourceState::ShaderResource;
                    cmd->InsertBarrier(barriers, 6);
                }

                // Texture slot conventions differ per backend:
                //   Vulkan GLSL (ShadowFilter.comp): depth@5, normal@4,
                //     sm0/sm1/out @1/2/3, sampler@6.
                //   Metal kernel (ShadowFilter.metal): depthTex@0, sm0@1,
                //     sm1@2, visibilityOut@3 — no normal/sampler params.
                // The flat {1..6} table bound depth@5 on Metal too, leaving the
                // kernel's texture(0) (depth) UNBOUND → vis sampled garbage →
                // direct-light visibility dead on the whole Metal PCSS path.
                const bool isVkShadow = device_->GetPlatform() == RHIPlatform::Vulkan;
                // P2-B: binding 9（translucentOut）此前从未写入——DSL 声明了
                // 该 storage image 但 filterParams 只填到 8，validation 报
                // "never been updated"，且透光输出实际未落到任何资源。
                DescData filterParams[10] = {
                    {0, DescriptorType::UniformBuffer, shadow_filter_cb_[cbIdx]},
                    {1, DescriptorType::SampledImage, sm[0]},
                    {2, DescriptorType::SampledImage, sm[1]},
                    {3, DescriptorType::StorageImage, shadow_visibility_tex_},
                    {4, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferNormal()},
                    {5, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferDepthSampleable()},
                    {6, DescriptorType::Sampler, shadow_filter_sampler_},
                    {7, DescriptorType::SampledImage, sm[2]},   // P4.2
                    {8, DescriptorType::SampledImage, sm[3]},   // P4.2
                    {9, DescriptorType::StorageImage, shadow_translucent_tex_},  // P2-B
                };
                if (!isVkShadow) {
                    filterParams[1] = {0, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferDepthSampleable()};
                    filterParams[2] = {1, DescriptorType::SampledImage, sm[0]};
                    filterParams[3] = {2, DescriptorType::SampledImage, sm[1]};
                    filterParams[4] = {3, DescriptorType::StorageImage, shadow_visibility_tex_};
                    filterParams[5] = {4, DescriptorType::SampledImage, gpu_draw_pipeline_->GetGBufferNormal()};
                    filterParams[6] = {6, DescriptorType::Sampler, shadow_filter_sampler_};
                }
                UpdateDesc(device_, shadow_filter_ds_[cbIdx], filterParams, 10);

                cmd->BindComputePipeline(shadow_filter_pipeline_);
                const DescriptorSetHandle sets[] = { shadow_filter_ds_[cbIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, shadow_filter_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((render_width_ / 2 + 7) / 8, (render_height_ / 2 + 7) / 8, 1);

                // 2026-09-07 修复：imageStore 使 vis 纹理处于 GENERAL 布局，
                // 下游 deferred 的 sampled 读取需要 SHADER_READ_ONLY。RG 状态
                // 跟踪误报 before=SHADER_READ_ONLY（验证层 VUID-vkCmdPipeline
                // Barrier-oldLayout-01197 ×20/帧，实际 GENERAL）→ 转换无效 →
                // 采样读到未定义值 → vis 全 0 → 直射全域死亡。显式补正确
                // 屏障，RG 后续转换变为合法 no-op。
                rhi::ResourceBarrier visBarrier{};
                visBarrier.resource = shadow_visibility_tex_;
                visBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
                visBarrier.afterState = rhi::ResourceState::ShaderResource;
                visBarrier.subresource = 0xFFFFFFFF;
                visBarrier.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&visBarrier, 1);
                // P4.1: 透光纹理同 vis——imageStore 后回 SR 供 DeferredLighting 采样
                rhi::ResourceBarrier transBarrier{};
                transBarrier.resource = shadow_translucent_tex_;
                transBarrier.beforeState = rhi::ResourceState::UnorderedAccess;
                transBarrier.afterState = rhi::ResourceState::ShaderResource;
                transBarrier.subresource = 0xFFFFFFFF;
                transBarrier.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&transBarrier, 1);
            }
        );
    }

    return outputs;
}

} // namespace primal::graphics
