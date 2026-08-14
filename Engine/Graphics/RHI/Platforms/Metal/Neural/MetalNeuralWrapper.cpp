/**
 * @file MetalNeuralWrapper.cpp
 * @brief MTL4 / 神经渲染接入占位实现
 * @details 仅做 runtime detect。
 *          GPUFamilyMetal4 = 5002 来自 macOS 26 SDK MTLDevice.hpp。
 *          不调用任何 MTL4 namespace API — 避免 strong-link 导致 macOS 14 加载失败。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-02
 * @version 0.1.0
 */

#include "MetalNeuralWrapper.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <iostream>

namespace primal::graphics::rhi {

// macOS 26 SDK 引入的 enum(值 5002)。在旧 SDK 下编译时 fallback 到字面量。
// 不能直接引用符号 GPUFamilyMetal4,因为 deployment target 14 的 SDK 可能没定义。
// 取而代之用字面量 5002 + static_assert 校验。
static constexpr MTL::GPUFamily kGPUFamilyMetal4 = static_cast<MTL::GPUFamily>(5002);

MetalNeuralWrapper::~MetalNeuralWrapper() {
    Shutdown();
}

MetalNeuralWrapper::Availability
MetalNeuralWrapper::Detect(MTL::Device* device) {
    Availability avail{};

    if (!device) {
        return avail;
    }

    // 1. macOS 主版本号(NSProcessInfo)
    NS::ProcessInfo* info = NS::ProcessInfo::processInfo();
    if (info) {
        NS::String* ver = info->operatingSystemVersionString();
        // operatingSystemVersionString 格式示例: "Version 14.5 (Build 23F79)"
        // 用 NSOperatingSystemVersion 更精确
        NS::OperatingSystemVersion osv = info->operatingSystemVersion();
        avail.macOSMajor = static_cast<u32>(osv.majorVersion);
    }

    // 2. device 是否声明 MTL4 支持
    // supportsFamily 在 macOS 10.15+ 可用,接受 GPUFamilyMetal4 需要 macOS 26 SDK 头。
    // 我们用字面量值绕过 SDK 版本检查,只在 runtime 查询。
    // metal-cpp 在不识别的 family 值上返回 false(底层 dispatch 不抛异常)。
    avail.mtl4Available = device->supportsFamily(kGPUFamilyMetal4);

    // 3. mlEncoder 占位 — 后续 MTL4 ML encoder 真实接入时填
    avail.mlEncoderAvailable = false;

    // 4. apiVersion 0=undetected / 4=MTL4 detect 成功
    if (avail.mtl4Available) {
        avail.apiVersion = 4;
    }

    return avail;
}

bool MetalNeuralWrapper::Initialize(MTL::Device* device) {
    if (!device) return false;

    device_     = device;  // 不 retain,MetalDevice 管理
    available_  = Detect(device);

    if (available_.mtl4Available) {
        std::cout << "[MetalNeuralWrapper] MTL4 available."
                  << " macOS=" << available_.macOSMajor
                  << " apiVersion=" << available_.apiVersion
                  << std::endl;
    } else {
        std::cout << "[MetalNeuralWrapper] MTL4 NOT available."
                  << " macOS=" << available_.macOSMajor
                  << " (requires macOS 26 + Metal 4 GPU)"
                  << std::endl;
    }
    return available_.mtl4Available;
}

void MetalNeuralWrapper::Shutdown() {
    // 占位 — 后续神经渲染项目在此释放 BinaryFunction/ArgumentTable 等
    device_    = nullptr;
    available_ = {};
}

} // namespace primal::graphics::rhi
