#include "SCDDGIIntegrationModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;

// ============================================================================
// Descriptor update helper (same pattern as ScreenProbeGIPass)
// ============================================================================

namespace {
struct DescriptorData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set,
                         const DescriptorData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
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
        } else if (params[i].type == DescriptorType::SampledImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}
} // namespace

// ============================================================================
// Initialize — pipelines + descriptor layouts matching the shader contracts:
//   ddgi_card_radiance_avg:           Vulkan flat: texture(0) atlas, buffer(1)
//                                     SurfaceCacheParams, buffer(2) cards,
//                                     buffer(3) card_radiance out; Metal keeps
//                                     buffer(0-2) + texture(0).
//   ddgi_probe_irradiance_from_cards: buffer(0) DDGIVolumeData, buffer(1) ranges,
//                                     buffer(2) contribs, buffer(3) card_radiance,
//                                     buffer(4) irradiance hist, buffer(5) irradiance out
// ============================================================================

bool SCDDGIIntegrationModule::Initialize(RHIDeviceBase* device,
                                           ShaderHandle card_radiance_shader,
                                           ShaderHandle probe_irradiance_shader,
                                           u32 render_width, u32 render_height) {
    device_ = device;
    (void)render_width;
    (void)render_height;

    // Vulkan shaders declare flat bindings (image 0, UBO 1, SSBO 2-3); Metal
    // keeps the overlapping texture/buffer namespaces of the original kernels.
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;

    if (card_radiance_shader != handles::INVALID_SHADER) {
        DescriptorSetLayoutBinding bindings[4];
        if (isVk) {
            bindings[0] = {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // lighting atlas
            bindings[1] = {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};  // SurfaceCacheParams
            bindings[2] = {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};  // cards
            bindings[3] = {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};  // card_radiance (out)
        } else {
            bindings[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};  // SurfaceCacheParams
            bindings[1] = {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};  // cards
            bindings[2] = {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr};  // card_radiance (out)
            bindings[3] = {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr};  // lighting atlas
        }
        card_radiance_set_layout_ = device->CreateDescriptorSetLayout({4, bindings});
        card_radiance_layout_ = device->CreatePipelineLayout({1, &card_radiance_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = card_radiance_shader;
        pd.layout = card_radiance_layout_;
        pd.threadGroupSize = {64, 1, 1};
        card_radiance_pipeline_ = device->CreateComputePipeline(pd);

        for (int i = 0; i < 3; ++i)
            card_radiance_ds_[i] = device->CreateDescriptorSet({card_radiance_set_layout_});
    }

    if (probe_irradiance_shader != handles::INVALID_SHADER) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // DDGIVolumeData
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ProbeContribRange
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ProbeCardContrib
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_radiance
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance history
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // irradiance output
        };
        probe_irradiance_set_layout_ = device->CreateDescriptorSetLayout({6, bindings});
        probe_irradiance_layout_ = device->CreatePipelineLayout({1, &probe_irradiance_set_layout_});

        ComputePipelineDesc pd{};
        pd.computeShader = probe_irradiance_shader;
        pd.layout = probe_irradiance_layout_;
        pd.threadGroupSize = {64, 1, 1};
        probe_irradiance_pipeline_ = device->CreateComputePipeline(pd);

        for (int i = 0; i < 3; ++i)
            probe_irradiance_ds_[i] = device->CreateDescriptorSet({probe_irradiance_set_layout_});
    }

    return true;
}

void SCDDGIIntegrationModule::Shutdown() {}

// ============================================================================
// Resources — card_radiance uses 16B/card stride: Metal's device float3* array
// indexing has a 16-byte stride, so 12B packing would misalign every card
// after the first (the test harness allocated 12B and got away with it only
// because probe reads clamp by contribution count).
// ============================================================================

bool SCDDGIIntegrationModule::EnsureResources(lumen::LumenDDGIPass* ddgi, lumen::SurfaceCachePass* sc) {
    if (assignment_built_) return true;
    if (!ddgi || !sc || !ddgi->IsInitialized() || !sc->IsInitialized()) return false;

    const auto& params = ddgi->GetParams();
    total_probes_ = params.probe_count_x * params.probe_count_y * params.probe_count_z;
    max_cards_ = 4096;   // matches SurfaceCacheParams max_cards default
    if (total_probes_ == 0 || sc->GetCardGenerator().GetCardCount() == 0) return false;

    if (card_radiance_buf_ == handles::INVALID_RESOURCE) {
        BufferDesc radDesc{};
        radDesc.size = (u64)max_cards_ * 16;   // float3 @ 16B stride
        radDesc.type = BufferType::Structured;
        radDesc.usage = GPUMemoryUsage::Dynamic;
        radDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        card_radiance_buf_ = device_->CreateBuffer(radDesc);
    }
    if (probe_contrib_range_buf_ == handles::INVALID_RESOURCE) {
        BufferDesc d{};
        d.size = (u64)total_probes_ * sizeof(SCProbeContribRange);
        d.type = BufferType::Structured;
        d.usage = GPUMemoryUsage::Dynamic;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        probe_contrib_range_buf_ = device_->CreateBuffer(d);
    }
    if (flat_contrib_buf_ == handles::INVALID_RESOURCE) {
        BufferDesc d{};
        d.size = (u64)total_probes_ * SC_MAX_CONTRIBS_PER_PROBE * sizeof(SCProbeCardContrib);
        d.type = BufferType::Structured;
        d.usage = GPUMemoryUsage::Dynamic;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        flat_contrib_buf_ = device_->CreateBuffer(d);
    }
    if (sc_params_cb_ == handles::INVALID_RESOURCE) {
        BufferDesc d{};
        d.size = 256;
        d.type = BufferType::Constant;
        d.usage = GPUMemoryUsage::Dynamic;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        sc_params_cb_ = device_->CreateBuffer(d);
    }
    for (int i = 0; i < 3; ++i) {
        if (ddgi_vol_cb_[i] == handles::INVALID_RESOURCE) {
            BufferDesc d{};
            d.size = sizeof(lumen::DDGIVolumeData);
            d.type = BufferType::Constant;
            d.usage = GPUMemoryUsage::Dynamic;
            d.memoryUsage = GPUMemoryUsage::Dynamic;
            ddgi_vol_cb_[i] = device_->CreateBuffer(d);
        }
    }

    return BuildCardProbeAssignment(ddgi, sc);
}

// ============================================================================
// CPU contribution builder (ported from TestNaniteStreamingPipeline —
// solid-angle × NdotL × SH-basis weights, top-6 per probe by weight).
// ============================================================================

bool SCDDGIIntegrationModule::BuildCardProbeAssignment(lumen::LumenDDGIPass* ddgi, lumen::SurfaceCachePass* sc) {
    const auto& cards = sc->GetCardGenerator().GetCards();
    const u32 cardCount = sc->GetCardGenerator().GetCardCount();

    const auto& ddgiParams = ddgi->GetParams();
    const auto& ddgiVol = ddgi->GetVolumeData();
    const math::v3 probeOrigin{ddgiVol.ProbeOrigin.x, ddgiVol.ProbeOrigin.y, ddgiVol.ProbeOrigin.z};
    const float probeSpacing = ddgiParams.probe_spacing;
    // 3× spacing (9m at 3m spacing) was too tight for interior scenes: probes
    // in one arcade never saw the sunlit nave floor or the opposite (directly
    // lit) wall, so shaded-side probes gathered only their own unlit local
    // cards and DDGI collapsed there. 6× spacing (min 24m) lets the dominant
    // distant emitters into the contribution set; the top-N weight cap keeps
    // the per-probe read budget unchanged.
    const float maxDist = std::max(probeSpacing * 6.0f, 24.0f);

    struct ProbeBuilder {
        SCProbeCardContrib entries[SC_MAX_CONTRIBS_PER_PROBE];
        float weights[SC_MAX_CONTRIBS_PER_PROBE];
        u32 count = 0;
    };
    std::vector<ProbeBuilder> builders(total_probes_);

    constexpr float PI = 3.14159265358979f;
    constexpr float FOUR_PI = 4.0f * PI;
    constexpr float SH_C0   = 0.282095f;
    constexpr float SH_C1   = 0.488603f;
    constexpr float SH_C2_0 = 1.092548f;
    constexpr float SH_C2_1 = 0.315392f;
    constexpr float SH_C2_2 = 0.546274f;

    for (u32 probeIdx = 0; probeIdx < total_probes_; ++probeIdx) {
        auto& b = builders[probeIdx];
        const u32 ix = probeIdx % ddgiParams.probe_count_x;
        const u32 iy = (probeIdx / ddgiParams.probe_count_x) % ddgiParams.probe_count_y;
        const u32 iz = probeIdx / (ddgiParams.probe_count_x * ddgiParams.probe_count_y);
        const math::v3 probePos{
            probeOrigin.x + (float)ix * probeSpacing,
            probeOrigin.y + (float)iy * probeSpacing,
            probeOrigin.z + (float)iz * probeSpacing};

        for (u32 c = 0; c < cardCount; ++c) {
            const auto& card = cards[c];
            if (card.resolution == 0) continue;

            const float dx = card.center.x - probePos.x;
            const float dy = card.center.y - probePos.y;
            const float dz = card.center.z - probePos.z;
            const float dist2 = dx * dx + dy * dy + dz * dz;
            if (dist2 > maxDist * maxDist) continue;

            const float dist = std::sqrt(dist2);
            if (dist < 0.01f) continue;

            const float invDist = 1.0f / dist;
            const float dirX = dx * invDist, dirY = dy * invDist, dirZ = dz * invDist;

            const uint8_t axis = card.axis_direction & 0xFF;
            const uint8_t dir  = (card.axis_direction >> 8) & 0xFF;
            math::v3 cardNormal{0, 0, 0};
            if (axis == 0) cardNormal.x = dir ? 1.0f : -1.0f;
            else if (axis == 1) cardNormal.y = dir ? 1.0f : -1.0f;
            else cardNormal.z = dir ? 1.0f : -1.0f;

            const float NdotL = -(cardNormal.x * dirX + cardNormal.y * dirY + cardNormal.z * dirZ);
            if (NdotL < 0.01f) continue;

            float cardArea;
            if (axis == 0) cardArea = card.extent.y * card.extent.z * 4.0f;
            else if (axis == 1) cardArea = card.extent.x * card.extent.z * 4.0f;
            else cardArea = card.extent.x * card.extent.y * 4.0f;
            if (cardArea < 0.001f) continue;

            const float solidAngle = cardArea / (dist2 + 1.0f);
            // 2026-09-05 物理化归一化（修"GI 平了"）：辐射度投影本形
            // C_j = Σ Ω_i·L_i·Y_j(dir_i)——不带 4π（Y0 基已归一化；旧
            // ×FOUR_PI 在 Ω 过冲时把近距探针的原始 L0 放大 ~1000×，
            // 迫使 shader 侧用平顶门压平，牺牲全部空间对比）。Ω 物理封顶
            // π：探针位于卡片包围盒内时 area/(d²+1) 可非物理地超 4π。
            // NdotL 保留——掠射角下卡片有效投影面积收缩。
            const float solidAngleCapped = std::min(solidAngle, 3.14159265f);
            const float totalWeight = solidAngleCapped * NdotL;

            // Full L0-L2 basis, signs/order matching DDGIUpdateIrradiance's
            // projection (band 6 is the C2_1 zonal term; x/y L1 and yz/xz L2
            // carry the minus signs).
            const float basis[9] = {
                SH_C0,
                -SH_C1 * dirY,
                 SH_C1 * dirZ,
                -SH_C1 * dirX,
                 SH_C2_0 * dirY * dirX,
                -SH_C2_0 * dirY * dirZ,
                 SH_C2_1 * (3.0f * dirZ * dirZ - 1.0f),
                -SH_C2_0 * dirX * dirZ,
                 SH_C2_2 * (dirX * dirX - dirY * dirY)};

            SCProbeCardContrib contrib;
            contrib.card_index = c;
            for (int i = 0; i < 9; ++i) contrib.sh_weights[i] = totalWeight * basis[i];

            if (b.count < SC_MAX_CONTRIBS_PER_PROBE) {
                b.entries[b.count] = contrib;
                b.weights[b.count] = totalWeight;
                ++b.count;
            } else {
                u32 minIdx = 0;
                for (u32 j = 1; j < SC_MAX_CONTRIBS_PER_PROBE; ++j) {
                    if (b.weights[j] < b.weights[minIdx]) minIdx = j;
                }
                if (totalWeight > b.weights[minIdx]) {
                    b.entries[minIdx] = contrib;
                    b.weights[minIdx] = totalWeight;
                }
            }
        }
    }

    std::vector<SCProbeContribRange> ranges(total_probes_);
    std::vector<SCProbeCardContrib> flat;
    flat.reserve((size_t)total_probes_ * SC_MAX_CONTRIBS_PER_PROBE);
    for (u32 p = 0; p < total_probes_; ++p) {
        ranges[p].start = (u32)flat.size();
        ranges[p].count = builders[p].count;
        for (u32 i = 0; i < builders[p].count; ++i) flat.push_back(builders[p].entries[i]);
    }

    {
        auto* mapped = static_cast<SCProbeContribRange*>(device_->MapBuffer(probe_contrib_range_buf_));
        if (mapped) {
            memcpy(mapped, ranges.data(), ranges.size() * sizeof(SCProbeContribRange));
            device_->UnmapBuffer(probe_contrib_range_buf_);
        }
    }
    {
        auto* mapped = static_cast<SCProbeCardContrib*>(device_->MapBuffer(flat_contrib_buf_));
        if (mapped) {
            memcpy(mapped, flat.data(), flat.size() * sizeof(SCProbeCardContrib));
            device_->UnmapBuffer(flat_contrib_buf_);
        }
    }
    {
        auto* mapped = static_cast<lumen::SurfaceCacheParams*>(device_->MapBuffer(sc_params_cb_));
        if (mapped) {
            lumen::SurfaceCacheParams params{};
            params.atlas_size = sc->GetAtlasSize();
            params.page_size = sc->GetPageSize();
            params.max_cards = cardCount;
            params.lookup_count = sc->GetCardGenerator().GetLookupCount();
            *mapped = params;
            device_->UnmapBuffer(sc_params_cb_);
        }
    }

    assignment_card_count_ = sc->GetCardGenerator().GetCardCount();
    assignment_built_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[SC→DDGI] Card→Probe assignment built: " << cardCount << " cards, "
              << total_probes_ << " probes, " << flat.size() << " contributions" << std::endl;
    return true;
}

// ============================================================================
// AddPass — CardRadianceAvg → barrier → ProbeIrradianceFromCards.
// RG setup mirrors the proven test orchestration: read deps on the SC atlas
// AND both DDGI irradiance handles so the RenderGraph orders this pass after
// DDGI's own irradiance update (both write irradiance_buffers_; without the
// edges the two passes can overlap and hang Apple Silicon).
// ============================================================================

void SCDDGIIntegrationModule::AddPass(rendergraph::RenderGraph& graph, const SCDDGIInputs& inputs) {
    if (!inputs.ddgi_pass || !inputs.surface_cache_pass) return;
    if (card_radiance_pipeline_ == handles::INVALID_PIPELINE ||
        probe_irradiance_pipeline_ == handles::INVALID_PIPELINE) return;

    static const bool traceOnce = [] {
        if (std::getenv("SC_DUMP_CARDS")) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[SCDDGI] AddPass active (dump enabled)" << std::endl;
            return true;
        }
        return false;
    }();
    (void)traceOnce;

    struct PassData {};

    graph.AddPass<PassData>("SC_DDGI_Integration",
        rendergraph::RGPassType::Compute,
        rendergraph::RGPassCategory::Lighting,
        [scRG = inputs.sc_lighting_rg,
         irrRG = inputs.ddgi_irradiance,
         irrHistRG = inputs.ddgi_irradiance_hist](PassData&, rendergraph::RenderGraphBuilder& builder) {
            if (scRG.IsValid())
                builder.Read(scRG, ResourceState::ShaderResource);
            // Ordering guard: DDGI's update writes these buffers first; this
            // pass read+modifies them (bypasses RG via direct buffer access),
            // and DDGIGIGather reads the result afterwards.
            if (irrRG.IsValid())
                builder.Read(irrRG, ResourceState::ShaderResource);
            if (irrHistRG.IsValid())
                builder.Read(irrHistRG, ResourceState::ShaderResource);
            builder.SideEffect();
        },
        [this, inputs](const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            auto* ddgi = inputs.ddgi_pass;
            auto* sc = inputs.surface_cache_pass;

            const u32 cardCount = sc->GetCardGenerator().GetCardCount();
            if (cardCount == 0) return;
            if (!EnsureResources(ddgi, sc)) return;
            // T8.13 fix: rebuild the per-probe card assignment whenever the
            // SC card set was regenerated (count changed) — the baked card
            // indices go stale otherwise (see header note).
            if (assignment_card_count_ != cardCount) {
                // Rate-limit: card counts can oscillate while streaming
                // registers meshes; a rebuild is heavy CPU (probes x cards).
                static u32 lastRebuildFrame = 0;
                if (inputs.frame_index - lastRebuildFrame >= 90u) {
                    lastRebuildFrame = inputs.frame_index;
                    assignment_built_ = false;
                    if (!EnsureResources(ddgi, sc)) return;
                }
            }

            // SC_DUMP_CARDS=1 — deferred print of the previous dump's copy.
            if (dump_pending_frame_ != 0xFFFFFFFFu &&
                inputs.frame_index > dump_pending_frame_ + 3u) {
                if (auto* rad = static_cast<const float*>(
                        device_->MapBuffer(card_radiance_staging_))) {
                    const auto& cards = sc->GetCardGenerator().GetCards();
                    const u32 n = dump_card_count_;
                    std::vector<u32> order(n);
                    for (u32 i = 0; i < n; ++i) order[i] = i;
                    auto lum = [&](u32 i) {
                        return 0.2126f * rad[i * 4 + 0] +
                               0.7152f * rad[i * 4 + 1] +
                               0.0722f * rad[i * 4 + 2];
                    };
                    std::sort(order.begin(), order.end(),
                              [&](u32 a, u32 b) { return lum(a) > lum(b); });
                    u32 litCount = 0;
                    for (u32 i = 0; i < n; ++i) litCount += (lum(i) > 0.05f) ? 1 : 0;
                    // Spread diagnostics: percentiles over lit cards show
                    // whether the field carries sun/shade contrast or has
                    // collapsed to a flat plateau.
                    {
                        std::vector<float> lums;
                        lums.reserve(litCount);
                        for (u32 i = 0; i < n; ++i)
                            if (lum(i) > 0.05f) lums.push_back(lum(i));
                        if (!lums.empty()) {
                            std::sort(lums.begin(), lums.end());
                            auto pct = [&](float p) {
                                return lums[std::min(lums.size() - 1,
                                                     (size_t)(p * (lums.size() - 1)))];
                            };
                            PRIMAL_LOG_STREAM(Error, "Render") << "  spread: p10=" << pct(0.10f)
                                      << " p50=" << pct(0.50f)
                                      << " p90=" << pct(0.90f)
                                      << " max=" << lums.back() << std::endl;
                        }
                    }
                    PRIMAL_LOG_STREAM(Error, "Render") << "[SC_CARDS] frame=" << dump_pending_frame_
                              << " cards=" << n << " lit(lum>0.05)=" << litCount
                              << " — top 24 by radiance:" << std::endl;
                    for (u32 k = 0; k < n && k < 24; ++k) {
                        const u32 i = order[k];
                        const auto& c = cards[i];
                        PRIMAL_LOG_STREAM(Error, "Render") << "  #" << k << " card=" << i
                                  << " center=(" << (int)c.center.x << "," << (int)c.center.y
                                  << "," << (int)c.center.z << ")"
                                  << " axis=" << (c.axis_direction & 0xFF)
                                  << " rgb=(" << rad[i * 4 + 0] << "," << rad[i * 4 + 1]
                                  << "," << rad[i * 4 + 2] << ")"
                                  << " cov=" << rad[i * 4 + 3] << std::endl;
                    }
                    device_->UnmapBuffer(card_radiance_staging_);
                }
                // TEMP: direct map of the live card radiance buffer (staging
                // copy above can lag / hit uninitialized heap on frame 0).
                if (auto* rad = static_cast<const float*>(
                        device_->MapBuffer(card_radiance_buf_))) {
                    u32 n2 = dump_card_count_;
                    std::vector<float> lums2;
                    for (u32 i = 0; i < n2; ++i) {
                        float l = 0.2126f*rad[i*4+0] + 0.7152f*rad[i*4+1] + 0.0722f*rad[i*4+2];
                        if (!std::isnan(l)) lums2.push_back(l);
                    }
                    if (!lums2.empty()) {
                        std::sort(lums2.begin(), lums2.end());
                        PRIMAL_LOG_STREAM(Error, "Render") << "  [live] cards lit(p50>0.05)="
                                  << std::count_if(lums2.begin(), lums2.end(),
                                                   [](float v){ return v > 0.05f; })
                                  << "/" << n2
                                  << " p50=" << lums2[n2/2] << " p90=" << lums2[(n2*9)/10]
                                  << " max=" << lums2.back() << std::endl;
                    }
                    device_->UnmapBuffer(card_radiance_buf_);
                }

                // Probe L0 series (same dump frame): luminance percentiles +
                // NaN/Inf count. Decaying p50 here while cards hold steady
                // localizes an energy drain to the probe update path.
                if (ddgi_irr_staging_ != handles::INVALID_RESOURCE && dump_probe_count_ > 0) {
                    if (auto* irr = static_cast<const float*>(
                            device_->MapBuffer(ddgi_irr_staging_))) {
                        std::vector<float> l0;
                        std::vector<float> l1;
                        l0.reserve(dump_probe_count_);
                        l1.reserve(dump_probe_count_);
                        u32 badCount = 0;
                        for (u32 p = 0; p < dump_probe_count_; ++p) {
                            const float* c0 = irr + (size_t)p * 36;
                            bool bad = false;
                            for (int k = 0; k < 3; ++k)
                                if (std::isnan(c0[k]) || std::isinf(c0[k])) bad = true;
                            if (bad) { ++badCount; continue; }
                            l0.push_back(0.2126f * c0[0] + 0.7152f * c0[1] +
                                         0.0722f * c0[2]);
                            // L1 方向能量（band1-3 幅度均值）——场角向特征标定量。
                            float l1m = 0.0f;
                            for (int b = 1; b <= 3; ++b) {
                                const float* bb = irr + (size_t)p * 36 + b * 4;
                                l1m += std::sqrt(bb[0]*bb[0] + bb[1]*bb[1] + bb[2]*bb[2]);
                            }
                            l1.push_back(l1m / 3.0f);
                        }
                        if (!l0.empty()) {
                            std::sort(l0.begin(), l0.end());
                            std::sort(l1.begin(), l1.end());
                            auto pct1 = [&](float q) {
                                return l1[std::min(l1.size() - 1,
                                                   (size_t)(q * (l1.size() - 1)))];
                            };
                            auto pct = [&](float q) {
                                return l0[std::min(l0.size() - 1,
                                                   (size_t)(q * (l0.size() - 1)))];
                            };
                            PRIMAL_LOG_STREAM(Error, "Render") << "  probes: n=" << l0.size()
                                      << " nan=" << badCount
                                      << " L0lum p10=" << pct(0.10f)
                                      << " p50=" << pct(0.50f)
                                      << " p90=" << pct(0.90f)
                                      << " max=" << l0.back()
                                      << " | L1mag p10=" << pct1(0.10f)
                                      << " p50=" << pct1(0.50f)
                                      << " p90=" << pct1(0.90f) << std::endl;

                            // Full-SH evaluation at the 6 axis normals: a
                            // healthy L0 with oversized L1/L2 can still clamp
                            // to zero after eval (this is what gather/final
                            // pixels actually receive). Buffer layout: 9
                            // consecutive vec3 bands per probe (36 floats).
                            static const math::v3 axes[6] = {
                                {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                            std::vector<float> evals;
                            evals.reserve(l0.size());
                            for (u32 p = 0; p < dump_probe_count_; ++p) {
                                const float* sh = irr + (size_t)p * 36;
                                bool bad = false;
                                for (int k = 0; k < 27 && !bad; ++k)
                                    if (std::isnan(sh[k]) || std::isinf(sh[k])) bad = true;
                                if (bad) continue;
                                auto band = [&](int i) -> math::v3 {
                                    return {sh[i * 3], sh[i * 3 + 1], sh[i * 3 + 2]};
                                };
                                float worst = 1e30f;
                                float best = -1e30f;
                                for (int a = 0; a < 6; ++a) {
                                    const math::v3 N = axes[a];
                                    math::v3 e = band(0) * 0.282095f
                                        + band(1) * (-0.488603f * N.y)
                                        + band(2) * (0.488603f * N.z)
                                        + band(3) * (-0.488603f * N.x)
                                        + band(4) * (1.092548f * N.y * N.x)
                                        + band(5) * (-1.092548f * N.y * N.z)
                                        + band(6) * (0.315392f * (3.0f * N.z * N.z - 1.0f))
                                        + band(7) * (-1.092548f * N.x * N.z)
                                        + band(8) * (0.546274f * (N.x * N.x - N.y * N.y));
                                    worst = std::min(worst, 0.2126f * e.x +
                                                     0.7152f * e.y + 0.0722f * e.z);
                                    best = std::max(best, 0.2126f * e.x +
                                                    0.7152f * e.y + 0.0722f * e.z);
                                }
                                evals.push_back(worst);
                            }
                            if (!evals.empty()) {
                                std::sort(evals.begin(), evals.end());
                                size_t mid = evals.size() / 2;
                                PRIMAL_LOG_STREAM(Error, "Render") << "  SHeval(worst axis) p10="
                                          << evals[evals.size() / 10]
                                          << " p50=" << evals[mid]
                                          << " p90=" << evals[evals.size() - 1 - evals.size() / 10]
                                          << std::endl;
                                // TEMP: max OVER axes (positive lobe ceiling) —
                                // huge values here = spikes gather feeds pixels.
                                float maxOverAll = -1e30f;
                                for (float v : evals) maxOverAll = std::max(maxOverAll, v);
                                PRIMAL_LOG_STREAM(Error, "Render") << "  SHeval(axis-max top) p50="
                                          << evals[mid] << " max=" << maxOverAll
                                          << std::endl;
                            }
                        } else {
                            PRIMAL_LOG_STREAM(Error, "Render") << "  probes: ALL " << badCount << " NaN/Inf" << std::endl;
                        }
                        device_->UnmapBuffer(ddgi_irr_staging_);
                    }

                    // TEMP diagnosis: SC shadow visibility samples (was the
                    // SC direct light unshadowed on Metal?).
                    {
                        const u32 asz = sc->GetAtlasSize();
                        if (asz > 0) {
                            u32 idx = (asz / 2) * asz + (asz / 2);
                            PRIMAL_LOG_STREAM(Error, "Render") << "  scShadowVis: pipelineReady="
                                      << sc->DebugShadowPipelineReady()
                                      << " center=" << sc->DebugShadowVisibilityAt(idx)
                                      << " texel0=" << sc->DebugShadowVisibilityAt(0)
                                      << " last=" << sc->DebugShadowVisibilityAt(asz * asz - 1)
                                      << std::endl;
                        }
                    }

                    // TEMP diagnosis: direct map of the (Dynamic/shared) ray
                    // buffer — hit/miss split + radiance & distance percentiles.
                    if (ddgi->GetRayDataBuffer() != handles::INVALID_RESOURCE) {
                        if (auto* rays = static_cast<const float*>(
                                device_->MapBuffer(ddgi->GetRayDataBuffer()))) {
                            const u64 rpp = 64u;
                            const u64 nRays = (u64)dump_probe_count_ * rpp;
                            u64 miss = 0, hit = 0, nanCount = 0;
                            std::vector<float> hitLum, hitDist;
                            std::vector<float> d0v, d1v;
                            for (u64 r = 0; r < nRays; ++r) {
                                const float* e = rays + r * 4;
                                if (std::isnan(e[0]) || std::isnan(e[1]) ||
                                    std::isnan(e[2]) || std::isnan(e[3])) { ++nanCount; continue; }
                                const u64 probe = r / rpp, lane = r % rpp;
                                if (lane == 0 && e[3] > -900.0f) { d0v.push_back(e[3]); continue; }
                                if (lane == 1 && e[3] > -900.0f) { d1v.push_back(e[3]); continue; }
                                if (lane <= 1u) continue;
                                if (e[3] < 0.0f) { ++miss; continue; }
                                ++hit;
                                hitLum.push_back(0.2126f*e[0] + 0.7152f*e[1] + 0.0722f*e[2]);
                                hitDist.push_back(e[3]);
                            }
                            auto qfn2 = [](std::vector<float>& v, float f) {
                                if (v.empty()) return 0.0f;
                                std::sort(v.begin(), v.end());
                                return v[std::min(v.size()-1, (size_t)(f*(v.size()-1)))];
                            };
                            if (!d0v.empty())
                                PRIMAL_LOG_STREAM(Error, "Render") << "  sdf@origin(d0): n=" << d0v.size()
                                          << " p10=" << qfn2(d0v,0.1f) << " p50=" << qfn2(d0v,0.5f)
                                          << " p90=" << qfn2(d0v,0.9f)
                                          << " | sdf@relocated(d1): p50=" << qfn2(d1v,0.5f)
                                          << " p90=" << qfn2(d1v,0.9f) << std::endl;
                            auto qfn = [](std::vector<float>& v, float f) {
                                if (v.empty()) return 0.0f;
                                std::sort(v.begin(), v.end());
                                return v[std::min(v.size()-1, (size_t)(f*(v.size()-1)))];
                            };
                            PRIMAL_LOG_STREAM(Error, "Render") << "  rays: n=" << nRays << " miss=" << miss
                                      << " hit=" << hit << " nan=" << nanCount
                                      << " hitLum p10=" << qfn(hitLum,0.1f)
                                      << " p50=" << qfn(hitLum,0.5f)
                                      << " p90=" << qfn(hitLum,0.9f)
                                      << " | hitDist p10=" << qfn(hitDist,0.1f)
                                      << " p50=" << qfn(hitDist,0.5f)
                                      << " p90=" << qfn(hitDist,0.9f) << std::endl;
                            device_->UnmapBuffer(ddgi->GetRayDataBuffer());
                        }
                    }
                }
                dump_pending_frame_ = 0xFFFFFFFFu;
            }

            // Slot indices follow the render frame count — the same base the
            // DDGI pass and SurfaceCache LightEval used to pick their triple-
            // buffer slots. current_buffer_index only rotates descriptor sets.
            const u32 frameIdx = inputs.frame_index % 3;
            const u32 histIdx = (inputs.frame_index + 2) % 3;

            // --- Sub-pass 1: CardRadianceAvg ---
            {
                DescriptorData params[4];
                if (device_->GetPlatform() == RHIPlatform::Vulkan) {
                    DescriptorData p[] = {
                        {0, DescriptorType::SampledImage,  sc->GetLightingAtlas(frameIdx)},
                        {1, DescriptorType::UniformBuffer, sc_params_cb_},
                        {2, DescriptorType::StorageBuffer, sc->GetCardDataBuffer()},
                        {3, DescriptorType::StorageBuffer, card_radiance_buf_},
                    };
                    memcpy(params, p, sizeof(p));
                } else {
                    DescriptorData p[] = {
                        {0, DescriptorType::UniformBuffer, sc_params_cb_},
                        {1, DescriptorType::StorageBuffer, sc->GetCardDataBuffer()},
                        {2, DescriptorType::StorageBuffer, card_radiance_buf_},
                        {0, DescriptorType::SampledImage,  sc->GetLightingAtlas(frameIdx)},
                    };
                    memcpy(params, p, sizeof(p));
                }
                UpdateDescriptorSet(device_, card_radiance_ds_[frameIdx], params, 4);

                cmd->BindComputePipeline(card_radiance_pipeline_);
                const DescriptorSetHandle sets[] = { card_radiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, card_radiance_layout_, 0, 1, sets, 0, nullptr);
                cmd->Dispatch((cardCount + 63) / 64, 1, 1);
            }

            // Barrier: card_radiance write → read
            {
                ResourceBarrier barrier{};
                barrier.resource = card_radiance_buf_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // SC_DUMP_CARDS=1 — every 90 frames copy the per-card radiance
            // into staging for the deferred print above (answers "which cards
            // actually carry light" without a debugger).
            static const bool dumpCards = [] {
                const char* e = std::getenv("SC_DUMP_CARDS");
                return e && atoi(e) != 0;
            }();
            u32 dumpInterval = 90u;
            if (const char* iv = std::getenv("SC_DUMP_INTERVAL"))
                dumpInterval = (u32)std::max(1, atoi(iv));
            if (dumpCards && inputs.frame_index % dumpInterval == 0u &&
                dump_pending_frame_ == 0xFFFFFFFFu) {
                if (card_radiance_staging_ == handles::INVALID_RESOURCE) {
                    BufferDesc d{};
                    d.size = (u64)max_cards_ * 16u;
                    d.usage = GPUMemoryUsage::Readback;
                    d.memoryUsage = GPUMemoryUsage::Readback;
                    card_radiance_staging_ = device_->CreateBuffer(d);
                }
                if (card_radiance_staging_ != handles::INVALID_RESOURCE) {
                    cmd->CopyBuffer(card_radiance_buf_, card_radiance_staging_,
                                    0, 0, (u64)cardCount * 16u);
                    dump_pending_frame_ = inputs.frame_index;
                    dump_card_count_ = cardCount;
                }
            }

            // --- Sub-pass 2: ProbeIrradianceFromCards ---
            // TEMP diagnosis: SC_INJECT_OFF=1 mutes the injection to bisect
            // the two-state oscillation (probes then carry only the DDGI
            // trace/update result).
            const u32 totalProbes = ddgi->GetVolumeData().ProbeCountTotal;
            if (std::getenv("SC_INJECT_OFF")) {
                // Still service the staging dump so the numeric series stays
                // comparable (copy current probe state).
                if (dumpCards && dump_pending_frame_ == inputs.frame_index &&
                    ddgi_irr_staging_ != handles::INVALID_RESOURCE) {
                    cmd->CopyBuffer(ddgi->GetIrradianceBuffer(frameIdx),
                                    ddgi_irr_staging_, 0, 0,
                                    (u64)totalProbes * 9u * 16u);
                }
            } else
            {
                const auto& ddgiVol = ddgi->GetVolumeData();
                auto* vol = static_cast<lumen::DDGIVolumeData*>(device_->MapBuffer(ddgi_vol_cb_[frameIdx]));
                if (vol) {
                    *vol = ddgiVol;
                    device_->UnmapBuffer(ddgi_vol_cb_[frameIdx]);
                }

                DescriptorData params[6] = {
                    {0, DescriptorType::UniformBuffer, ddgi_vol_cb_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_contrib_range_buf_},
                    {2, DescriptorType::StorageBuffer, flat_contrib_buf_},
                    {3, DescriptorType::StorageBuffer, card_radiance_buf_},
                    {4, DescriptorType::StorageBuffer, ddgi->GetIrradianceBuffer(histIdx)},
                    {5, DescriptorType::StorageBuffer, ddgi->GetIrradianceBuffer(frameIdx)},
                };
                UpdateDescriptorSet(device_, probe_irradiance_ds_[frameIdx], params, 6);

                cmd->BindComputePipeline(probe_irradiance_pipeline_);
                const DescriptorSetHandle sets[] = { probe_irradiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, probe_irradiance_layout_, 0, 1, sets, 0, nullptr);

                // SC-first: every probe collects from the atlas every frame
                // (≤ SC_MAX_CONTRIBS_PER_PROBE card reads each); the SDF trace
                // fills in where SC coverage is thin instead of vice versa.
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);

                // Same debug dump frame: copy the just-updated probe L0s so
                // the print can track card vs probe energy separately.
                if (dumpCards && dump_pending_frame_ == inputs.frame_index) {
                    if (ddgi_irr_staging_ == handles::INVALID_RESOURCE) {
                        BufferDesc d{};
                        d.size = (u64)totalProbes * 9u * 16u;
                        d.usage = GPUMemoryUsage::Readback;
                        d.memoryUsage = GPUMemoryUsage::Readback;
                        ddgi_irr_staging_ = device_->CreateBuffer(d);
                    }
                    if (ddgi_irr_staging_ != handles::INVALID_RESOURCE) {
                        cmd->CopyBuffer(ddgi->GetIrradianceBuffer(frameIdx),
                                        ddgi_irr_staging_, 0, 0,
                                        (u64)totalProbes * 9u * 16u);
                        dump_probe_count_ = totalProbes;
                    }
                }
            }

            // Metal publishes one canonical probe field after SC injection.
            //
            // The renderer rotates three physical DDGI buffers.  Leaving only
            // frameIdx updated allows each slot to retain a different temporal
            // trajectory (especially after sparse updates or a frame hitch), so
            // Gather displays three distinct brightness levels in an A/B/C
            // cycle.  Vulkan does not exhibit the issue; keep its established
            // path unchanged and mirror the completed Metal result into the two
            // idle slots.  The next frame therefore starts from the exact prior
            // result regardless of which physical slot it selects.
            if (device_->GetPlatform() == RHIPlatform::Metal &&
                !std::getenv("SC_MIRROR_OFF")) {
                const u64 irradianceBytes = (u64)totalProbes * 9u * 16u;
                cmd->MemoryBarrier(
                    PipelineStage::ComputeShader, PipelineStage::Transfer,
                    AccessFlag::ShaderWrite, AccessFlag::TransferRead);
                for (u32 slot = 0; slot < 3; ++slot) {
                    if (slot == frameIdx) continue;
                    cmd->CopyBuffer(ddgi->GetIrradianceBuffer(frameIdx),
                                    ddgi->GetIrradianceBuffer(slot),
                                    0, 0, irradianceBytes);
                }
                cmd->MemoryBarrier(
                    PipelineStage::Transfer, PipelineStage::ComputeShader,
                    AccessFlag::TransferWrite, AccessFlag::ShaderRead);
            }
        }
    );
}

} // namespace primal::graphics
