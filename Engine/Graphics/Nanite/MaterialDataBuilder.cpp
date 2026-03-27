#include "MaterialDataBuilder.h"
#include "../MaterialInstance.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Platforms/Metal/MetalDevice.h"
#include "../RHI/Platforms/Metal/MetalTexture.h"
#include "../../Content/ContentToEngine.h"
#include "../../Utilities/IOStream.h"
#include <iostream>
#include <cmath>
#include <algorithm>

namespace primal::graphics::nanite {

GPUMaterialRegistry::MaterialData MaterialDataBuilder::ExtractMaterialData(
    graphics::MaterialInstance* instance
) {
    GPUMaterialRegistry::MaterialData data{};

    if (!instance) {
        std::cerr << "[MaterialDataBuilder] Null material instance" << std::endl;
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
    data.flags = 0;

    return data;
}

GPUMaterialRegistry::MaterialData MaterialDataBuilder::ExtractMaterialData(
    graphics::MaterialInstance* instance,
    const TextureArrayBuildContext& ctx
) {
    GPUMaterialRegistry::MaterialData data{};

    if (!instance) {
        std::cerr << "[MaterialDataBuilder] Null material instance" << std::endl;
        return data;
    }

    // 🎨 NEW: Use texture mapping to get array indices
    rhi::ResourceHandle albedo = instance->GetTextureHandle(0);
    auto it = ctx.albedoToIndex.find(albedo);
    // 🔥 CRITICAL FIX: If texture not found, use INVALID (0xFFFFFFFF) instead of 0
    // This prevents accidentally sampling the wrong texture layer
    data.albedo_texture_idx = (it != ctx.albedoToIndex.end()) ? it->second : 0xFFFFFFFF;

    rhi::ResourceHandle normal = instance->GetTextureHandle(1);
    it = ctx.normalToIndex.find(normal);
    data.normal_texture_idx = (it != ctx.normalToIndex.end()) ? it->second : 0xFFFFFFFF;

    rhi::ResourceHandle orm = instance->GetTextureHandle(2);
    it = ctx.ormToIndex.find(orm);
    data.orm_texture_idx = (it != ctx.ormToIndex.end()) ? it->second : 0xFFFFFFFF;

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
    data.flags = 0;

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
        std::cerr << "[MaterialDataBuilder] WARNING: No material instances to build" << std::endl;
        return true;  // Not an error, just nothing to do
    }

    std::cout << "[MaterialDataBuilder] Building texture arrays for "
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
        if (!CreateTextureArray(device, nullptr, ctx.uniqueAlbedo, "AlbedoTextureArray", outAlbedoArray)) {
            std::cerr << "[MaterialDataBuilder] Failed to create albedo texture array" << std::endl;
            success = false;
        }
    }

    if (!ctx.uniqueNormal.empty()) {
        if (!CreateTextureArray(device, nullptr, ctx.uniqueNormal, "NormalTextureArray", outNormalArray)) {
            std::cerr << "[MaterialDataBuilder] Failed to create normal texture array" << std::endl;
            success = false;
        }
    }

    if (!ctx.uniqueORM.empty()) {
        if (!CreateTextureArray(device, nullptr, ctx.uniqueORM, "ORMTextureArray", outORMArray)) {
            std::cerr << "[MaterialDataBuilder] Failed to create ORM texture array" << std::endl;
            success = false;
        }
    }

    std::cout << "[MaterialDataBuilder] Texture array build complete (Phase 2: mapping created, copying TODO)" << std::endl;
    return success;
}

jobsystem::JobHandle MaterialDataBuilder::BuildAsync(
    rhi::RHIDeviceBase* device,
    const std::vector<graphics::MaterialInstance*>& instances,
    GPUMaterialRegistry* registry
) {
    // TODO: Implement async build
    std::cout << "[MaterialDataBuilder] BuildAsync not yet implemented" << std::endl;
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

    // 🎨 NEW: Get texture size from Metal texture
    math::u32v2 GetTextureSize(rhi::RHIDeviceBase* device, rhi::ResourceHandle texture) {
        using namespace rhi;

        // Access Metal texture through MetalDevice
        MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
        MetalTexture* metalTex = metalDevice->GetTexture(texture);

        if (metalTex && metalTex->GetNativeTexture()) {
            MTL::Texture* mtlTex = metalTex->GetNativeTexture();
            return {static_cast<u32>(mtlTex->width()), static_cast<u32>(mtlTex->height())};
        }

        // Fallback to default size
        std::cerr << "[MaterialDataBuilder] WARNING: Could not get texture size, using default 512x512" << std::endl;
        return {512, 512};
    }

    // 🎨 NEW: Get texture format from Metal texture
    rhi::DataFormat GetTextureFormat(rhi::RHIDeviceBase* device, rhi::ResourceHandle texture) {
        using namespace rhi;

        // Access Metal texture through MetalDevice
        MetalDevice* metalDevice = static_cast<MetalDevice*>(device);
        MetalTexture* metalTex = metalDevice->GetTexture(texture);

        if (metalTex && metalTex->GetNativeTexture()) {
            MTL::Texture* mtlTex = metalTex->GetNativeTexture();
            // Convert MTLPixelFormat to DataFormat
            // This is a simplified conversion - may need more cases
            switch (mtlTex->pixelFormat()) {
                case MTL::PixelFormatRGBA8Unorm:
                    return rhi::DataFormat::RGBA8_UNorm;
                case MTL::PixelFormatRGBA8Unorm_sRGB:
                    return rhi::DataFormat::RGBA8_sRGB;
                case MTL::PixelFormatBGRA8Unorm:
                case MTL::PixelFormatBGRA8Unorm_sRGB:
                    // RHI doesn't have BGRA formats, map to RGBA
                    return rhi::DataFormat::RGBA8_UNorm;
                default:
                    std::cerr << "[MaterialDataBuilder] WARNING: Unknown pixel format " << static_cast<u32>(mtlTex->pixelFormat()) << ", using RGBA8_UNorm" << std::endl;
                    return rhi::DataFormat::RGBA8_UNorm;
            }
        }

        // Fallback to default format
        std::cerr << "[MaterialDataBuilder] WARNING: Could not get texture format, using RGBA8_UNorm" << std::endl;
        return rhi::DataFormat::RGBA8_UNorm;
    }
}

// 🎨 NEW: Create placeholder textures (1024x1024 default textures to match array size)
void MaterialDataBuilder::CreatePlaceholderTextures(
    TextureArrayBuildContext& ctx,
    rhi::RHIDeviceBase* device
) {
    std::cout << "[MaterialDataBuilder] Creating 1024x1024 placeholder textures..." << std::endl;

    // 🎨 CRITICAL: All textures in array must be same size
    // Sponza uses 1024x1024, so placeholders must match
    constexpr u32 TARGET_SIZE = 1024;
    constexpr u32 PIXEL_COUNT = TARGET_SIZE * TARGET_SIZE;
    constexpr u32 DATA_SIZE = PIXEL_COUNT * 4;  // RGBA

    // Helper to create texture from pixel data using content system
    auto CreateTextureFromPixels = [device](const std::vector<u32>& pixels, rhi::DataFormat format, const char* name) -> rhi::ResourceHandle {
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
        writer.write((u32)format);       // format
        writer.write(row_pitch);
        writer.write(slice_pitch);
        writer.write(reinterpret_cast<const char*>(pixels.data()), slice_pitch);

        primal::id::id_type id = primal::content::create_resource(blob.data(), primal::content::asset_type::texture);
        if (primal::id::is_valid(id)) {
            return primal::content::get_rhi_texture_handle(id);
        }
        return rhi::handles::INVALID_RESOURCE;
    };

    // 1. Albedo placeholder (white)
    {
        std::vector<u32> whiteData(PIXEL_COUNT, 0xFFFFFFFF);  // RGBA = (1, 1, 1, 1)
        ctx.placeholderAlbedo = CreateTextureFromPixels(whiteData, rhi::DataFormat::RGBA8_sRGB, "PlaceholderAlbedo");
    }

    // 2. Normal placeholder (flat normal = 0, 0, 1)
    {
        // Normal = (0, 0, 1) → RGB = (128, 128, 255) in [0,255] range
        // For ABGR format (Metal): A=255, B=255, G=128, R=128 → 0xFF8080FF
        std::vector<u32> normalData(PIXEL_COUNT, 0xFF8080FF);
        ctx.placeholderNormal = CreateTextureFromPixels(normalData, rhi::DataFormat::RGBA8_UNorm, "PlaceholderNormal");
    }

    // 3. ORM placeholder (AO=1, R=0.5, M=0)
    {
        // AO=1, R=0.5, M=0 → RGB = (255, 128, 0)
        // For ABGR format: A=255, B=0, G=128, R=255 → 0xFF0080FF
        std::vector<u32> ormData(PIXEL_COUNT, 0xFF0080FF);
        ctx.placeholderORM = CreateTextureFromPixels(ormData, rhi::DataFormat::RGBA8_UNorm, "PlaceholderORM");
    }

    std::cout << "[MaterialDataBuilder] 1024x1024 placeholder textures created" << std::endl;
}

// 🎨 NEW: Build texture mapping from material instances
void MaterialDataBuilder::BuildTextureMapping(
    const std::vector<graphics::MaterialInstance*>& instances,
    TextureArrayBuildContext& ctx,
    rhi::RHIDeviceBase* device
) {
    std::cout << "[MaterialDataBuilder] Building texture mapping..." << std::endl;

    // 1. Create placeholder textures (store for later use, don't add to mapping yet)
    CreatePlaceholderTextures(ctx, device);

    // 🔥 CRITICAL FIX: Do NOT add placeholder to mapping automatically
    // This ensures MaterialID 0 maps to texture layer 0
    // Placeholder will be used only when texture is missing (index lookup returns default 0)

    // 2. Collect unique textures from all material instances
    std::cout << "[MaterialDataBuilder] Processing " << instances.size() << " material instances..." << std::endl;

    for (size_t i = 0; i < instances.size(); ++i) {
        const auto* instance = instances[i];
        if (!instance) {
            std::cout << "  [" << i << "] NULL instance, skipping" << std::endl;
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
        std::cout << "  [MaterialID=" << i << "] Texture indices BEFORE: "
                  << "albedo_idx=" << albedoIdx << ", "
                  << "normal_idx=" << normalIdx << ", "
                  << "orm_idx=" << ormIdx << std::endl;

        // Debug output for first few materials (texture handles)
        if (i < 5) {
            std::cout << "    Texture handles: "
                      << "albedo=" << albedo << ", "
                      << "normal=" << normal << ", "
                      << "orm=" << orm;

            // Check if invalid
            if (albedo == rhi::handles::INVALID_RESOURCE &&
                normal == rhi::handles::INVALID_RESOURCE &&
                orm == rhi::handles::INVALID_RESOURCE) {
                std::cout << " ❌ ALL INVALID";
            } else {
                std::cout << " ✓";
            }
            std::cout << std::endl;
        }

        // Add to mapping with deduplication
        AddTextureToMapping(ctx.albedoToIndex, ctx.uniqueAlbedo, albedo);
        AddTextureToMapping(ctx.normalToIndex, ctx.uniqueNormal, normal);
        AddTextureToMapping(ctx.ormToIndex, ctx.uniqueORM, orm);

        // 🔥 NEW DEBUG: Print texture indices AFTER adding
        uint32_t newAlbedoIdx = ctx.albedoToIndex[albedo];
        uint32_t newNormalIdx = ctx.normalToIndex[normal];
        uint32_t newOrmIdx = ctx.ormToIndex[orm];

        std::cout << "  [MaterialID=" << i << "] Texture indices AFTER: "
                  << "albedo_idx=" << newAlbedoIdx << ", "
                  << "normal_idx=" << newNormalIdx << ", "
                  << "orm_idx=" << newOrmIdx << std::endl;
    }

    std::cout << "[MaterialDataBuilder] Texture mapping complete. Total unique textures: "
              << ctx.uniqueAlbedo.size() << " albedo, "
              << ctx.uniqueNormal.size() << " normal, "
              << ctx.uniqueORM.size() << " ORM" << std::endl;

    // 🔥 CRITICAL DEBUG: Print final mapping for all materials
    std::cout << "[MaterialDataBuilder] FINAL TEXTURE INDEX MAPPING:" << std::endl;
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto* instance = instances[i];
        if (!instance) continue;

        rhi::ResourceHandle albedo = instance->GetTextureHandle(0);
        rhi::ResourceHandle normal = instance->GetTextureHandle(1);
        rhi::ResourceHandle orm = instance->GetTextureHandle(2);

        uint32_t albedoIdx = ctx.albedoToIndex[albedo];
        uint32_t normalIdx = ctx.normalToIndex[normal];
        uint32_t ormIdx = ctx.ormToIndex[orm];

        std::cout << "  MaterialID=" << i << " → "
                  << "albedo_layer=" << albedoIdx << ", "
                  << "normal_layer=" << normalIdx << ", "
                  << "orm_layer=" << ormIdx << std::endl;
    }
}

// 🎨 NEW: Create texture array using BlitTexture
bool MaterialDataBuilder::CreateTextureArray(
    rhi::RHIDeviceBase* device,
    rhi::RHICommandBuffer* cmdBuffer,
    const std::vector<rhi::ResourceHandle>& sources,
    const char* name,
    rhi::ResourceHandle& outArray
) {
    if (sources.empty()) {
        std::cerr << "[MaterialDataBuilder] No source textures for " << name << std::endl;
        return false;
    }

    std::cout << "[MaterialDataBuilder] Creating texture array '" << name
              << "' with " << sources.size() << " layers..." << std::endl;

    // Get texture info from first valid source
    rhi::ResourceHandle firstValidTex = rhi::handles::INVALID_RESOURCE;
    for (const auto& tex : sources) {
        if (tex != rhi::handles::INVALID_RESOURCE) {
            firstValidTex = tex;
            break;
        }
    }

    if (firstValidTex == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[MaterialDataBuilder] No valid textures for " << name << std::endl;
        return false;
    }

    // Infer size and format from first texture
    math::u32v2 size = GetTextureSize(device, firstValidTex);
    rhi::DataFormat format = GetTextureFormat(device, firstValidTex);

    std::cout << "[MaterialDataBuilder] Texture info: " << size.x << "x" << size.y
              << ", format=" << static_cast<u32>(format) << std::endl;

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

    outArray = device->CreateTexture(arrayDesc);
    if (outArray == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[MaterialDataBuilder] Failed to create texture array '" << name << "'" << std::endl;
        return false;
    }

    // 🎨 TODO: Copy each texture to array layer using BlitTexture
    // This requires cmdBuffer to be valid (not nullptr)
    // For now, the texture array is created but empty
    if (cmdBuffer) {
        std::cout << "[MaterialDataBuilder] Copying textures to array..." << std::endl;

        for (size_t layer = 0; layer < sources.size(); ++layer) {
            if (sources[layer] == rhi::handles::INVALID_RESOURCE) {
                continue;  // Skip invalid textures
            }

            // Verify texture compatibility
            math::u32v2 texSize = GetTextureSize(device, sources[layer]);
            if (texSize.x != size.x || texSize.y != size.y) {
                std::cerr << "[MaterialDataBuilder] WARNING: Texture at layer " << layer
                          << " has different size (" << texSize.x << "x" << texSize.y << "), skipping" << std::endl;
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

            // Perform blit
            cmdBuffer->BlitTexture(
                sources[layer],
                outArray,
                &region,
                1,
                rhi::FilterMode::Linear
            );
        }

        std::cout << "[MaterialDataBuilder] Texture copying complete" << std::endl;
    } else {
        std::cout << "[MaterialDataBuilder] WARNING: No command buffer provided, textures not copied" << std::endl;
    }

    std::cout << "[MaterialDataBuilder] Texture array '" << name << "' created successfully" << std::endl;
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
        std::cerr << "[MaterialDataBuilder] Missing texture at binding " << binding << std::endl;
    }

    return handle;
}

} // namespace primal::graphics::nanite
