/**
 * @file RHIDeviceFactory.h
 * @brief RHI 设备被动分发器
 * @details 根据 DeviceDesc.platform 将枚举值映射到具体的 RHIDevice 子类。
 *          不做 OS 检测、不做 fallback —— 平台决策由 UI 层 (Editor) 通过
 *          DeviceDesc.platform 显式指定，本工厂只做被动映射。
 * @author GameEngine VulkanCPP Team
 * @date 2026-06-16
 * @version 0.1.0
 */

#pragma once

#include "RHIDevice.h"

namespace primal::graphics::rhi {

/**
 * @brief 创建 RHI 设备
 * @param desc 设备描述符 (含 platform 字段，由 UI 层决策)
 * @return 设备指针，失败返回 nullptr
 *
 * 调用方负责通过 DestroyRHIDevice 销毁。
 */
RHIDeviceBase* CreateRHIDevice(const DeviceDesc& desc);

/**
 * @brief 销毁 RHI 设备
 * @param device 设备指针 (可为 nullptr)
 */
void DestroyRHIDevice(RHIDeviceBase* device);

} // namespace primal::graphics::rhi
