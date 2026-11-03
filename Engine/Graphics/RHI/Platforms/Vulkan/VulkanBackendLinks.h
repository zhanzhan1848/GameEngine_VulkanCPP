#pragma once
// VulkanBackendLinks.h — 后端注册对象的链接锚点（F0-11 补丁）
//
// 背景：VulkanCaptureBridge / VulkanPassTimestampEntry 通过静态对象在静态
// 初始化时向中立注册表（RHICaptureBridge.h / RHIPassTimestampAudit.h）登记。
// 但静态库成员若无任何被引用符号，链接器不会把它拉入最终二进制——注册
// 静默丢失，GetEntry()/BackendCaptureSupported() 恒为空（2026-11-03 实测：
// libEngine.a 含 17 个该 TU 符号，Sponza 二进制 0 个）。
//
// 修法：本头声明两个锚点函数（定义在各注册 .cpp 内），由
// VulkanDevice::initializeImpl 显式调用——VulkanDevice 必然被链接，锚点
// 符号引用强制拉入注册 TU。调用幂等（注册覆盖同指针）。
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace primal::graphics::rhi {

/// 强制拉入 VulkanCaptureBridge.cpp（RHICaptureBridge 纹理解析特化注册）。
void EnsureVulkanCaptureBridgeLinked();

/// 强制拉入 VulkanPassTimestampEntry.cpp（pass timestamp 审计入口注册）。
namespace pass_timestamp {
void EnsureVulkanPassTimestampEntryLinked();
} // namespace pass_timestamp

} // namespace primal::graphics::rhi

#endif // defined(ENABLE_VULKAN) && ENABLE_VULKAN
