#include "MaterialDataBuilder.h"
#include "../MaterialInstance.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#ifdef __APPLE__
#include "../RHI/Platforms/Metal/MetalDevice.h"
#if defined(__APPLE__)
#include "../RHI/Platforms/Metal/MetalTexture.h"
#endif
#endif
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "../RHI/Platforms/Dawn/DawnDevice.h"
#include "../RHI/Platforms/Dawn/DawnTexture.h"
#endif
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "../RHI/Platforms/Vulkan/VulkanDevice.h"
#include "../RHI/Platforms/Vulkan/VulkanTexture.h"
#include "../RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif
#include "../../Content/ContentToEngine.h"
#include "../../Utilities/IOStream.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <functional>
#include <cstdlib>
#include <cstring>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::nanite {

GPUMaterialRegistry::MaterialData MaterialDataBuilder::ExtractMaterialData(
    graphics::MaterialInstance* instance
) {
    GPUMaterialRegistry::MaterialData data{};

    if (!instance) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Null material instance" << std::endl;
        return data;
    }

    // Use default indices (will be updated later when texture mapping is available)
    data.albedo_texture_idx = 0;
    data.normal_texture_idx = 0;
    data.orm_texture_idx = 0;

    // Get material factors
    math::v3 tint;
    float metallic, roughness;
    instance->GetMaterialFactors(tint, metallic, roughness);

    data.albedo_tint[0] = tint.x;
    data.albedo_tint[1] = tint.y;
    data.albedo_tint[2] = tint.z;
    data.metallic_factor = metallic;
    data.roughness_factor = roughness;
    data.normal_scale = 1.0f;
    data.uv_scale[0] = 1.0f;
    data.uv_scale[1] = 1.0f;
    data.flags = GPUMaterialRegistry::EncodeThicknessFlags(
        GPUMaterialRegistry::EncodeTransmissionFlags(
            0u, instance->GetTransmissionFactor()),
        instance->GetThicknessFactor());

    return data;
}

GPUMaterialRegistry::MaterialData MaterialDataBuilder::ExtractMaterialData(
    graphics::MaterialInstance* instance,
    const TextureArrayBuildContext& ctx
) {
    GPUMaterialRegistry::MaterialData data{};

    if (!instance) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Null material instance" << std::endl;
        return data;
    }

    // 🎨 NEW: Use texture mapping to get array indices.
    // Returns 0 (placeholder layer) when handle is missing or was filtered out
    // (e.g. 1x1 test-harness fallbacks that would mismatch the array's
    // 1024x1024 size and leave their layer uninitialized).
    rhi::ResourceHandle albedo = instance->GetTextureHandle(0);
    auto it = ctx.albedoToIndex.find(albedo);
    data.albedo_texture_idx = (it != ctx.albedoToIndex.end()) ? it->second : 0u;

    rhi::ResourceHandle normal = instance->GetTextureHandle(1);
    it = ctx.normalToIndex.find(normal);
    data.normal_texture_idx = (it != ctx.normalToIndex.end()) ? it->second : 0u;

    rhi::ResourceHandle orm = instance->GetTextureHandle(2);
    it = ctx.ormToIndex.find(orm);
    data.orm_texture_idx = (it != ctx.ormToIndex.end()) ? it->second : 0u;

    // Get material factors
    math::v3 tint;
    float metallic, roughness;
    instance->GetMaterialFactors(tint, metallic, roughness);

    data.albedo_tint[0] = tint.x;
    data.albedo_tint[1] = tint.y;
    data.albedo_tint[2] = tint.z;
    data.metallic_factor = metallic;
    data.roughness_factor = roughness;
    data.normal_scale = 1.0f;
    data.uv_scale[0] = 1.0f;  // 🔥 NEW: Default UV scaling (no repetition)
    data.uv_scale[1] = 1.0f;
    data.flags = GPUMaterialRegistry::EncodeThicknessFlags(
        GPUMaterialRegistry::EncodeTransmissionFlags(
            0u, instance->GetTransmissionFactor()),
        instance->GetThicknessFactor());

    return data;
}

bool MaterialDataBuilder::AreMaterialsCompatible(
    const graphics::MaterialInstance& a,
    const graphics::MaterialInstance& b,
    float tolerance
) {
    // Compare textures
    rhi::ResourceHandle a_albedo = a.GetTextureHandle(0);
    rhi::ResourceHandle b_albedo = b.GetTextureHandle(0);
    if (a_albedo != b_albedo) return false;

    rhi::ResourceHandle a_normal = a.GetTextureHandle(1);
    rhi::ResourceHandle b_normal = b.GetTextureHandle(1);
    if (a_normal != b_normal) return false;

    rhi::ResourceHandle a_orm = a.GetTextureHandle(2);
    rhi::ResourceHandle b_orm = b.GetTextureHandle(2);
    if (a_orm != b_orm) return false;

    // Compare material factors
    math::v3 a_tint, b_tint;
    float a_metallic, b_metallic;
    float a_roughness, b_roughness;

    a.GetMaterialFactors(a_tint, a_metallic, a_roughness);
    b.GetMaterialFactors(b_tint, b_metallic, b_roughness);

    // Check tint difference
    float tint_diff = std::sqrt(
        std::pow(a_tint.x - b_tint.x, 2) +
        std::pow(a_tint.y - b_tint.y, 2) +
        std::pow(a_tint.z - b_tint.z, 2)
    );
    if (tint_diff > tolerance) return false;

    // Check metallic difference
    if (std::abs(a_metallic - b_metallic) > tolerance) return false;

    // Check roughness difference
    if (std::abs(a_roughness - b_roughness) > tolerance) return false;

    // Thin-surface transport is part of the material identity.  Omitting it
    // here would merge an opaque and a cloth material that happen to share
    // textures and scalar PBR factors.
    if (std::abs(a.GetTransmissionFactor() - b.GetTransmissionFactor()) > tolerance)
        return false;
    if (std::abs(a.GetThicknessFactor() - b.GetThicknessFactor()) > tolerance)
        return false;

    return true;
}

bool MaterialDataBuilder::BuildTextureArrays(
    rhi::RHIDeviceBase* device,
    const std::vector<graphics::MaterialInstance*>& instances,
    rhi::ResourceHandle& outAlbedoArray,
    rhi::ResourceHandle& outNormalArray,
    rhi::ResourceHandle& outORMArray
) {
    if (instances.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: No material instances to build" << std::endl;
        return true;  // Not an error, just nothing to do
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Building texture arrays for "
              << instances.size() << " materials..." << std::endl;

    // 🎨 Phase 2: Real texture array implementation

    // 1. Build texture mapping (collect unique textures)
    TextureArrayBuildContext ctx;
    BuildTextureMapping(instances, ctx, device);

    // 2. Create texture arrays
    // TODO: Need command buffer for BlitTexture operations
    // For now, we'll create empty arrays (actual copying will be added in next iteration)

    bool success = true;

    if (!ctx.uniqueAlbedo.empty()) {
        // TODO: Pass actual command buffer
        if (!CreateTextureArray(device, nullptr, ctx.uniqueAlbedo, "AlbedoTextureArray", outAlbedoArray, TextureArrayChannel::Albedo)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Failed to create albedo texture array" << std::endl;
            success = false;
        }
    }

    if (!ctx.uniqueNormal.empty()) {
        if (!CreateTextureArray(device, nullptr, ctx.uniqueNormal, "NormalTextureArray", outNormalArray)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Failed to create normal texture array" << std::endl;
            success = false;
        }
    }

    if (!ctx.uniqueORM.empty()) {
        if (!CreateTextureArray(device, nullptr, ctx.uniqueORM, "ORMTextureArray", outORMArray)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Failed to create ORM texture array" << std::endl;
            success = false;
        }
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Texture array build complete (Phase 2: mapping created, copying TODO)" << std::endl;
    return success;
}

jobsystem::JobHandle MaterialDataBuilder::BuildAsync(
    rhi::RHIDeviceBase* device,
    const std::vector<graphics::MaterialInstance*>& instances,
    GPUMaterialRegistry* registry
) {
    // TODO: Implement async build
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] BuildAsync not yet implemented" << std::endl;
    return jobsystem::JobHandle();
}

// 🎨 NEW: Helper function to add texture to mapping with deduplication
namespace {
    void AddTextureToMapping(
        std::unordered_map<rhi::ResourceHandle, uint32_t>& textureToIndex,
        std::vector<rhi::ResourceHandle>& uniqueTextures,
        rhi::ResourceHandle texture
    ) {
        if (texture == rhi::handles::INVALID_RESOURCE) {
            return;  // Skip invalid textures
        }

        // Check if already exists
        auto it = textureToIndex.find(texture);
        if (it == textureToIndex.end()) {
            // New unique texture
            uint32_t idx = static_cast<uint32_t>(uniqueTextures.size());
            textureToIndex[texture] = idx;
            uniqueTextures.push_back(texture);
        }
        // If exists, we reuse the existing index (no action needed)
    }

    // 🎨 NEW: Get texture size — dispatch by RHI platform.
    // The Metal-only static_cast<MetalDevice*> path was UB on Dawn and caused
    // a libc++ mutex abort inside CreateTextureArray.
    math::u32v2 GetTextureSize(rhi::RHIDeviceBase* device, rhi::ResourceHandle texture) {
        using namespace rhi;

        if (!device || texture == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: Null device/texture in GetTextureSize, defaulting to 512x512" << std::endl;
            return {512, 512};
        }

        const RHIPlatform platform = device->GetPlatform();
#ifdef __APPLE__
        if (platform == RHIPlatform::Metal) {
            MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
            MetalTexture* metalTex = metalDevice->GetTexture(texture);
            if (metalTex && metalTex->GetNativeTexture()) {
                MTL::Texture* mtlTex = metalTex->GetNativeTexture();
                return {static_cast<u32>(mtlTex->width()), static_cast<u32>(mtlTex->height())};
            }
        }
#endif
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
        if (platform == RHIPlatform::Dawn) {
            DawnDevice* dawnDevice = static_cast<DawnDevice*>(device);
            DawnTexture* dawnTex = dawnDevice->GetTexture(texture);
            if (dawnTex && dawnTex->GetNativeTexture()) {
                // Use the native WGPUTexture accessors — the descriptor's size
                // is unreliable for content-loaded textures (defaults to 1x1).
                WGPUTexture native = dawnTex->GetNativeTexture();
                return {wgpuTextureGetWidth(native), wgpuTextureGetHeight(native)};
            }
        }
#endif
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        if (platform == RHIPlatform::Vulkan) {
            VulkanDevice* vkDevice = static_cast<VulkanDevice*>(device);
            VulkanTexture* vkTex = vkDevice->GetTexture(texture);
            if (vkTex) {
                const auto& desc = vkTex->GetTextureDesc();
                return {desc.size.x, desc.size.y};
            }
        }
#endif
        // D3D12 与 Vulkan 同形：走平台中立 RHI 查询（D3D12Device 已实现），
        // 避免落入 512x512 默认值警告档。
        if (platform == RHIPlatform::D3D12) {
            u32 w = 0, h = 0;
            if (device->GetTextureSize(texture, w, h)) {
                return {w, h};
            }
        }

        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: Could not get texture size, using default 512x512" << std::endl;
        return {512, 512};
    }

    // 🎨 NEW: Get texture format — dispatch by RHI platform.
    rhi::DataFormat GetTextureFormat(rhi::RHIDeviceBase* device, rhi::ResourceHandle texture) {
        using namespace rhi;

        if (!device || texture == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: Null device/texture in GetTextureFormat, defaulting to RGBA8_UNorm" << std::endl;
            return rhi::DataFormat::RGBA8_UNorm;
        }

        const RHIPlatform platform = device->GetPlatform();
#ifdef __APPLE__
        if (platform == RHIPlatform::Metal) {
            MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
            MetalTexture* metalTex = metalDevice->GetTexture(texture);
            if (metalTex && metalTex->GetNativeTexture()) {
                MTL::Texture* mtlTex = metalTex->GetNativeTexture();
                switch (mtlTex->pixelFormat()) {
                    case MTL::PixelFormatRGBA8Unorm:
                        return rhi::DataFormat::RGBA8_UNorm;
                    case MTL::PixelFormatRGBA8Unorm_sRGB:
                        return rhi::DataFormat::RGBA8_sRGB;
                    case MTL::PixelFormatBGRA8Unorm:
                    case MTL::PixelFormatBGRA8Unorm_sRGB:
                        return rhi::DataFormat::RGBA8_UNorm;
                    default:
                        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: Unknown Metal pixel format " << static_cast<u32>(mtlTex->pixelFormat()) << ", using RGBA8_UNorm" << std::endl;
                        return rhi::DataFormat::RGBA8_UNorm;
                }
            }
        }
#endif
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
        if (platform == RHIPlatform::Dawn) {
            DawnDevice* dawnDevice = static_cast<DawnDevice*>(device);
            DawnTexture* dawnTex = dawnDevice->GetTexture(texture);
            if (dawnTex) {
                return dawnTex->GetTextureDesc().format;
            }
        }
#endif
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        if (platform == RHIPlatform::Vulkan) {
            VulkanDevice* vkDevice = static_cast<VulkanDevice*>(device);
            VulkanTexture* vkTex = vkDevice->GetTexture(texture);
            if (vkTex) {
                return vkTex->GetTextureDesc().format;
            }
        }
#endif

        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: Could not get texture format, using RGBA8_UNorm" << std::endl;
        return rhi::DataFormat::RGBA8_UNorm;
    }
}

// 🎨 NEW: Create placeholder textures (1024x1024 default textures to match array size)
void MaterialDataBuilder::CreatePlaceholderTextures(
    TextureArrayBuildContext& ctx,
    rhi::RHIDeviceBase* device,
    rhi::DataFormat albedo_fmt,
    rhi::DataFormat normal_fmt,
    rhi::DataFormat orm_fmt
) {
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Creating 1024x1024 placeholder textures "
              << "(albedo fmt=" << static_cast<u32>(albedo_fmt)
              << ", normal fmt=" << static_cast<u32>(normal_fmt)
              << ", orm fmt=" << static_cast<u32>(orm_fmt) << ")..." << std::endl;

    // 🎨 CRITICAL: All textures in array must be same size
    // Sponza uses 1024x1024, so placeholders must match
    constexpr u32 TARGET_SIZE = 1024;
    constexpr u32 PIXEL_COUNT = TARGET_SIZE * TARGET_SIZE;
    constexpr u32 DATA_SIZE = PIXEL_COUNT * 4;  // RGBA

    // Helper to create texture from pixel data using content system
    auto CreateTextureFromPixels = [device](const std::vector<u32>& pixels, rhi::DataFormat format, const char* name) -> rhi::ResourceHandle {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MDB] CreateTextureFromPixels start: " << name << std::endl;
        // Build blob data (matches TestNaniteStreamingPipeline format)
        u32 row_pitch = TARGET_SIZE * 4;
        u32 slice_pitch = DATA_SIZE;
        u32 blob_size = (6 * sizeof(u32)) + (2 * sizeof(u32) + slice_pitch);

        std::vector<u8> blob(blob_size);
        primal::utl::blob_stream_writer writer(blob.data(), blob.size());

        writer.write((u32)TARGET_SIZE);  // width
        writer.write((u32)TARGET_SIZE);  // height
        writer.write((u32)1);            // array_size
        writer.write((u32)0);            // flags
        writer.write((u32)1);            // mip_levels
        // ContentToEngine::create_texture_resource expects DXGI format codes,
        // not rhi::DataFormat enum values. Map explicitly.
        u32 dxgiFormat = 28; // RGBA8_UNorm default
        if (format == rhi::DataFormat::RGBA8_sRGB) dxgiFormat = 29;
        else if (format == rhi::DataFormat::BC1_UNorm) dxgiFormat = 71;
        else if (format == rhi::DataFormat::BC1_sRGB) dxgiFormat = 72;
        else if (format == rhi::DataFormat::BC7_UNorm) dxgiFormat = 98;
        else if (format == rhi::DataFormat::BC7_sRGB) dxgiFormat = 99;
        writer.write(dxgiFormat);        // format (DXGI)
        writer.write(row_pitch);
        writer.write(slice_pitch);
        writer.write(reinterpret_cast<const char*>(pixels.data()), slice_pitch);

        PRIMAL_LOG_STREAM(Error, "Render") << "[MDB] Calling create_resource for " << name << std::endl;
        primal::id::id_type id = primal::content::create_resource(blob.data(), primal::content::asset_type::texture);
        PRIMAL_LOG_STREAM(Error, "Render") << "[MDB] create_resource returned: id=" << id << " for " << name << std::endl;
        if (primal::id::is_valid(id)) {
            auto handle = primal::content::get_rhi_texture_handle(id);
            PRIMAL_LOG_STREAM(Error, "Render") << "[MDB] RHI handle=" << handle << " for " << name << std::endl;
            return handle;
        }
        return rhi::handles::INVALID_RESOURCE;
    };

    // 1. Albedo placeholder (white)
    {
        std::vector<u32> whiteData(PIXEL_COUNT, 0xFFFFFFFF);  // RGBA = (1, 1, 1, 1)
        ctx.placeholderAlbedo = CreateTextureFromPixels(whiteData, albedo_fmt, "PlaceholderAlbedo");
    }

    // 2. Normal placeholder (flat normal = 0, 0, 1)
    {
        // Normal = (0, 0, 1) → RGB = (128, 128, 255) in [0,255] range
        // For ABGR format (Metal): A=255, B=255, G=128, R=128 → 0xFF8080FF
        std::vector<u32> normalData(PIXEL_COUNT, 0xFF8080FF);
        ctx.placeholderNormal = CreateTextureFromPixels(normalData, normal_fmt, "PlaceholderNormal");
    }

    // 3. ORM placeholder (AO=1, R=0.5, M=0)
    {
        // AO=1, R=0.5, M=0 → RGB = (255, 128, 0)
        // For ABGR format: A=255, B=0, G=128, R=255 → 0xFF0080FF
        std::vector<u32> ormData(PIXEL_COUNT, 0xFF0080FF);
        ctx.placeholderORM = CreateTextureFromPixels(ormData, orm_fmt, "PlaceholderORM");
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] 1024x1024 placeholder textures created" << std::endl;
}

// 🎨 NEW: Build texture mapping from material instances
void MaterialDataBuilder::BuildTextureMapping(
    const std::vector<graphics::MaterialInstance*>& instances,
    TextureArrayBuildContext& ctx,
    rhi::RHIDeviceBase* device
) {
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Building texture mapping..." << std::endl;

    // 1. Pre-scan source textures to find the canonical format per channel.
    //    WebGPU requires all layers of a texture_2d_array to share the exact
    //    format; the test loads albedo as RGBA8_UNorm but creates its own
    //    fallback as RGBA8_sRGB, so the "first source's format" heuristic
    //    gives the wrong answer. Picking the most common format and rejecting
    //    mismatches keeps every layer blittable.
    auto detect_canonical_format = [&](std::function<rhi::ResourceHandle(const graphics::MaterialInstance*)> getter,
                                       rhi::DataFormat placeholder_fmt) -> rhi::DataFormat {
        std::unordered_map<u32, u32> histogram;
        for (const auto* inst : instances) {
            if (!inst) continue;
            rhi::ResourceHandle tex = getter(inst);
            if (tex == rhi::handles::INVALID_RESOURCE) continue;
            math::u32v2 s = GetTextureSize(device, tex);
            if (s.x < 16 || s.y < 16) continue;  // skip 1x1 fallbacks
            u32 fmt = static_cast<u32>(GetTextureFormat(device, tex));
            histogram[fmt]++;
        }
        if (histogram.empty()) return placeholder_fmt;
        u32 best_fmt = 0, best_count = 0;
        for (const auto& [fmt, count] : histogram) {
            if (count > best_count) { best_count = count; best_fmt = fmt; }
        }
        return static_cast<rhi::DataFormat>(best_fmt);
    };
    auto get_albedo = [](const graphics::MaterialInstance* inst) { return inst->GetTextureHandle(0); };
    auto get_normal = [](const graphics::MaterialInstance* inst) { return inst->GetTextureHandle(1); };
    auto get_orm    = [](const graphics::MaterialInstance* inst) { return inst->GetTextureHandle(2); };
    const rhi::DataFormat canonical_albedo_fmt = detect_canonical_format(get_albedo, rhi::DataFormat::RGBA8_sRGB);
    const rhi::DataFormat canonical_normal_fmt = detect_canonical_format(get_normal, rhi::DataFormat::RGBA8_UNorm);
    const rhi::DataFormat canonical_orm_fmt    = detect_canonical_format(get_orm,    rhi::DataFormat::RGBA8_UNorm);
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Canonical formats: albedo=" << static_cast<u32>(canonical_albedo_fmt)
              << ", normal=" << static_cast<u32>(canonical_normal_fmt)
              << ", orm=" << static_cast<u32>(canonical_orm_fmt) << std::endl;

    // 1b. Create placeholder textures USING the canonical format. The placeholder
    //     becomes layer 0 of each array; if its format doesn't match the array's
    //     canonical format, BlitTexture will skip it, leaving layer 0
    //     uninitialized — and any material that falls back to placeholder
    //     (defaults to idx 0) would sample undefined memory.
    CreatePlaceholderTextures(ctx, device, canonical_albedo_fmt,
                              canonical_normal_fmt, canonical_orm_fmt);

    // 1c. Register placeholders as layer 0.
    ctx.albedoToIndex[ctx.placeholderAlbedo] = 0;
    ctx.uniqueAlbedo.push_back(ctx.placeholderAlbedo);
    ctx.normalToIndex[ctx.placeholderNormal] = 0;
    ctx.uniqueNormal.push_back(ctx.placeholderNormal);
    ctx.ormToIndex[ctx.placeholderORM] = 0;
    ctx.uniqueORM.push_back(ctx.placeholderORM);

    // 2. Collect unique textures from all material instances.
    // Skip textures whose size < 16x16 OR format != canonical — they'd be
    // rejected by BlitTexture anyway, leaving the assigned layer uninitialized.
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Processing " << instances.size() << " material instances..." << std::endl;
    u32 skipped_small_albedo = 0, skipped_small_normal = 0, skipped_small_orm = 0;
    u32 skipped_fmt_albedo = 0, skipped_fmt_normal = 0, skipped_fmt_orm = 0;

    for (size_t i = 0; i < instances.size(); ++i) {
        const auto* instance = instances[i];
        if (!instance) {
            PRIMAL_LOG_STREAM(Info, "Render") << "  [" << i << "] NULL instance, skipping" << std::endl;
            continue;
        }

        // 🐛 DEBUG: Get texture handles and print them
        rhi::ResourceHandle albedo = instance->GetTextureHandle(0);
        rhi::ResourceHandle normal = instance->GetTextureHandle(1);
        rhi::ResourceHandle orm = instance->GetTextureHandle(2);

        // 🔥 NEW DEBUG: Print texture index assignment
        uint32_t albedoIdx = 0, normalIdx = 0, ormIdx = 0;

        // Find current texture indices (before adding)
        auto albedoIt = ctx.albedoToIndex.find(albedo);
        if (albedoIt != ctx.albedoToIndex.end()) {
            albedoIdx = albedoIt->second;
        }
        auto normalIt = ctx.normalToIndex.find(normal);
        if (normalIt != ctx.normalToIndex.end()) {
            normalIdx = normalIt->second;
        }
        auto ormIt = ctx.ormToIndex.find(orm);
        if (ormIt != ctx.ormToIndex.end()) {
            ormIdx = ormIt->second;
        }

        // Debug output for ALL materials (critical!)
        PRIMAL_LOG_STREAM(Info, "Render") << "  [MaterialID=" << i << "] Texture indices BEFORE: "
                  << "albedo_idx=" << albedoIdx << ", "
                  << "normal_idx=" << normalIdx << ", "
                  << "orm_idx=" << ormIdx << std::endl;

        // Debug output for first few materials (texture handles)
        if (i < 5) {
            PRIMAL_LOG_STREAM(Info, "Render") << "    Texture handles: "
                      << "albedo=" << albedo << ", "
                      << "normal=" << normal << ", "
                      << "orm=" << orm;

            // Check if invalid
            if (albedo == rhi::handles::INVALID_RESOURCE &&
                normal == rhi::handles::INVALID_RESOURCE &&
                orm == rhi::handles::INVALID_RESOURCE) {
                PRIMAL_LOG_STREAM(Info, "Render") << " ❌ ALL INVALID";
            } else {
                PRIMAL_LOG_STREAM(Info, "Render") << " ✓";
            }
            PRIMAL_LOG_STREAM(Info, "Render") << std::endl;
        }

        // Add to mapping with deduplication. Skip textures that would be
        // rejected by BlitTexture so they don't get assigned a layer that
        // ends up uninitialized:
        //   - Size < 16x16 (test-harness 1x1 fallbacks)
        //   - Format != canonical format for this channel
        // Skipped textures fall back to layer 0 (placeholder) in
        // ExtractMaterialData.
        auto add_if_real = [&](rhi::ResourceHandle tex,
                               std::unordered_map<rhi::ResourceHandle, uint32_t>& map,
                               std::vector<rhi::ResourceHandle>& uniq,
                               u32& skip_small_count,
                               u32& skip_fmt_count,
                               rhi::DataFormat canonical_fmt) {
            if (tex == rhi::handles::INVALID_RESOURCE) return;
            if (map.find(tex) != map.end()) return;  // already mapped
            math::u32v2 sz = GetTextureSize(device, tex);
            if (sz.x < 16 || sz.y < 16) {
                ++skip_small_count;
                return;
            }
            rhi::DataFormat fmt = GetTextureFormat(device, tex);
            if (fmt != canonical_fmt) {
                ++skip_fmt_count;
                return;
            }
            uint32_t idx = static_cast<uint32_t>(uniq.size());
            map[tex] = idx;
            uniq.push_back(tex);
        };
        add_if_real(albedo, ctx.albedoToIndex, ctx.uniqueAlbedo,
                    skipped_small_albedo, skipped_fmt_albedo, canonical_albedo_fmt);
        add_if_real(normal, ctx.normalToIndex, ctx.uniqueNormal,
                    skipped_small_normal, skipped_fmt_normal, canonical_normal_fmt);
        add_if_real(orm, ctx.ormToIndex, ctx.uniqueORM,
                    skipped_small_orm, skipped_fmt_orm, canonical_orm_fmt);

        // 🔥 NEW DEBUG: Print texture indices AFTER adding
        // Uses find() so skipped (small) textures don't get auto-inserted with
        // index 0 by operator[].
        auto idx_or_dash = [](const std::unordered_map<rhi::ResourceHandle, uint32_t>& m,
                              rhi::ResourceHandle h) -> std::string {
            auto it = m.find(h);
            return (it != m.end()) ? std::to_string(it->second) : std::string("placeholder(0)");
        };
        PRIMAL_LOG_STREAM(Info, "Render") << "  [MaterialID=" << i << "] Texture indices AFTER: "
                  << "albedo_idx=" << idx_or_dash(ctx.albedoToIndex, albedo) << ", "
                  << "normal_idx=" << idx_or_dash(ctx.normalToIndex, normal) << ", "
                  << "orm_idx=" << idx_or_dash(ctx.ormToIndex, orm) << std::endl;
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Texture mapping complete. Total unique textures: "
              << ctx.uniqueAlbedo.size() << " albedo (skipped " << skipped_small_albedo
              << " small, " << skipped_fmt_albedo << " fmt), "
              << ctx.uniqueNormal.size() << " normal (skipped " << skipped_small_normal
              << " small, " << skipped_fmt_normal << " fmt), "
              << ctx.uniqueORM.size() << " ORM (skipped " << skipped_small_orm
              << " small, " << skipped_fmt_orm << " fmt)"
              << std::endl;

    // 🔥 CRITICAL DEBUG: Print final mapping for all materials
    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] FINAL TEXTURE INDEX MAPPING:" << std::endl;
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto* instance = instances[i];
        if (!instance) continue;

        rhi::ResourceHandle albedo = instance->GetTextureHandle(0);
        rhi::ResourceHandle normal = instance->GetTextureHandle(1);
        rhi::ResourceHandle orm = instance->GetTextureHandle(2);

        auto idx_or_default = [](const std::unordered_map<rhi::ResourceHandle, uint32_t>& m,
                                 rhi::ResourceHandle h) -> uint32_t {
            auto it = m.find(h);
            return (it != m.end()) ? it->second : 0u;  // 0 = placeholder
        };
        uint32_t albedoIdx = idx_or_default(ctx.albedoToIndex, albedo);
        uint32_t normalIdx = idx_or_default(ctx.normalToIndex, normal);
        uint32_t ormIdx = idx_or_default(ctx.ormToIndex, orm);

        PRIMAL_LOG_STREAM(Info, "Render") << "  MaterialID=" << i << " → "
                  << "albedo_layer=" << albedoIdx << ", "
                  << "normal_layer=" << normalIdx << ", "
                  << "orm_layer=" << ormIdx << std::endl;
    }
}

// 🎨 NEW: Create texture array using BlitTexture
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
namespace {
bool BuildCheckedVulkanAlbedoArray(rhi::RHIDeviceBase* device,rhi::RHICommandBuffer* provided,
    const std::vector<rhi::ResourceHandle>& sources,rhi::TextureDesc desc,rhi::ResourceHandle& output) {
    using namespace rhi;
    auto& vk=static_cast<VulkanDevice&>(*device);
    desc.mipLevels=1;for(u32 n=std::max(desc.size.x,desc.size.y);n>1;n>>=1)++desc.mipLevels;
    desc.usage=TextureUsage::ShaderResource|TextureUsage::CopySource|TextureUsage::CopyDest;
    // Reject every unsupported source before recording any candidate work.
    // A skipped layer would be undefined data at every generated mip.
    for(size_t i=0;i<sources.size();++i) {
        auto* source=vk.GetTexture(sources[i]);
        if(!source||!source->OwnsImage()||!source->GetNativeImage())return false;
        const auto& sd=source->GetTextureDesc();
        const bool same=sd.size.x==desc.size.x&&sd.size.y==desc.size.y;
        const bool placeholder=i==0&&sd.size.x==1&&sd.size.y==1;
        if(sd.type!=TextureType::Texture2D||sd.size.z!=1||sd.format!=desc.format||
           (!same&&!placeholder)||source->GetMipLayout(0)==VK_IMAGE_LAYOUT_UNDEFINED)return false;
    }
    CommandBufferHandle owned=handles::INVALID_COMMAND_BUFFER;
    auto* cmd=provided;
    if(!cmd) {
        owned=device->CreateCommandBuffer(CommandQueueType::Graphics);
        cmd=GetCommandBuffer(owned);
        if(!cmd||!cmd->Begin()){if(owned!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(owned);return false;}
    }
    auto& native=static_cast<VulkanCommandBuffer&>(*cmd);
    if(!vulkan::CanGenerateAlbedoMipChain(vk,native,desc)) {
        if(owned!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(owned);
        PRIMAL_LOG_STREAM(Error,"Render") << "[MaterialDataBuilder] checked Vulkan albedo mips unsupported format/queue/state";
        return false;
    }
    const auto candidate=device->CreateTexture(desc);
    if(candidate==handles::INVALID_RESOURCE){if(owned!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(owned);return false;}
    bool ok=true;
    for(u32 layer=0;layer<sources.size()&&ok;++layer)
        ok=vulkan::CopyAlbedoMip0Layer(vk,native,sources[layer],candidate,layer,layer==0);
    if(ok)ok=vulkan::GenerateAlbedoMipChain(vk,native,candidate);
    if(owned!=handles::INVALID_COMMAND_BUFFER) {
        if(ok)ok=cmd->End()&&cmd->Submit(0)&&cmd->WaitForCompletion();
        device->DestroyCommandBuffer(owned);
    }
    if(!ok) {
        // For a supplied command buffer, false requires its owner to discard
        // that recording; no partially initialized array may be published.
        device->DestroyTexture(candidate);return false;
    }
    output=candidate;
    PRIMAL_LOG_STREAM(Info,"Render") << "[MaterialDataBuilder] checked Vulkan albedo array layers=" << desc.arraySize
        << " mips=" << desc.mipLevels << " format=" << u32(desc.format) << " mip0=exact-copy placeholder=explicit";
    return true;
}
}
#endif

bool MaterialDataBuilder::CreateTextureArray(
    rhi::RHIDeviceBase* device,
    rhi::RHICommandBuffer* cmdBuffer,
    const std::vector<rhi::ResourceHandle>& sources,
    const char* name,
    rhi::ResourceHandle& outArray,
    TextureArrayChannel channel
) {
    if (sources.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] No source textures for " << name << std::endl;
        return false;
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Creating texture array '" << name
              << "' with " << sources.size() << " layers..." << std::endl;

    // Get texture info from first NON-placeholder source.
    // The test creates 1x1 fallback textures when normal/ORM paths are missing;
    // using one of those as the array's reference size would force every real
    // 1024x1024 texture to be skipped as a size mismatch.
    rhi::ResourceHandle firstValidTex = rhi::handles::INVALID_RESOURCE;
    for (const auto& tex : sources) {
        if (tex == rhi::handles::INVALID_RESOURCE) continue;
        math::u32v2 s = GetTextureSize(device, tex);
        if (s.x > 1 && s.y > 1) {
            firstValidTex = tex;
            break;
        }
    }
    if (firstValidTex == rhi::handles::INVALID_RESOURCE) {
        // All sources are 1x1 (or invalid). Fall back to first non-invalid.
        for (const auto& tex : sources) {
            if (tex != rhi::handles::INVALID_RESOURCE) {
                firstValidTex = tex;
                break;
            }
        }
    }

    if (firstValidTex == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] No valid textures for " << name << std::endl;
        return false;
    }

    // Infer size from first valid texture.
    math::u32v2 size = GetTextureSize(device, firstValidTex);

    // Pick the format used by the MAJORITY of sources. The first source may be
    // a placeholder with a different format (sRGB vs UNorm), and WebGPU
    // requires all layers of a texture_2d_array to share the exact format —
    // otherwise every real-texture layer gets BlitTexture-skipped at runtime
    // and sampling returns undefined memory.
    std::unordered_map<u32, u32> formatHistogram;
    for (const auto& tex : sources) {
        if (tex == rhi::handles::INVALID_RESOURCE) continue;
        math::u32v2 s = GetTextureSize(device, tex);
        if (s.x != size.x || s.y != size.y) continue;
        u32 fmt = static_cast<u32>(GetTextureFormat(device, tex));
        formatHistogram[fmt]++;
    }
    u32 bestFormat = 0;
    u32 bestCount = 0;
    for (const auto& [fmt, count] : formatHistogram) {
        if (count > bestCount) {
            bestCount = count;
            bestFormat = fmt;
        }
    }
    rhi::DataFormat format = static_cast<rhi::DataFormat>(bestFormat);

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Texture info: " << size.x << "x" << size.y
              << ", format=" << static_cast<u32>(format)
              << " (picked from " << formatHistogram.size() << " unique formats"
              << ", best count=" << bestCount << ")" << std::endl;

    // Create texture array
    rhi::TextureDesc arrayDesc{};
    arrayDesc.name = name;
    arrayDesc.type = rhi::TextureType::Texture2DArray;
    arrayDesc.format = format;
    arrayDesc.size = {size.x, size.y, 1};
    arrayDesc.arraySize = static_cast<u32>(sources.size());
    arrayDesc.mipLevels = 1;  // TODO: Support mipmaps
    arrayDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;
    arrayDesc.memoryUsage = rhi::GPUMemoryUsage::Static;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
    const char* mipPolicy=std::getenv("PRIMAL_VK_ALBEDO_MIPS");
    if(device->GetPlatform()==rhi::RHIPlatform::Vulkan&&channel==TextureArrayChannel::Albedo&&
       mipPolicy&&std::strcmp(mipPolicy,"1")==0)
        return BuildCheckedVulkanAlbedoArray(device,cmdBuffer,sources,arrayDesc,outArray);
#else
    (void)channel;
#endif
    outArray = device->CreateTexture(arrayDesc);
    if (outArray == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Failed to create texture array '" << name << "'" << std::endl;
        return false;
    }

    // Acquire our own command buffer if none was provided.
    // The caller (BuildTextureArrays) passes nullptr because it runs at scene-load
    // time outside any render loop; without a cmd buffer, the array is allocated
    // but every layer remains uninitialized, and sampling returns 0.
    rhi::CommandQueueType queueType = rhi::CommandQueueType::Transfer;
    rhi::ResourceHandle ownedCmdHandle = rhi::handles::INVALID_RESOURCE;
    rhi::RHICommandBuffer* activeCmd = cmdBuffer;
    if (!activeCmd) {
        ownedCmdHandle = device->CreateCommandBuffer(queueType);
        activeCmd = rhi::GetCommandBuffer(ownedCmdHandle);
        if (!activeCmd) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Failed to acquire command buffer for "
                      << name << std::endl;
            return false;
        }
        activeCmd->Begin();
    }

    u32 copiedLayers = 0;
    u32 skippedLayers = 0;
    for (size_t layer = 0; layer < sources.size(); ++layer) {
        if (sources[layer] == rhi::handles::INVALID_RESOURCE) {
            continue;  // Skip invalid textures
        }

        // Verify texture compatibility
        math::u32v2 texSize = GetTextureSize(device, sources[layer]);
        if (texSize.x != size.x || texSize.y != size.y) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: '" << name << "' layer "
                      << layer << " size " << texSize.x << "x" << texSize.y
                      << " != array " << size.x << "x" << size.y << ", skipping"
                      << std::endl;
            ++skippedLayers;
            continue;
        }

        rhi::DataFormat srcFormat = GetTextureFormat(device, sources[layer]);
        if (srcFormat != format) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] WARNING: '" << name << "' layer "
                      << layer << " format " << static_cast<u32>(srcFormat)
                      << " != array format " << static_cast<u32>(format)
                      << ", skipping (WebGPU requires exact match)" << std::endl;
            ++skippedLayers;
            continue;
        }

        // Setup blit region
        rhi::TextureBlitRegion region{};
        region.srcSubresource = {0, 0, 1};      // mip 0, layer 0
        region.dstSubresource = {0, static_cast<u32>(layer), 1};  // mip 0, target layer
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {static_cast<s32>(size.x), static_cast<s32>(size.y), 1};
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {static_cast<s32>(size.x), static_cast<s32>(size.y), 1};

        activeCmd->BlitTexture(
            sources[layer],
            outArray,
            &region,
            1,
            rhi::FilterMode::Linear
        );
        ++copiedLayers;
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] '" << name << "' copied " << copiedLayers
              << " layers, skipped " << skippedLayers << std::endl;

    // If we owned the command buffer, submit and wait now.
    if (ownedCmdHandle != rhi::handles::INVALID_RESOURCE) {
        // T4.6.5 part 30: BlitTexture leaves the array in TRANSFER_DST_OPTIMAL.
        // Subsequent descriptor writes hardcode SHADER_READ_ONLY_OPTIMAL
        // (VulkanDescriptorSet.cpp:131) → VUID-vkCmdDraw-None-09600 fires.
        // Transition all layers+mips to ShaderResource so sampling works.
        // When the caller passes its own cmd buffer, the caller owns this
        // transition (mirror CopyBufferToTexture contract).
        rhi::ResourceBarrier toSRV;
        toSRV.resource = outArray;
        toSRV.beforeState = rhi::ResourceState::CopyDest;
        toSRV.afterState = rhi::ResourceState::ShaderResource;
        toSRV.subresource = rhi::RHI_ALL_SUBRESOURCES;
        toSRV.queueFamily = 0xFFFFFFFF;
        activeCmd->InsertBarrier(&toSRV, 1);

        activeCmd->End();
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = ownedCmdHandle;
        device->Submit(submitInfo);
        activeCmd->WaitForCompletion();
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[MaterialDataBuilder] Texture array '" << name << "' created successfully" << std::endl;
    return true;
}

rhi::ResourceHandle MaterialDataBuilder::GetTextureSafe(
    graphics::MaterialInstance* instance,
    u32 binding
) {
    if (!instance) {
        return rhi::handles::INVALID_RESOURCE;
    }

    rhi::ResourceHandle handle = instance->GetTextureHandle(binding);
    if (handle == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataBuilder] Missing texture at binding " << binding << std::endl;
    }

    return handle;
}

} // namespace primal::graphics::nanite
