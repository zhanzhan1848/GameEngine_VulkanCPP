/**
 * @file VulkanCaptureBridge.cpp
 * @brief RHICaptureBridge 的 Vulkan 特化（F0-11）
 * @details 后端目录内的 static_cast 合法域（F0-12 门禁白名单不适用于
 *          Platforms/* 自身）。查询路径与旧 LumenCompareCapture 直查完全
 *          一致（GetTexture -> GetNativeImage -> GetTextureDesc ->
 *          GetTextureSize），仅搬到本目录执行——Vulkan 行为零变化。
 */

#include "Graphics/RenderGraph/RHICaptureBridge.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "VulkanDevice.h"
#include "VulkanTexture.h"
#include "VulkanBackendLinks.h"

namespace primal::graphics::rhicapture {

namespace {

bool ResolveTextureVulkan(rhi::RHIDeviceBase& base, rhi::ResourceHandle handle,
                          TextureResolve& out) {
    if (base.GetPlatform() != rhi::RHIPlatform::Vulkan) return false;
    auto& device = static_cast<rhi::VulkanDevice&>(base);
    auto* texture = device.GetTexture(handle);
    if (!texture) {
        // 未解析为纹理：调用方按 buffer 路径处理（GetBufferByteSize 虚接口）。
        return false;
    }
    out.isTexture = true;
    out.hasNativeImage = texture->GetNativeImage() != VK_NULL_HANDLE;
    out.desc = texture->GetTextureDesc();
    device.GetTextureSize(handle, out.width, out.height);
    return true;
}

// 静态注册：capture 层经 RHICaptureBridge 分派，无需 include 后端头。
const bool kVulkanTextureResolverRegistered =
    (RegisterTextureResolver(rhi::RHIPlatform::Vulkan, &ResolveTextureVulkan), true);

} // namespace

} // namespace primal::graphics::rhicapture

namespace primal::graphics::rhi {

/// 链接锚点（见 VulkanBackendLinks.h）：静态注册对象无符号引用时会被
/// 链接器从静态库裁剪，注册静默丢失（2026-11-03 实测）。引用本 TU 的
/// 静态对象即可强制链接器拉入整个翻译单元。幂等。
void EnsureVulkanCaptureBridgeLinked() {
    (void)rhicapture::kVulkanTextureResolverRegistered;
}

} // namespace primal::graphics::rhi

#endif // defined(ENABLE_VULKAN) && ENABLE_VULKAN
