/**
 * @file VulkanTexture.h
 * @brief Vulkan RHI 纹理实现
 * @details Phase 3 范围:VkImage + VmaAllocation + default VkImageView + VkImageLayout 跟踪。
 *          支持被 VulkanCommandBuffer 的 CopyBufferToTexture / BlitTexture / GenerateMipmaps 使用。
 *          Layout 跟踪是 CommandBuffer 隐式 transition 的关键(Metal 没有这层概念)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "VulkanMath.h"
#include "../../Core/RHIResource.h"

#include <unordered_map>

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanTexture : public RHIResource {
    friend class VulkanDevice;
    friend class VulkanCommandBuffer;
    friend class VulkanSwapChain;
public:
    /// 常规构造 — VMA 分配新 VkImage,Initialize() 走 vmaCreateImage 路径。
    VulkanTexture(VulkanDevice& device, const TextureDesc& desc);

    /// Phase 4b wrap 构造 — 包裹一个外部已存在的 VkImage(例如 swapchain image)。
    /// 不拥有 VkImage(由 caller 拥有),但自己创建并拥有 VkImageView。
    /// Initialize() 会跳过 vmaCreateImage,只创建 view。
    VulkanTexture(VulkanDevice& device, const TextureDesc& desc, VkImage existingImage);

    /// Phase 5 view 构造 — 由 createTextureViewImpl 调用。
    /// 包裹 (src.vkImage_, per-mip/layer VkImageView),不拥有 VkImage 但拥有 VkImageView。
    /// Initialize() 走 InitializeAsView 路径:alias source image,创建受限 subresourceRange 的 view。
    VulkanTexture(VulkanDevice& device, const TextureViewDesc& viewDesc, VulkanTexture& src);

    VulkanTexture(VulkanTexture&& other) noexcept;
    VulkanTexture& operator=(VulkanTexture&& other) noexcept;
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    virtual ~VulkanTexture();

    bool Initialize() override;

    VkImage GetNativeImage() const { return vkImage_; }
    VkImageView GetNativeView() const { return vkView_; }
    VmaAllocation GetAllocation() const { return allocation_; }
    bool OwnsImage() const { return ownsImage_; }

    /// 获取 array texture 的某层 view(layer=N),viewType=2D,baseArrayLayer=N,layerCount=1。
    /// 用于 BeginRenderPass 把 render target 绑到特定 cascade layer(Metal 用 setSlice 实现同等语义)。
    /// 默认 vkView_ 只覆盖 layer 0;调用 GetLayerView(N>=1) 会 lazy-create 一个 layer-specific view。
    /// 单层 texture (arraySize<=1) 直接返回 vkView_,跳过 cache 查找。
    VkImageView GetLayerView(u32 layer);

    /// P4c-F5: 获取覆盖全部 layer+mip 的 2D_ARRAY 视图(layered 渲染用,
    /// 对应 Metal 的 renderTargetArrayLength 路径)。lazy 缓存,复用
    /// GetLayerView 的模式;非 array 纹理直接返回 vkView_。
    VkImageView GetArrayView();

    /// 原始 TextureDesc(包含 width/height/depth/mipLevels 等真实几何,
    /// RHIResource 基类的 ResourceDesc::size 是 u64 字节数,无法承载这些信息)
    const TextureDesc& GetTextureDesc() const { return texDesc_; }

    /// 当前 image layout — barrier 计算需要它。
    /// For multi-mip textures (mipLevels > 1), returns mip 0's layout from
    /// mipLayouts_ (managed by TransitionImageLayout). For single-mip, uses
    /// the global currentLayout_ set by InsertBarrier/BeginRenderPass.
    VkImageLayout GetCurrentLayout() const {
        if (!mipLayouts_.empty()) {
            return mipLayouts_[0] != VK_IMAGE_LAYOUT_UNDEFINED ? mipLayouts_[0] : currentLayout_;
        }
        return currentLayout_;
    }
    void SetCurrentLayout(VkImageLayout l) {
        currentLayout_ = l;
        // Keep mipLayouts_ in sync — InsertBarrier uses this for oldLayout.
        if (!mipLayouts_.empty()) {
            for (auto& ml : mipLayouts_) ml = l;
        }
    }

    /// 当前 mip layout 数组(GenerateMipmaps 用)
    VkImageLayout GetMipLayout(u32 mip) const {
        return mip < mipLayouts_.size() ? mipLayouts_[mip] : currentLayout_;
    }
    void SetMipLayout(u32 mip, VkImageLayout l) {
        if (mip < mipLayouts_.size()) mipLayouts_[mip] = l;
    }

    /// 该纹理应使用的 image aspect mask — 深度格式(DEPTH 或 DEPTH+STENCIL)返回
    /// `VK_IMAGE_ASPECT_DEPTH_BIT`,其它返回 `VK_IMAGE_ASPECT_COLOR_BIT`。
    /// 给 VulkanCommandBuffer 的 copy/blit/barrier 路径使用,替代旧的硬编码 COLOR_BIT。
    /// (与 Initialize() 中 VkImageView 的 aspectMask 计算保持一致 — 见 VulkanTexture.cpp:185,258。)
    VkImageAspectFlags GetAspectMask() const {
        return vulkan::IsDepthVkFormat(vkFormat_)
                   ? VK_IMAGE_ASPECT_DEPTH_BIT
                   : VK_IMAGE_ASPECT_COLOR_BIT;
    }

protected:
    void destroyImpl() override;

    /// P4c-F3: 仅 GPUMemoryUsage::Staging/Readback(VMA HOST_VISIBLE 持久映射)
    /// 返回映射指针;DEVICE_LOCAL 返回 nullptr 并打一次 warn。
    /// 与 Metal 的行为差异:Metal ReplaceRegion 本质也是内核 staging,
    /// Vulkan 暴露为 updateData 而非 map(见 README 限制表)。
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;

    /// P4c-F3: 立即模式纹理上传(资产加载期,无帧上下文)。
    /// 语义:data 为 mip0/slice0 的紧密行主序 blob;
    ///   - offset=0 且 size=整图 → 全图更新;
    ///   - offset 非 0 → 行粒度子矩形(须满足 offset % bytesPerRow == 0,
    ///     size 为 bytesPerRow 整数倍,覆盖 [offset/bytesPerRow, +rows) 行)。
    /// 布局由 RHI 内部闭合:copy 后 barrier 回 currentLayout_(UNDEFINED 时
    /// 提升为 SHADER_READ_ONLY),调用方无需手动 barrier(与 Metal 对齐)。
    /// 帧内队列化路径(QueueBlit_Texture)属 F4;BC 压缩格式暂不支持
    /// (走 CopyBufferToTexture 命令路径)。
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    VkImage          vkImage_{VK_NULL_HANDLE};
    VkImageView      vkView_{VK_NULL_HANDLE};
    VmaAllocation    allocation_{nullptr};

    /// P4c-F3: Staging/Readback 内存用途的持久映射指针(VMA MAPPED)。
    void*            mappedPtr_{nullptr};
    /// P4c-F3: DEVICE_LOCAL map 拒绝只 warn 一次。
    bool             warnedMapUnsupported_{false};

    /// Per-layer ImageView cache for array textures (key = array layer index).
    /// Lazily populated by GetLayerView(). Single-layer textures never touch this.
    std::unordered_map<u32, VkImageView> layerViews_;

    /// P4c-F5: layered 渲染的 2D_ARRAY 全层视图(lazy)。destroyImpl 随
    /// layerViews_ 一起延迟销毁。
    VkImageView     arrayView_{VK_NULL_HANDLE};

    VkImageUsageFlags vkUsageFlags_{0};  // 构造时从 TextureDesc 缓存
    VkFormat         vkFormat_{VK_FORMAT_UNDEFINED};

    bool             ownsImage_{true};   /// false = 外部拥有 VkImage(swapchain wrap 模式)
    VkImage          wrappedImage_{VK_NULL_HANDLE};  /// wrap 模式下的外部 image(Initialize 用)

    /// Phase 5 view 模式标志 — true = 由 createTextureViewImpl 创建,Initialize
    /// 走 InitializeAsView 路径(alias src image,创建受限 subresourceRange view)。
    bool             isView_{false};
    TextureViewDesc  viewDesc_{};
    VulkanTexture*   viewSrc_{nullptr};

    VkImageLayout    currentLayout_{VK_IMAGE_LAYOUT_UNDEFINED};
    std::vector<VkImageLayout> mipLayouts_;  // GenerateMipmaps 需要 per-mip

    TextureDesc      texDesc_;  // 完整保留原始 desc(width/height/depth/mipLevels/format 等)
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
