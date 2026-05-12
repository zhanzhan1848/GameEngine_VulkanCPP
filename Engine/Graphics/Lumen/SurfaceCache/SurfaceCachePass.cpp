#include "SurfaceCachePass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <iostream>
#include <cstring>

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

namespace {

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct SurfaceCacheRGData {
    RGResourceHandle prev_frame_color;
};

} // anonymous namespace

// ============================================================================
// SurfaceCachePass Implementation
// ============================================================================

SurfaceCachePass::~SurfaceCachePass() {
    Shutdown();
}

bool SurfaceCachePass::Initialize(RHIDeviceBase* device, const LumenConfig& config) {
    if (initialized_) return true;

    device_ = device;
    config_ = config;
    atlas_size_ = config.surface_cache_atlas_size;
    page_size_ = config.surface_cache_page_size;

    // Initialize card generator
    card_generator_.Initialize(device, atlas_size_, page_size_, config.surface_cache_max_cards);

    // Create atlas textures
    CreateAtlasTextures();

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create descriptor set layouts (stub)
    CreateDescriptorSetLayouts();

    // Create pipelines (stub)
    CreatePipelines();

    initialized_ = true;

    std::cout << "[SurfaceCache] Initialized (atlas: "
              << atlas_size_ << "x" << atlas_size_
              << ", page: " << page_size_
              << ", max_cards: " << config.surface_cache_max_cards << ")" << std::endl;
    return true;
}

void SurfaceCachePass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    device_ = nullptr;

    // No explicit GPU resource destruction needed -- handles are POD types
    // managed by the RHI device's garbage collector.
}

// ============================================================================
// Private helper methods
// ============================================================================

void SurfaceCachePass::CreateAtlasTextures() {
    auto CreateAtlas = [&](const char* name, DataFormat format, ResourceHandle& handle, bool renderTarget = false) {
        TextureDesc desc{};
        desc.size = { atlas_size_, atlas_size_, 1 };
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = format;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        if (renderTarget) desc.usage = desc.usage | TextureUsage::RenderTarget;
        desc.name = name;
        handle = device_->CreateTexture(desc);
    };

    // Albedo: RGBA8 (albedo + alpha) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Albedo", DataFormat::RGBA8_UNorm, albedo_atlas_, true);

    // Normal: RG16F (octahedral-encoded normal) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Normal", DataFormat::RG16_Float, normal_atlas_, true);

    // Depth: R32F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Depth", DataFormat::R32_Float, depth_atlas_, true);

    // Emissive: RGBA16F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Emissive", DataFormat::RGBA16_Float, emissive_atlas_, true);

    // Lighting atlas: triple-buffered (RGBA16F for HDR lighting)
    for (int i = 0; i < 3; ++i) {
        CreateAtlas(
            ("SurfaceCache_Lighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            lighting_atlas_[i]);
        CreateAtlas(
            ("SurfaceCache_PrevLighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            prev_lighting_atlas_[i]);
    }

    // Runtime buffers
    {
        // Light assignment buffer: per-tile light index lists
        u32 pagesPerSide = atlas_size_ / page_size_;
        u32 totalPages = pagesPerSide * pagesPerSide;
        BufferDesc lightAssignDesc{};
        lightAssignDesc.size = (u64)totalPages * config_.surface_cache_max_cards * sizeof(u32);
        lightAssignDesc.type = BufferType::Structured;
        lightAssignDesc.usage = GPUMemoryUsage::Dynamic;
        lightAssignDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        light_assignment_buffer_ = device_->CreateBuffer(lightAssignDesc);
    }

    {
        // Light info buffer: light parameters for evaluation
        BufferDesc lightInfoDesc{};
        lightInfoDesc.size = 256 * sizeof(u32) * 4;  // Max 256 lights, 64 bytes each
        lightInfoDesc.type = BufferType::Structured;
        lightInfoDesc.usage = GPUMemoryUsage::Dynamic;
        lightInfoDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        light_info_buffer_ = device_->CreateBuffer(lightInfoDesc);
    }

    {
        // Ray hits buffer: for indirect tracing
        u32 pagesPerSide = atlas_size_ / page_size_;
        u32 totalPages = pagesPerSide * pagesPerSide;
        BufferDesc rayHitsDesc{};
        rayHitsDesc.size = (u64)totalPages * page_size_ * page_size_ * sizeof(float) * 4; // RGBA per texel
        rayHitsDesc.type = BufferType::Structured;
        rayHitsDesc.usage = GPUMemoryUsage::Dynamic;
        rayHitsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ray_hits_buffer_ = device_->CreateBuffer(rayHitsDesc);
    }
}

void SurfaceCachePass::CreateConstantBuffers() {
    auto CreateCBs = [&](ResourceHandle (&cbs)[3], u64 size) {
        for (int i = 0; i < 3; i++) {
            BufferDesc desc{};
            desc.size = size;
            desc.type = BufferType::Constant;
            desc.usage = GPUMemoryUsage::Dynamic;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            cbs[i] = device_->CreateBuffer(desc);
        }
    };

    CreateCBs(global_cb_, 512);    // GlobalShaderData (padded)
    CreateCBs(params_cb_, 512);    // SurfaceCacheParams (padded)
}

void SurfaceCachePass::CreateDescriptorSetLayouts() {
    // Stub — descriptor set layouts will be created when pipelines are implemented
    // in later tasks. For now, handles remain INVALID.
}

void SurfaceCachePass::CreatePipelines() {
    // Stub — pipelines will be created when shaders are implemented
    // in later tasks. For now, handles remain INVALID.
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

SurfaceCacheOutput SurfaceCachePass::AddPass(
    RenderGraph& graph,
    RGResourceHandle prev_frame_color,
    ResourceHandle light_data_buffer,
    const SurfaceCacheFrameData& frame_data,
    u32 current_frame_index)
{
    SurfaceCacheOutput output{};

    u32 outIdx = current_frame_index % 3;

    // Import persistent lighting atlas into render graph
    auto lightingHandle = graph.ImportResource(
        "SurfaceCache_Lighting_" + std::to_string(outIdx),
        lighting_atlas_[outIdx]);

    output.lighting_atlas = lightingHandle;

    graph.AddPass<SurfaceCacheRGData>("LumenSurfaceCache",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, lightingHandle](
            SurfaceCacheRGData& data, RenderGraphBuilder& builder) {
            // Read previous frame color
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;

            // Write to lighting atlas
            builder.Write(lightingHandle, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: dispatch sub-passes (stub for now)
        // ====================================================================
        [this, frame_data, current_frame_index, outIdx, lightingHandle](
            const SurfaceCacheRGData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            // TODO: Sub-pass dispatch will be added in later tasks:
            // 1. CardCapture — rasterize mesh views into atlas
            // 2. DepthDilate — dilate depth atlas to fill holes
            // 3. LightCull — assign lights to atlas tiles
            // 4. LightEval — evaluate direct lighting per texel
            // 5. IndirectTrace — trace rays for indirect lighting
            // 6. IndirectResolve — resolve indirect lighting with temporal filtering
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
