#pragma once
// RHICaptureBridge.h — 观测层（LumenCompareCapture / PassChainCapture）的
// 平台中立分派入口（D3D12 M0 F0-11）。
//
// 背景：capture 层此前直接 include VulkanDevice.h 并 static_cast
// （commit 810d4ae8 引入的 Vulkan 化调用点），使共享代码长出对特定后端的
// 硬依赖——违反 Master Plan §3 "无按后端的调用方代码分支"（F0-12 门禁
// 强制）。本头提供注册式桥：
//   - 共享 capture 头只 include 本头（无任何 Platforms/* 头）。
//   - 各后端在自己的 Platforms/<backend>/ 目录里实现 ResolveTextureFn 并
//     在静态初始化时注册（Vulkan: VulkanCaptureBridge.cpp；D3D12: M1 起同
//     模式提供）。未注册的后端 = 显式不支持（BackendCaptureSupported=false，
//     capture 层按既有语义跳过）。
//   - Vulkan 行为零变化：同一查询路径（GetTexture -> GetNativeImage ->
//     GetTextureDesc -> GetTextureSize），仅搬到后端目录内执行。
//
// 线程性：与 capture 层相同（主渲染线程调用）；注册发生在静态初始化。
#include "Graphics/RHI/Core/RHIDevice.h"
#include <array>
#include <cstdint>

namespace primal::graphics::rhicapture {

/// 纹理解析结果（后端无关投影；字段语义对齐旧 LumenCompareCapture 直查路径）。
struct TextureResolve {
    bool isTexture = false;      ///< 句柄在后端可解析为纹理
    bool hasNativeImage = false; ///< 有真实原生图像（旧路径 GetNativeImage()!=null）
    rhi::TextureDesc desc{};     ///< 纹理描述（format/size/mips/array）
    u32 width = 0;               ///< 有效宽（视图 mip 折算后）
    u32 height = 0;
};

/// 后端实现：把 ResourceHandle 解析为上述投影。返回 false = 不是纹理
/// （buffer 或未注册资源——调用方走 buffer 路径）。
using ResolveTextureFn = bool (*)(rhi::RHIDeviceBase&, rhi::ResourceHandle, TextureResolve&);

/// 每平台注册槽（下标 = RHIPlatform 值；Dawn/Metal/D3D12 未注册=nullptr）。
inline ResolveTextureFn& ResolverSlot(rhi::RHIPlatform platform) {
    // 函数局部 static + inline：ODR 安全的微型注册表（无 .cpp 依赖，
    // 未链接任何后端时 capture 层仍可编译）。
    static std::array<ResolveTextureFn, 8> slots{};
    return slots[static_cast<u8>(platform) & 7u];
}

inline void RegisterTextureResolver(rhi::RHIPlatform platform, ResolveTextureFn fn) {
    ResolverSlot(platform) = fn;
}

inline bool ResolveTexture(rhi::RHIDeviceBase& device, rhi::ResourceHandle handle,
                           TextureResolve& out) {
    if (auto* fn = ResolverSlot(device.GetPlatform())) return fn(device, handle, out);
    return false;
}

/// 该设备的后端是否支持纹理捕获观测（未注册 = 显式不支持）。
/// 替代旧代码的 `GetPlatform() != RHIPlatform::Vulkan` 门（语义不变：
/// 仅 Vulkan 注册；Metal/Dawn/D3D12 在各自实现落地前一律 false）。
inline bool BackendCaptureSupported(const rhi::RHIDeviceBase& device) {
    return ResolverSlot(device.GetPlatform()) != nullptr;
}

} // namespace primal::graphics::rhicapture
