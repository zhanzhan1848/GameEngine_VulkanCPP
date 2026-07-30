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
#endif

#include <iostream>
#include <cstring>

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
        std::cerr << "[VulkanTexture] device not initialized" << std::endl;
        return false;
    }
    if (vkFormat_ == VK_FORMAT_UNDEFINED) {
        std::cerr << "[VulkanTexture] DataFormat not mapped to VkFormat" << std::endl;
        return false;
    }

    // === Phase 5 view 模式:alias source VkImage + 创建受限 subresourceRange view ===
    // 不拥有 VkImage(viewSrc_ 拥有),只创建+拥有 VkImageView。
    if (isView_) {
        if (!viewSrc_ || viewSrc_->vkImage_ == VK_NULL_HANDLE) {
            std::cerr << "[VulkanTexture] view mode but source image is null" << std::endl;
            return false;
        }
        vkImage_ = viewSrc_->vkImage_;  // 别名,不拥有

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = vkImage_;
        // viewType 推导:2D/3D/Cube 直接映射,Unknown 兜底 2D。
        switch (viewDesc_.viewType) {
            case TextureType::Texture3D:
                vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            case TextureType::TextureCube:
                vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
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
            (vkFormat_ == VK_FORMAT_D32_SFLOAT || vkFormat_ == VK_FORMAT_D24_UNORM_S8_UINT ||
             vkFormat_ == VK_FORMAT_D32_SFLOAT_S8_UINT)
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = viewDesc_.mostDetailedMip;
        vci.subresourceRange.levelCount   = std::max<u32>(1u, viewDesc_.mipCount);
        vci.subresourceRange.baseArrayLayer = viewDesc_.firstArraySlice;
        vci.subresourceRange.layerCount   = std::max<u32>(1u, viewDesc_.arraySize);

        if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
            std::cerr << "[VulkanTexture] view: vkCreateImageView failed" << std::endl;
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
            std::cerr << "[VulkanTexture] wrap mode but wrappedImage_ is null" << std::endl;
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
            (vkFormat_ == VK_FORMAT_D32_SFLOAT || vkFormat_ == VK_FORMAT_D24_UNORM_S8_UINT ||
             vkFormat_ == VK_FORMAT_D32_SFLOAT_S8_UINT)
            ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = std::max<u32>(1u, static_cast<u32>(mipLayouts_.size()));
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;

        if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
            std::cerr << "[VulkanTexture] wrap: vkCreateImageView failed" << std::endl;
            vkImage_ = VK_NULL_HANDLE;
            return false;
        }
        state_ = ResourceState::Ready;
        return true;
    }

    // === 常规模式:VMA 分配 VkImage ===
    VmaAllocator allocator = vkDevice.GetVmaAllocator();
    if (!allocator) {
        std::cerr << "[VulkanTexture] VMA allocator not initialized" << std::endl;
        return false;
    }
    if (vkUsageFlags_ == 0) {
        std::cerr << "[VulkanTexture] usage=Unknown rejected" << std::endl;
        return false;
    }

    // === VkImageCreateInfo ===
    const bool isCube = (texDesc_.type == TextureType::TextureCube);
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // Cube textures must declare the CUBE_COMPATIBLE bit so layers can be
    // interpreted as +X/-X/+Y/-Y/+Z/-Z faces when bound to a CUBE view.
    ici.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    // TextureType 在 RHIResource 基类被压平成 ResourceType::Texture;从 extent 推断:
    //   z > 1 → 3D,否则 2D(Phase 3 测试范围只用 2D;Cube/1D 由 Phase 5 扩展)
    ici.imageType = (texDesc_.size.z > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = vkFormat_;
    ici.extent.width  = std::max<u32>(1u, texDesc_.size.x);
    ici.extent.height = std::max<u32>(1u, texDesc_.size.y);
    ici.extent.depth  = std::max<u32>(1u, texDesc_.size.z);
    ici.mipLevels = std::max<u32>(1u, static_cast<u32>(mipLayouts_.size()));
    // Cube textures always have exactly 6 faces (one per array layer).
    // Otherwise use the caller-specified arraySize (1 for non-array, N for 2D-array).
    ici.arrayLayers = isCube ? 6u : std::max<u32>(1u, texDesc_.arraySize);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = vkUsageFlags_;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.flags = 0;
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;  // Texture 默认 GPU-only
    aci.memoryTypeBits = 0;
    aci.pool = nullptr;
    aci.pUserData = nullptr;
    aci.priority = 0.5f;

    VmaAllocationInfo info{};
    VkResult res = vmaCreateImage(allocator, &ici, &aci, &vkImage_, &allocation_, &info);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanTexture] vmaCreateImage failed: " << res
                  << " w=" << ici.extent.width << " h=" << ici.extent.height
                  << " fmt=" << static_cast<int>(vkFormat_) << std::endl;
        return false;
    }

    // === Default VkImageView ===
    // Choose viewType by TextureType:
    //   - TextureCube        → VK_IMAGE_VIEW_TYPE_CUBE       (6 faces, sampler uses direction)
    //   - arraySize > 1      → VK_IMAGE_VIEW_TYPE_2D_ARRAY   (SPIR-V texture_2d_array / storage_2d_array)
    //   - else (single)      → VK_IMAGE_VIEW_TYPE_2D         (SPIR-V texture_2d / storage_2d)
    // Storage cube is not supported in Vulkan core; engine cube-storage paths use 2D-array.
    VkImageViewType viewType;
    u32 viewLayerCount;
    if (isCube) {
        viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        viewLayerCount = 6u;
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
        (vkFormat_ == VK_FORMAT_D32_SFLOAT || vkFormat_ == VK_FORMAT_D24_UNORM_S8_UINT ||
         vkFormat_ == VK_FORMAT_D32_SFLOAT_S8_UINT)
        ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = ici.mipLevels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = viewLayerCount;

    if (vkCreateImageView(dev, &vci, nullptr, &vkView_) != VK_SUCCESS) {
        std::cerr << "[VulkanTexture] vkCreateImageView failed" << std::endl;
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
        std::cerr << "[VulkanTexture] GetLayerView vkCreateImageView failed for layer "
                  << layer << std::endl;
        return VK_NULL_HANDLE;
    }
    layerViews_[layer] = view;
    return view;
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
    layerViewsToDestroy.reserve(layerViews_.size());
    for (const auto& [k, v] : layerViews_) {
        if (v != VK_NULL_HANDLE) layerViewsToDestroy.push_back(v);
    }

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
}

} // namespace primal::graphics::rhi
