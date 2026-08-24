#include "SCDDGIIntegrationModule.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCacheTypes.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

#include <cmath>
#include <iostream>
#include <vector>

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
//   ddgi_card_radiance_avg:          buffer(0) SurfaceCacheParams, buffer(1) cards,
//                                    buffer(2) card_radiance out, texture(0) atlas
//   ddgi_probe_irradiance_from_cards: buffer(0) DDGIVolumeData, buffer(1) ranges,
//                                    buffer(2) contribs, buffer(3) card_radiance,
//                                    buffer(4) irradiance hist, buffer(5) irradiance out,
//                                    buffer(6) probe_update_list, buffer(7) confidence
// ============================================================================

bool SCDDGIIntegrationModule::Initialize(RHIDeviceBase* device,
                                           ShaderHandle card_radiance_shader,
                                           ShaderHandle probe_irradiance_shader,
                                           u32 render_width, u32 render_height) {
    device_ = device;
    (void)render_width;
    (void)render_height;

    if (card_radiance_shader != handles::INVALID_SHADER) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // SurfaceCacheParams
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_radiance (out)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // lighting atlas
        };
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
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe_update_list
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // confidence
        };
        probe_irradiance_set_layout_ = device->CreateDescriptorSetLayout({8, bindings});
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
    const float maxDist = probeSpacing * 3.0f;

    struct ProbeBuilder {
        SCProbeCardContrib entries[SC_MAX_CONTRIBS_PER_PROBE];
        float weights[SC_MAX_CONTRIBS_PER_PROBE];
        u32 count = 0;
    };
    std::vector<ProbeBuilder> builders(total_probes_);

    constexpr float PI = 3.14159265358979f;
    constexpr float FOUR_PI = 4.0f * PI;
    constexpr float SH_C0 = 0.282095f;
    constexpr float SH_C1 = 0.488603f;

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
            const float totalWeight = solidAngle * NdotL * FOUR_PI;

            const float basis[4] = {
                SH_C0,
                -SH_C1 * dirY,
                 SH_C1 * dirZ,
                -SH_C1 * dirX};

            SCProbeCardContrib contrib;
            contrib.card_index = c;
            for (int i = 0; i < 4; ++i) contrib.sh_weights[i] = totalWeight * basis[i];

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

    assignment_built_ = true;
    std::cout << "[SC→DDGI] Card→Probe assignment built: " << cardCount << " cards, "
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

            const u32 frameIdx = inputs.current_buffer_index % 3;
            const u32 histIdx = (inputs.current_buffer_index + 2) % 3;

            // --- Sub-pass 1: CardRadianceAvg ---
            {
                DescriptorData params[4] = {
                    {0, DescriptorType::UniformBuffer, sc_params_cb_},
                    {1, DescriptorType::StorageBuffer, sc->GetCardDataBuffer()},
                    {2, DescriptorType::StorageBuffer, card_radiance_buf_},
                    {0, DescriptorType::SampledImage,  sc->GetLightingAtlas(frameIdx)},
                };
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

            // --- Sub-pass 2: ProbeIrradianceFromCards ---
            {
                const auto& ddgiVol = ddgi->GetVolumeData();
                auto* vol = static_cast<lumen::DDGIVolumeData*>(device_->MapBuffer(ddgi_vol_cb_[frameIdx]));
                if (vol) {
                    *vol = ddgiVol;
                    device_->UnmapBuffer(ddgi_vol_cb_[frameIdx]);
                }

                DescriptorData params[8] = {
                    {0, DescriptorType::UniformBuffer, ddgi_vol_cb_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_contrib_range_buf_},
                    {2, DescriptorType::StorageBuffer, flat_contrib_buf_},
                    {3, DescriptorType::StorageBuffer, card_radiance_buf_},
                    {4, DescriptorType::StorageBuffer, ddgi->GetIrradianceBuffer(histIdx)},
                    {5, DescriptorType::StorageBuffer, ddgi->GetIrradianceBuffer(frameIdx)},
                    {6, DescriptorType::StorageBuffer, ddgi->GetProbeUpdateListBuffer(frameIdx)},
                    {7, DescriptorType::StorageBuffer, ddgi->GetConfidenceBuffer(frameIdx)},
                };
                UpdateDescriptorSet(device_, probe_irradiance_ds_[frameIdx], params, 8);

                cmd->BindComputePipeline(probe_irradiance_pipeline_);
                const DescriptorSetHandle sets[] = { probe_irradiance_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, probe_irradiance_layout_, 0, 1, sets, 0, nullptr);

                const u32 updateCount = ddgiVol.ProbeUpdateCount;
                cmd->Dispatch((updateCount + 63) / 64, 1, 1);
            }
        }
    );
}

} // namespace primal::graphics
