/**
 * @file VulkanTexture.cpp
 * @brief VulkanTexture 实现
 * @details VMA 集成:
 *          - Static/Immutable/RenderTarget/DepthStencil: AUTO_PREFER_DEVICE + DEVICE_LOCAL
 *          - Dynamic/Staging: HOST_VISIBLE(很少用)
 *          image layout 跟踪:Initialize 完成后默认 UNDEFINED,首张上传需 transition 到 TRANSFER_DST_OPTIMAL。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanTexture.h"
#include "VulkanDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include <vk_mem_alloc.h>
#include "VulkanHangDiag.h"
#endif

#include <iostream>
#include <cstring>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi {

namespace {

// 计算图像字节数(粗略,用于 ResourceDesc.size 占位)
// 不参与实际 VMA 分配,VMA 会按 image 真实占用算。
u64 EstimateTextureSizeBytes(const TextureDesc& desc) {
    u32 w = desc.size.x;
    u32 h = desc.size.y;
    u32 d = std::max<u32>(1u, desc.size.z);
    // 简化:假设 4 bytes/texel(大部分颜色格式)
    u64 bytesPerTexel = 4;
    switch (desc.format) {
        case DataFormat::R8_UNorm: case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:  case DataFormat::R8_SInt:
            bytesPerTexel = 1; break;
        case DataFormat::R16_Float: case DataFormat::R16_UInt:
        case DataFormat::RG8_UNorm: case DataFormat::RG8_SNorm:
            bytesPerTexel = 2; break;
        case DataFormat::R32_Float: case DataFormat::R32_UInt:
        case DataFormat::RG16_Float:
            bytesPerTexel = 4; break;
        case DataFormat::RGBA16_Float:
            bytesPerTexel = 8; break;
        case DataFormat::RGBA32_Float:
            bytesPerTexel = 16; break;
        case DataFormat::D32_Float:
            bytesPerTexel = 4; break;
        case DataFormat::D24_UNorm_S8_UInt:
        case DataFormat::D32_Float_S8X24_UInt:
            bytesPerTexel = 4; break;
        default:
            bytesPerTexel = 4; break;
    }
    u64 total = u64(w) * u64(h) * u64(d) * bytesPerTexel;
    // 加 mip chain
    u32 mipTotal = 1;
    u32 maxDim = std::max({w, h, d});
    while (maxDim > 1) { maxDim >>= 1; ++mipTotal; }
    u32 mipCount = desc.mipLevels > 0 ? desc.mipLevels : mipTotal;
    for (u32 m = 1; m < mipCount; ++m) {
        u32 mw = std::max<u32>(1u, w >> m);
        u32 mh = std::max<u32>(1u, h >> m);
        u32 md = std::max<u32>(1u, d >> m);
        total += u64(mw) * u64(mh) * u64(md) * bytesPerTexel;
    }
    if (desc.arraySize > 1) total *= desc.arraySize;
    return total;
}

} // anonymous namespace

VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc)
    : RHIResource(device, ResourceDesc(
          ResourceType::Texture,
          ResourceUsage::None,
          desc.memoryUsage,
          EstimateTextureSizeBytes(desc),
          desc.name.c_str())),
      vkUsageFlags_(vulkan::ToVkImageUsageFlags(desc.usage)),
      vkFormat_(vulkan::ToVkFormat(desc.format)),
      ownsImage_(true),
      currentLayout_(VK_IMAGE_LAYOUT_UNDEFINED),
      texDesc_(desc)
{
    mipLayouts_.assign(std::max<u32>(1u, desc.mipLevels), VK_IMAGE_LAYOUT_UNDEFINED);
}

VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc, VkImage existingImage)
    : RHIResource(device, ResourceDesc(
          ResourceType::Texture,
          ResourceUsage::None,
          desc.memoryUsage,
          EstimateTextureSizeBytes(desc),
          desc.name.c_str())),
      vkUsageFlags_(vulkan::ToVkImageUsageFlags(desc.usage)),
      vkFormat_(vulkan::ToVkFormat(desc.format)),
      ownsImage_(false),
      wrappedImage_(existingImage),
      currentLayout_(VK_IMAGE_LAYOUT_UNDEFINED),
      texDesc_(desc)
{
    mipLayouts_.assign(std::max<u32>(1u, desc.mipLevels), VK_IMAGE_LAYOUT_UNDEFINED);
}

VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureViewDesc& viewDesc, VulkanTexture& src)
    : RHIResource(device, ResourceDesc(
          ResourceType::Texture,
          ResourceUsage::None,
          src.texDesc_.memoryUsage,
          0,
          "TextureView")),
      vkUsageFlags_(src.vkUsageFlags_),
      vkFormat_(vulkan::ToVkFormat(viewDesc.format != DataFormat::Unknown ? viewDesc.format
                                                                          : src.texDesc_.format)),
      ownsImage_(false),
      isView_(true),
      viewDesc_(viewDesc),
      viewSrc_(&src),
      currentLayout_(src.currentLayout_),
      texDesc_(src.texDesc_)
{
    // View 子集 geometry: 维度不变,只 subresourceRange 缩窄。texDesc_ 主要给
    // GetTextureDesc() caller 提供 format/type 信息;真实 GPU extent 来自源 image。
    texDesc_.type = viewDesc.viewType;
    texDesc_.format = viewDesc.format != DataFormat::Unknown ? viewDesc.format : src.texDesc_.format;
    texDesc_.mipLevels = std::max<u32>(1u, viewDesc.mipCount);
    texDesc_.arraySize = std::max<u32>(1u, viewDesc.arraySize);
    mipLayouts_ = src.mipLayouts_;  // 跟随 source per-mip layout
}

VulkanTexture::VulkanTexture(VulkanTexture&& other) noexcept
    : RHIResource(std::move(other)),
      vkImage_(other.vkImage_),
      vkView_(other.vkView_),
      allocation_(other.allocation_),
      mappedPtr_(other.mappedPtr_),
      warnedMapUnsupported_(other.warnedMapUnsupported_),
      vkUsageFlags_(other.vkUsageFlags_),
      vkFormat_(other.vkFormat_),
      ownsImage_(other.ownsImage_),
      wrappedImage_(other.wrappedImage_),
      isView_(other.isView_),
      viewDesc_(other.viewDesc_),
      viewSrc_(other.viewSrc_),
      currentLayout_(other.currentLayout_),
      mipLayouts_(std::move(other.mipLayouts_)),
      texDesc_(std::move(other.texDesc_)) {
    other.vkImage_ = VK_NULL_HANDLE;
    other.vkView_ = VK_NULL_HANDLE;
    other.allocation_ = nullptr;
    other.mappedPtr_ = nullptr;
    other.ownsImage_ = true;
    other.wrappedImage_ = VK_NULL_HANDLE;
    other.isView_ = false;
    other.viewSrc_ = nullptr;
    other.currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

VulkanTexture& VulkanTexture::operator=(VulkanTexture&& other) noexcept {
    if (this != &other) {
        destroyImpl();
        RHIResource::operator=(std::move(other));
        vkImage_ = other.vkImage_;
        vkView_ = other.vkView_;
        allocation_ = other.allocation_;
        mappedPtr_ = other.mappedPtr_;
        warnedMapUnsupported_ = other.warnedMapUnsupported_;
        vkUsageFlags_ = other.vkUsageFlags_;
        vkFormat_ = other.vkFormat_;
        ownsImage_ = other.ownsImage_;
        wrappedImage_ = other.wrappedImage_;
        isView_ = other.isView_;
        viewDesc_ = other.viewDesc_;
        viewSrc_ = other.viewSrc_;
        currentLayout_ = other.currentLayout_;
        mipLayouts_ = std::move(other.mipLayouts_);
        texDesc_ = std::move(other.texDesc_);
        other.vkImage_ = VK_NULL_HANDLE;
        other.vkView_ = VK_NULL_HANDLE;
        other.allocation_ = nullptr;
        other.mappedPtr_ = nullptr;
        other.ownsImage_ = true;
        other.wrappedImage_ = VK_NULL_HANDLE;
        other.isView_ = false;
        other.viewSrc_ = nullptr;
        other.currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return *this;
}

VulkanTexture::~VulkanTexture() {
    destroyImpl();
}

bool VulkanTexture::Initialize() {
    VulkanDevice& vkDevice = static_cast<VulkanDevice&>(device_);
    VkDevice dev = vkDevice.GetNativeDevice();
    if (dev == VK_NULL_HANDLE) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] device not initialized" << std::endl;
        return false;
    }
    if (vkFormat_ == VK_FORMAT_UNDEFINED) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] DataFormat not mapped to VkFormat" << std::endl;
        return false;
    }

    // === Phase 5 view 模式:alias source VkImage + 创建受限 subresourceRange view ===
    // 不拥有 VkImage(viewSrc_ 拥有),只创建+拥有 VkImageView。
    if (isView_) {
        if (!viewSrc_ || viewSrc_->vkImage_ == VK_NULL_HANDLE) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] view mode but source image is null" << std::endl;
            return false;
        }
        vkImage_ = viewSrc_->vkImage_;  // 别名,不拥有

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = vkImage_;
        // viewType 推导:2D/2DArray/3D/Cube 直接映射,Unknown 兜底 2D。
        // T4.6.5 part 37: 2DArray 视图支持(创建 cube texture 的 2D-array 存储
        // 视图用于 equirect→cube 转换 shader 写入)。
        switch (viewDesc_.viewType) {
            case TextureType::Texture3D:
                vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            case TextureType::TextureCube:
                vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
                break;
            case TextureType::Texture2DArray:
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                break;
            case TextureType::Texture2D:
            case TextureType::Unknown:
            default:
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
        }
        vci.format = vkFormat_;
        vci.components = VkComponentMapping{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask =
            vulkan::IsDepthVkFormat(vkFormat_)
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = viewDesc_.mostDetailedMip;
        vci.subresourceRange.levelCount   = std::max<u32>(1u, viewDesc_.mipCount);
        vci.subresourceRange.baseArrayLayer = viewDesc_.firstArraySlice;
        vci.subresourceRange.layerCount   = std::max<u32>(1u, viewDesc_.arraySize);

        if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] view: vkCreateImageView failed" << std::endl;
            vkImage_ = VK_NULL_HANDLE;
            return false;
        }
        // Layout 跟随 source(view 共享同一 VkImage;layout 是 image-wide)。
        currentLayout_ = viewSrc_->currentLayout_;
        state_ = ResourceState::Ready;
        return true;
    }

    // === wrap 模式:跳过 VMA 分配,直接用外部 image(swapchain image) ===
    if (!ownsImage_) {
        if (wrappedImage_ == VK_NULL_HANDLE) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] wrap mode but wrappedImage_ is null" << std::endl;
            return false;
        }
        vkImage_ = wrappedImage_;  // 借用,不拥有

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = vkImage_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = vkFormat_;
        vci.components = VkComponentMapping{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask =
            vulkan::IsDepthVkFormat(vkFormat_)
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = std::max<u32>(1u, static_cast<u32>(mipLayouts_.size()));
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;

        if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] wrap: vkCreateImageView failed" << std::endl;
            vkImage_ = VK_NULL_HANDLE;
            return false;
        }
        state_ = ResourceState::Ready;
        return true;
    }

    // === 常规模式:VMA 分配 VkImage ===
    VmaAllocator allocator = vkDevice.GetVmaAllocator();
    if (!allocator) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] VMA allocator not initialized" << std::endl;
        return false;
    }
    if (vkUsageFlags_ == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] usage=Unknown rejected" << std::endl;
        return false;
    }

    // === P4c-F1: format capability 检查(vkGetPhysicalDeviceFormatProperties) ===
    // 不静默:usage 需要的 feature bit 缺失时显式 warn 并失败,
    // 而不是让 vkCreateImage / 首次 draw 抛出晦涩的 validation error。
    // transfer bits 仅 warn(Vulkan 1.0 实现按 spec 隐含支持,部分驱动不显式上报)。
    // P4c-F3: Staging/Readback 走 LINEAR tiling(host-visible 映射的 spec 正确
    // 路径),capability 按对应 tiling 的 features 查询。
    const bool hostVisibleUsage = (texDesc_.memoryUsage == GPUMemoryUsage::Staging ||
                                   texDesc_.memoryUsage == GPUMemoryUsage::Readback);
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(vkDevice.GetNativePhysicalDevice(), vkFormat_, &fp);
        const VkFormatFeatureFlags tilingFeatures =
            hostVisibleUsage ? fp.linearTilingFeatures : fp.optimalTilingFeatures;
        struct FeatureCheck { VkImageUsageFlagBits usage; VkFormatFeatureFlags required; const char* name; };
        const FeatureCheck checks[] = {
            { VK_IMAGE_USAGE_SAMPLED_BIT,              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,          "SAMPLED" },
            { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,     VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,       "COLOR_ATTACHMENT" },
            { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT" },
            { VK_IMAGE_USAGE_STORAGE_BIT,              VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,          "STORAGE_IMAGE" },
            { VK_IMAGE_USAGE_TRANSFER_SRC_BIT,         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,           "TRANSFER_SRC" },
            { VK_IMAGE_USAGE_TRANSFER_DST_BIT,         VK_FORMAT_FEATURE_TRANSFER_DST_BIT,           "TRANSFER_DST" },
        };
        const bool transferOnlyWarn = true;
        for (const auto& c : checks) {
            if ((vkUsageFlags_ & c.usage) == 0) continue;
            if ((fp.optimalTilingFeatures & c.required) == 0) {
                PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] format " << static_cast<int>(texDesc_.format)
                          << " (VkFormat " << static_cast<int>(vkFormat_) << ") lacks optimalTiling "
                          << c.name << " support required by usage — "
                          << ((c.usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT ||
                               c.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT) && transferOnlyWarn
                                  ? "continuing (implicit in Vk1.0)"
                                  : "rejecting")
                          << std::endl;
                if (!(transferOnlyWarn &&
                      (c.usage == VK_IMAGE_USAGE_TRANSFER_SRC_BIT ||
                       c.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT))) {
                    return false;
                }
            }
        }
    }

    // === VkImageCreateInfo ===
    const bool isCube = (texDesc_.type == TextureType::TextureCube);
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // Cube textures must declare the CUBE_COMPATIBLE bit so layers can be
    // interpreted as +X/-X/+Y/-Y/+Z/-Z faces when bound to a CUBE view.
    ici.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    // TextureType 在 RHIResource 基类被压平成 ResourceType::Texture;从 extent/type 推断:
    //   type==Texture3D 或 z > 1 → 3D(3D 允许 depth=1,与下方 view 判据保持同一口径,
    //   避免 Texture3D+z==1 产出 "2D image + 3D view" 的非法组合)
    ici.imageType = (texDesc_.size.z > 1 || texDesc_.type == TextureType::Texture3D)
                        ? VK_IMAGE_TYPE_3D
                        : VK_IMAGE_TYPE_2D;
    ici.format = vkFormat_;
    ici.extent.width  = std::max<u32>(1u, texDesc_.size.x);
    ici.extent.height = std::max<u32>(1u, texDesc_.size.y);
    ici.extent.depth  = std::max<u32>(1u, texDesc_.size.z);
    ici.mipLevels = std::max<u32>(1u, static_cast<u32>(mipLayouts_.size()));
    // Cube textures always have exactly 6 faces (one per array layer).
    // Otherwise use the caller-specified arraySize (1 for non-array, N for 2D-array).
    ici.arrayLayers = isCube ? 6u : std::max<u32>(1u, texDesc_.arraySize);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    // P4c-F3: host-visible 用途走 LINEAR(映射写 spec 正确);其余 OPTIMAL。
    ici.tiling = hostVisibleUsage ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    ici.usage = vkUsageFlags_;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.flags = 0;
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;  // Texture 默认 GPU-only
    // P4c-F3: Staging/Readback → HOST_VISIBLE + 持久 MAPPED(mapImpl 契约)。
    if (texDesc_.memoryUsage == GPUMemoryUsage::Staging) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.usage = VMA_MEMORY_USAGE_AUTO;
    } else if (texDesc_.memoryUsage == GPUMemoryUsage::Readback) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.usage = VMA_MEMORY_USAGE_AUTO;
    }
    aci.memoryTypeBits = 0;
    aci.pool = nullptr;
    aci.pUserData = nullptr;
    aci.priority = 0.5f;

    VmaAllocationInfo info{};
    VkResult res = vmaCreateImage(allocator, &ici, &aci, &vkImage_, &allocation_, &info);
    if (res != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] vmaCreateImage failed: " << res
                  << " w=" << ici.extent.width << " h=" << ici.extent.height
                  << " fmt=" << static_cast<int>(vkFormat_) << std::endl;
        return false;
    }
    mappedPtr_ = info.pMappedData;

    // === Default VkImageView ===
    // Choose viewType by TextureType:
    //   - Texture3D          → VK_IMAGE_VIEW_TYPE_3D         (SPIR-V texture_3d / storage_3d)
    //   - TextureCube        → VK_IMAGE_VIEW_TYPE_CUBE       (6 faces, sampler uses direction)
    //   - Texture2DArray     → VK_IMAGE_VIEW_TYPE_2D_ARRAY   (always — even arraySize=1; SPIR-V
    //                                                        OpTypeImage Arrayed=1 must match)
    //   - arraySize > 1      → VK_IMAGE_VIEW_TYPE_2D_ARRAY
    //   - else (single)      → VK_IMAGE_VIEW_TYPE_2D
    // Storage cube is not supported in Vulkan core; engine cube-storage paths use 2D-array.
    const bool is3D = (texDesc_.type == TextureType::Texture3D || texDesc_.size.z > 1);
    VkImageViewType viewType;
    u32 viewLayerCount;
    if (is3D) {
        viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewLayerCount = 1u;
    } else if (isCube) {
        viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewLayerCount = 6u;
    } else if (texDesc_.type == TextureType::Texture2DArray) {
        // Shader declares `texture_2d_array` → SPIR-V OpTypeImage Arrayed=1 → Vulkan
        // requires VK_IMAGE_VIEW_TYPE_2D_ARRAY, even when arraySize==1. Otherwise
        // vkCmdDrawIndirect throws VUID-vkCmdDrawIndirect-viewType-07752.
        viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewLayerCount = std::max<u32>(texDesc_.arraySize, 1u);
    } else if (texDesc_.arraySize > 1) {
        viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewLayerCount = texDesc_.arraySize;
    } else {
        viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewLayerCount = 1u;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = vkImage_;
    vci.viewType = viewType;
    vci.format = vkFormat_;
    vci.components = VkComponentMapping{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask =
        vulkan::IsDepthVkFormat(vkFormat_)
        ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = ici.mipLevels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = viewLayerCount;

    if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] vkCreateImageView failed" << std::endl;
        vmaDestroyImage(allocator, vkImage_, allocation_);
        vkImage_ = VK_NULL_HANDLE;
        allocation_ = nullptr;
        return false;
    }

    // Debug label
    if (desc_.name && vkDevice.GetDebugUtilsSetObjectName()) {
        VkDebugUtilsObjectNameInfoEXT nameInfo{};
        nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
        nameInfo.objectHandle = reinterpret_cast<u64>(vkImage_);
        nameInfo.pObjectName = desc_.name;
        vkDevice.GetDebugUtilsSetObjectName()(vkDevice.GetNativeDevice(), &nameInfo);
    }

    state_ = ResourceState::Ready;
    return true;
}

VkImageView VulkanTexture::GetLayerView(u32 layer) {
    // Single-layer textures: default view already covers layer 0.
    if (texDesc_.arraySize <= 1 && layer == 0) {
        return vkView_;
    }

    auto it = layerViews_.find(layer);
    if (it != layerViews_.end()) {
        return it->second;
    }

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = vkImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vkFormat_;
    vci.components = VkComponentMapping{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask = GetAspectMask();
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = texDesc_.mipLevels;
    vci.subresourceRange.baseArrayLayer = layer;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] GetLayerView vkCreateImageView failed for layer "
                  << layer << std::endl;
        return VK_NULL_HANDLE;
    }
    layerViews_[layer] = view;
    return view;
}

// ============================================================================
// P4c-F3: map / unmap / updateData
// ============================================================================
void* VulkanTexture::mapImpl(u64 offset, u64 size) {
    (void)size;
    if (mappedPtr_ != nullptr) {
        return static_cast<u8*>(mappedPtr_) + offset;
    }
    if (!warnedMapUnsupported_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] map on DEVICE_LOCAL texture unsupported — use "
                     "updateData (staging upload) or create with GPUMemoryUsage::Staging/"
                     "Readback. Further map attempts will fail silently." << std::endl;
        warnedMapUnsupported_ = true;
    }
    return nullptr;
}

void VulkanTexture::unmapImpl() {
    // 持久映射,nothing to do(VMA MAPPED 模式)。
}

bool VulkanTexture::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!data || size == 0) return false;
    if (!ownsImage_ || isView_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData on wrap/view texture unsupported" << std::endl;
        return false;
    }
    if (vkImage_ == VK_NULL_HANDLE || state_ != ResourceState::Ready) return false;

    // P4c-F3 范围:2D 单 slice mip0;3D/cube/array/BC 留给命令路径。
    if (texDesc_.size.z > 1 || texDesc_.type == TextureType::TextureCube ||
        texDesc_.arraySize > 1) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: only 2D single-slice supported (F3 scope), "
                     "use CopyBufferToTexture for 3D/cube/array" << std::endl;
        return false;
    }
    if (vulkan::IsBlockCompressedFormat(texDesc_.format)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: BC formats unsupported (block layout) — "
                     "use CopyBufferToTexture" << std::endl;
        return false;
    }
    const u64 bpt = vulkan::BytesPerTexel(texDesc_.format);
    if (bpt == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: unknown texel size for format "
                  << static_cast<int>(texDesc_.format) << std::endl;
        return false;
    }

    const u32 width  = std::max<u32>(1u, texDesc_.size.x);
    const u32 height = std::max<u32>(1u, texDesc_.size.y);
    const u64 bytesPerRow = u64(width) * bpt;
    if (offset % bytesPerRow != 0 || size % bytesPerRow != 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: offset/size must be row-aligned ("
                  << "bytesPerRow=" << bytesPerRow << ", offset=" << offset
                  << ", size=" << size << ")" << std::endl;
        return false;
    }
    const u32 y0 = static_cast<u32>(offset / bytesPerRow);
    const u32 rows = static_cast<u32>(size / bytesPerRow);
    if (y0 + rows > height) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: rows [" << y0 << ", " << y0 + rows
                  << ") exceed height " << height << std::endl;
        return false;
    }

    VulkanDevice& vkDev = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDev.GetVmaAllocator();
    VkDevice vkDevice = vkDev.GetNativeDevice();
    u32 queueFamily = vkDev.GetGraphicsQueueFamily();
    if (!allocator || queueFamily == UINT32_MAX) return false;

    const VkImageAspectFlags aspect = GetAspectMask();
    // 布局闭合由 RHI 内部负责(与 Metal 语义对齐,调用方无需手动 barrier):
    // currentLayout_ → TRANSFER_DST → 拷贝 → 回 currentLayout_(UNDEFINED 时
    // 提升为用途位决定的回落布局——R11 前硬编码 SRO,对无 SAMPLED 位的深度图
    // 触发 VUID-01211)。
    const VkImageLayout layoutBefore = currentLayout_;
    const VkImageLayout layoutAfter =
        (layoutBefore != VK_IMAGE_LAYOUT_UNDEFINED && layoutBefore != VK_IMAGE_LAYOUT_PREINITIALIZED)
            ? layoutBefore : GetRestingLayout();

    // === P4c-F4: 帧内模式 — staging 队列(下一 cmdbuf Begin 编码,零等待) ===
    // 契约:帧内更新的数据在下一个 command buffer 开始时可见;需要本 cmdbuf
    // 立即可见的调用方应使用无帧上下文的同步路径(资产加载期)。
    if (vkDev.IsFrameRecording()) {
        VulkanStagingAllocator& staging = vkDev.GetStagingAllocator();
        VulkanStagingAllocator::Allocation alloc = staging.Allocate(size, 256);
        if (!alloc.overflow) {
            std::memcpy(alloc.cpuPtr, data, size);
            staging.QueueBlit_Texture(alloc, vkImage_,
                                      /*mipLevel=*/0, /*slice=*/0,
                                      /*origin=*/0, y0, 0,
                                      /*extent=*/width, rows, 1,
                                      /*bytesPerRow=*/0, /*bytesPerImage=*/0,
                                      /*currentLayout=*/layoutBefore,
                                      /*backLayout=*/layoutAfter,
                                      /*aspect=*/aspect,
                                      /*texelSize=*/static_cast<u32>(bpt));
            // P0-B 定位: 帧内上传 trace（谁在帧录制期上传纹理并留下
            // TRANSFER_DST 中间态到下一 Begin 才闭合）
            if (std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
                fprintf(stderr, "[TEXUPLOAD] img=0x%llx name='%s' %ux%u deferred-upload\n",
                        (unsigned long long)(uintptr_t)vkImage_,
                        texDesc_.name.c_str(), width, height);
            }
            SetCurrentLayout(layoutAfter);
            return true;
        }
        // overflow fallback:排干已 queue 数据后走一次性路径(README 记录)
        staging.FlushBlocking();
    }

    // === 立即模式:一次性 staging + one-shot cmdbuf(资产加载期语义) ===
    VkBufferCreateInfo stagingCI{};
    stagingCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingCI.size = size;
    stagingCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo stagingACI{};
    stagingACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    stagingACI.usage = VMA_MEMORY_USAGE_AUTO;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(allocator, &stagingCI, &stagingACI,
                        &stagingBuf, &stagingAlloc, &stagingInfo) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateData: staging vmaCreateBuffer failed" << std::endl;
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, data, size);

    // 2. transient pool + one-shot cmdbuf(VulkanBuffer 慢路径同款)。
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &pci, nullptr, &pool) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ai.commandPool = pool;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &ai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto imageBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags srcAccess, VkPipelineStageFlags srcStage,
                            VkAccessFlags dstAccess, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = vkImage_;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;   // 只更新 mip0
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    imageBarrier(layoutBefore, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;   // 紧密行主序
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = aspect;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = {0, static_cast<int32_t>(y0), 0};
    copy.imageExtent = {width, rows, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, vkImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // 回布局:access/stage 按 final layout 配对(避免 VUID-VkImageMemoryBarrier-
    // imageLayout-01215 家族)。
    switch (layoutAfter) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            imageBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layoutAfter,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            imageBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layoutAfter,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            imageBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layoutAfter,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            break;
        default:
            imageBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layoutAfter,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            break;
    }
    vkEndCommandBuffer(cmd);

    // 3. submit + wait(同步语义:调用方期望返回后即可采样)。
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vkDev.GetGraphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vkDev.GetGraphicsQueue());

    vkFreeCommandBuffers(vkDevice, pool, 1, &cmd);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

    SetCurrentLayout(layoutAfter);
    return true;
}

// VT M0.7: 任意子区域上传。结构镜像 updateDataImpl,差异点:
// 任意 mip/origin 的 bounds 校验 + barrier 的 baseMipLevel 跟随 region。
bool VulkanTexture::updateRegion(const void* data, u64 size, const TextureRegionUpdate& region) {
    if (!data || size == 0) return false;
    if (!ownsImage_ || isView_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion on wrap/view texture unsupported" << std::endl;
        return false;
    }
    if (vkImage_ == VK_NULL_HANDLE || state_ != ResourceState::Ready) return false;

    // M6 F6-3：支持 2D 单 slice 与 TEXTURE3D（非 array）——3D 是 GI 体积
    // 数据（探针网格/SPGI 级联/SDF）主载体；array/cube 家族仍走命令层
    // CopyBufferToTexture。
    const bool is3D = texDesc_.type == TextureType::Texture3D;
    if ((!is3D && (texDesc_.type != TextureType::Texture2D || texDesc_.size.z > 1)) ||
        texDesc_.arraySize > 1) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: only 2D single-slice or "
                     "non-array Texture3D supported (M6 scope)" << std::endl;
        return false;
    }
    if (is3D && region.baseArrayLayer != 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: 3D requires baseArrayLayer=0" << std::endl;
        return false;
    }
    if (vulkan::IsBlockCompressedFormat(texDesc_.format)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: block-compressed formats unsupported — "
                     "use CopyBufferToTexture" << std::endl;
        return false;
    }
    const u64 bpt = vulkan::BytesPerTexel(texDesc_.format);
    if (bpt == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: unknown texel size for format "
                  << static_cast<int>(texDesc_.format) << std::endl;
        return false;
    }
    if (region.mipLevel >= texDesc_.mipLevels) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: mip " << region.mipLevel
                  << " >= mipLevels " << texDesc_.mipLevels << std::endl;
        return false;
    }
    if (region.offsetZ != 0 || region.depth != 1) {
        // 3D 允许 z 子区域；2D 保持平面约束。
        if (!is3D) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: 2D requires offsetZ=0/depth=1" << std::endl;
            return false;
        }
    }
    if (region.depth == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: depth==0" << std::endl;
        return false;
    }

    const u32 mipW = std::max<u32>(1u, texDesc_.size.x >> region.mipLevel);
    const u32 mipH = std::max<u32>(1u, texDesc_.size.y >> region.mipLevel);
    const u32 mipD = std::max<u32>(1u, texDesc_.size.z >> region.mipLevel);
    if (region.width == 0 || region.height == 0 ||
        region.offsetX + region.width > mipW || region.offsetY + region.height > mipH ||
        (is3D && region.offsetZ + region.depth > mipD)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: region [" << region.offsetX << ","
                  << region.offsetY << "," << region.offsetZ << " " << region.width << "x"
                  << region.height << "x" << region.depth
                  << ") exceeds mip " << region.mipLevel << " dims "
                  << mipW << "x" << mipH << "x" << mipD << std::endl;
        return false;
    }
    const u64 bytesPerRow = u64(region.width) * bpt;
    // 3D：紧密行主序 × depth 层（slice 间距 = bytesPerRow*height，与
    // bufferImageHeight=0 紧密约定一致）；2D 保持平面核算。
    const u64 expectedSize = bytesPerRow * region.height
                           * (is3D ? region.depth : 1u);
    if (size != expectedSize) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: size " << size << " != tightly-packed "
                  << expectedSize << " (bytesPerRow=" << bytesPerRow
                  << " x rows=" << region.height << "x depth=" << region.depth << ")" << std::endl;
        return false;
    }

    VulkanDevice& vkDev = static_cast<VulkanDevice&>(device_);
    VmaAllocator allocator = vkDev.GetVmaAllocator();
    VkDevice vkDevice = vkDev.GetNativeDevice();
    u32 queueFamily = vkDev.GetGraphicsQueueFamily();
    if (!allocator || queueFamily == UINT32_MAX) return false;

    const VkImageAspectFlags aspect = GetAspectMask();
    const VkImageLayout trackedLayout = mipLayouts_.empty()
                                            ? currentLayout_
                                            : GetMipLayout(region.mipLevel);
    // old 用真实跟踪布局(UNDEFINED 合法,首传丢弃语义);back-layout 在
    // 未定义时提升为用途位决定的回落布局(R11 前硬编码 SRO,无 SAMPLED 位
    // 的深度图非法,VUID-01201/01211 家族) — 两者不能混用(旧实现把提升值同时
    // 当 old 传给首屏障,触发 validation "expects SHADER_READ_ONLY, got
    // UNDEFINED")。
    const VkImageLayout layoutBefore = trackedLayout;
    const VkImageLayout layoutAfter =
        (trackedLayout != VK_IMAGE_LAYOUT_UNDEFINED && trackedLayout != VK_IMAGE_LAYOUT_PREINITIALIZED)
            ? trackedLayout : GetRestingLayout();

    // === 帧内模式:staging 队列(下一 cmdbuf Begin 编码,零等待) ===
    if (vkDev.IsFrameRecording()) {
        VulkanStagingAllocator& staging = vkDev.GetStagingAllocator();
        VulkanStagingAllocator::Allocation alloc = staging.Allocate(size, 256);
        if (!alloc.overflow) {
            std::memcpy(alloc.cpuPtr, data, size);
            staging.QueueBlit_Texture(alloc, vkImage_,
                                      region.mipLevel, region.baseArrayLayer,
                                      region.offsetX, region.offsetY, region.offsetZ,
                                      region.width, region.height, region.depth,
                                      /*bytesPerRow=*/0, /*bytesPerImage=*/0,
                                      /*currentLayout=*/layoutBefore,
                                      /*backLayout=*/layoutAfter,
                                      /*aspect=*/aspect,
                                      /*texelSize=*/static_cast<u32>(bpt));
            if (mipLayouts_.empty()) SetCurrentLayout(layoutAfter);
            else SetMipLayout(region.mipLevel, layoutAfter);
            return true;
        }
        staging.FlushBlocking();
    }

    // === 立即模式:一次性 staging + one-shot cmdbuf(资产加载期语义) ===
    VkBufferCreateInfo stagingCI{};
    stagingCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingCI.size = size;
    stagingCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo stagingACI{};
    stagingACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    stagingACI.usage = VMA_MEMORY_USAGE_AUTO;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    VmaAllocationInfo stagingInfo{};
    if (vmaCreateBuffer(allocator, &stagingCI, &stagingACI,
                        &stagingBuf, &stagingAlloc, &stagingInfo) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] updateRegion: staging vmaCreateBuffer failed" << std::endl;
        return false;
    }
    std::memcpy(stagingInfo.pMappedData, data, size);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &pci, nullptr, &pool) != VK_SUCCESS) {
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    ai.commandPool = pool;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &ai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto imageBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags srcAccess, VkPipelineStageFlags srcStage,
                            VkAccessFlags dstAccess, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = vkImage_;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = region.mipLevel;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = region.baseArrayLayer;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    imageBarrier(layoutBefore, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;   // 紧密行主序
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = aspect;
    copy.imageSubresource.mipLevel = region.mipLevel;
    copy.imageSubresource.baseArrayLayer = region.baseArrayLayer;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = {int32_t(region.offsetX), int32_t(region.offsetY), 0};
    copy.imageExtent = {region.width, region.height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, vkImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    imageBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layoutAfter,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);

    // VT M0.9: 带 fence 提交再 vkWaitForFences — validation layer 的后台
    // retire 线程在"无 fence submit + waitIdle + 立即 free transient pool"
    // 高频序列下存在内部竞争(layer 内 QueueSubState::Retire 崩溃,80 次/帧
    // 级上传可复现)。fence 信号强制 layer 同步完成 retire,再释放资源。
    VkFence submitFence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(vkDevice, &fci, nullptr, &submitFence) != VK_SUCCESS) {
        submitFence = VK_NULL_HANDLE;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vkDev.GetGraphicsQueue(), 1, &si, submitFence);
    if (submitFence != VK_NULL_HANDLE) {
        // R10 taskC：UINT64_MAX fence 等待在 device lost 后永不返回；
        // PRIMAL_VK_HANG_DIAG=1 时改为有界轮询，门控关闭时原调用不变。
        if (hangdiag::Enabled()) {
            hangdiag::NoteSignalFence(submitFence);
            hangdiag::BoundedWaitFence(vkDevice, "VulkanTexture.uploadFence", submitFence);
        } else {
            vkWaitForFences(vkDevice, 1, &submitFence, VK_TRUE, UINT64_MAX);
        }
    } else {
        hangdiag::ScopedHangWatch wait("queueWaitIdle", "VulkanTexture.upload");
        vkQueueWaitIdle(vkDev.GetGraphicsQueue());
    }

    vkFreeCommandBuffers(vkDevice, pool, 1, &cmd);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    if (submitFence != VK_NULL_HANDLE) {
        hangdiag::RetireFence(submitFence);
        vkDestroyFence(vkDevice, submitFence, nullptr);
    }
    vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

    if (mipLayouts_.empty()) SetCurrentLayout(layoutAfter);
    else SetMipLayout(region.mipLevel, layoutAfter);
    return true;
}

VkImageView VulkanTexture::GetArrayView() {
    // P4c-F5: 非 array 纹理的默认 view 已覆盖唯一 layer。
    if (texDesc_.arraySize <= 1 && texDesc_.type != TextureType::Texture2DArray) {
        return vkView_;
    }
    if (arrayView_ != VK_NULL_HANDLE) return arrayView_;

    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = vkImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = vkFormat_;
    vci.components = VkComponentMapping{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                         VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask = GetAspectMask();
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = texDesc_.mipLevels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = std::max<u32>(texDesc_.arraySize, 1u);

    if (vkCreateImageView(dev, &vci, nullptr, &arrayView_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanTexture] GetArrayView vkCreateImageView failed" << std::endl;
        arrayView_ = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return arrayView_;
}

void VulkanTexture::destroyImpl() {
    if (vkImage_ == VK_NULL_HANDLE && allocation_ == nullptr && vkView_ == VK_NULL_HANDLE) return;

    VkImage image = vkImage_;
    VkImageView view = vkView_;
    VmaAllocation alloc = allocation_;
    bool owned = ownsImage_;
    VmaAllocator allocator = static_cast<VulkanDevice&>(device_).GetVmaAllocator();
    VkDevice dev = static_cast<VulkanDevice&>(device_).GetNativeDevice();

    // Capture per-layer views so the deferred destroyer can release them too.
    std::vector<VkImageView> layerViewsToDestroy;
    layerViewsToDestroy.reserve(layerViews_.size() + 1);
    for (const auto& [k, v] : layerViews_) {
        if (v != VK_NULL_HANDLE) layerViewsToDestroy.push_back(v);
    }
    if (arrayView_ != VK_NULL_HANDLE) layerViewsToDestroy.push_back(arrayView_);

    device_.GetGarbageCollector().DeferredDestroy([dev, allocator, image, view, alloc, owned,
                                                   layerViewsToDestroy]() {
        if (dev != VK_NULL_HANDLE && view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
        for (VkImageView lv : layerViewsToDestroy) {
            if (lv != VK_NULL_HANDLE) vkDestroyImageView(dev, lv, nullptr);
        }
        // wrap 模式不释放 VkImage(swapchain 拥有)
        if (owned && allocator && image != VK_NULL_HANDLE && alloc) {
            vmaDestroyImage(allocator, image, alloc);
        }
    });

    vkImage_ = VK_NULL_HANDLE;
    vkView_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    ownsImage_ = true;
    wrappedImage_ = VK_NULL_HANDLE;
    currentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    mipLayouts_.clear();
    layerViews_.clear();
    arrayView_ = VK_NULL_HANDLE;
}

} // namespace primal::graphics::rhi
