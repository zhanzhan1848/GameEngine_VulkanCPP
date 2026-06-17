// RenderTargetAPI.cpp - Offscreen render target creation/destruction via RenderTexture.
//
// Format enum:
//   RTFormat_RGBA8   = 0
//   RTFormat_RGBA16F = 1
//   RTFormat_R8      = 2
//   RTFormat_Depth24 = 3
//
// Returns 1-based handle (0 = invalid). Uses RenderTexture::Create internally.
#include "Common.h"
#include "CommonHeaders.h"
#include "../Graphics/RHI/Core/RHIDevice.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/RenderTexture.h"

#include <cstdio>
#include <vector>

using namespace primal;

namespace {

// RenderTexture requires an entity_id for its registry. We use a synthetic
// monotonic counter since these are engine-internal offscreen targets.
u32 g_rt_entity_counter{ 0x80000000 }; // high bit set to avoid collision

struct RenderTargetEntry {
    graphics::RenderTexture* texture;
    u32 entity_id;
};

std::vector<RenderTargetEntry> g_render_targets;

} // anonymous namespace

EDITOR_INTERFACE u64 CreateRenderTarget(u32 width, u32 height, u32 format)
{
    if (width == 0 || height == 0) {
        std::fprintf(stderr, "[CreateRenderTarget] invalid dimensions %ux%u\n", width, height);
        return 0;
    }

    auto* device = graphics::get_rhi_device();
    if (!device) {
        std::fprintf(stderr, "[CreateRenderTarget] RHI device not initialized\n");
        return 0;
    }

    // Map format enum to RHI DataFormat
    graphics::rhi::DataFormat rhiFormat;
    switch (format) {
        case 0: rhiFormat = graphics::rhi::DataFormat::RGBA8_UNorm; break;
        case 1: rhiFormat = graphics::rhi::DataFormat::RGBA16_Float; break;
        case 2: rhiFormat = graphics::rhi::DataFormat::R8_UNorm; break;
        case 3: rhiFormat = graphics::rhi::DataFormat::D24_UNorm_S8_UInt; break;
        default:
            std::fprintf(stderr, "[CreateRenderTarget] unknown format %u\n", format);
            return 0;
    }

    u32 entity_id = g_rt_entity_counter++;

    auto* rt = new graphics::RenderTexture();
    graphics::rhi::TextureDesc desc{};
    desc.size = { width, height, 1 };
    desc.format = rhiFormat;
    desc.usage = graphics::rhi::TextureUsage::RenderTarget | graphics::rhi::TextureUsage::ShaderResource;
    desc.mipLevels = 1;
    desc.arraySize = 1;

    if (!rt->Create(device, entity_id, desc)) {
        std::fprintf(stderr, "[CreateRenderTarget] RenderTexture::Create failed\n");
        delete rt;
        return 0;
    }

    g_render_targets.push_back({ rt, entity_id });
    return static_cast<u64>(g_render_targets.size()); // 1-based handle
}

EDITOR_INTERFACE void DestroyRenderTarget(u64 handle)
{
    if (handle == 0 || handle > g_render_targets.size()) return;
    auto& entry = g_render_targets[handle - 1];
    if (!entry.texture) return;

    auto* device = graphics::get_rhi_device();
    if (device) {
        entry.texture->Destroy(device);
    }
    delete entry.texture;
    entry.texture = nullptr;
    entry.entity_id = id::invalid_id;
}
