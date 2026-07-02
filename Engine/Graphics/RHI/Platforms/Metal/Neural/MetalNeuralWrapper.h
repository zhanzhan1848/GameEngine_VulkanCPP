/**
 * @file MetalNeuralWrapper.h
 * @brief MTL4 / 神经渲染接入占位
 * @details 仅做 runtime detect,不实际调用 MTL4 API。
 *          为后续神经渲染项目(L0 MetalFX Denoising / L1 HDRNet / L2 TensorOps)做基础设施准备。
 *
 * 接入策略:
 *   - macOS 26 SDK 引入 MTL4 namespace 与新 selector(如 supportsFamily(GPUFamilyMetal4))
 *   - 旧 runtime 通过 supportsFamily(GPUFamilyMetal4) 检测硬件 + driver 支持
 *   - 同时检查 macOS 主版本号 >= 26 作为 OS 端 gate
 *   - 任一不满足则 mtl4Available = false,wrapper 不参与渲染管线
 *
 * 注意:本 wrapper **不直接调用 MTL4 API**。strong-link 到 MTL4 符号会让 macOS 14
 * runtime 加载失败。所有 MTL4 API 使用留待后续神经渲染项目,届时用 dlsym / weak-link。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-02
 * @version 0.1.0
 */

#pragma once

#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommon.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief MTL4 / 神经渲染 wrapper(占位)
 * @details Detect-only:验证 MTL4 路径可走通,不实际接入渲染管线。
 */
class MetalNeuralWrapper {
public:
    /// 检测结果
    struct Availability {
        bool mtl4Available{false};       ///< device supportsFamily(GPUFamilyMetal4) 且 macOS >= 26
        bool mlEncoderAvailable{false};  ///< 后续填:MTL4 ML encoder 可用性(目前恒为 false)
        u32   apiVersion{0};             ///< 0 = undetected, 4 = MTL4 detect 成功
        u32   macOSMajor{0};             ///< NSProcessInfo major OS version
    };

public:
    MetalNeuralWrapper() = default;
    ~MetalNeuralWrapper();

    MetalNeuralWrapper(const MetalNeuralWrapper&) = delete;
    MetalNeuralWrapper& operator=(const MetalNeuralWrapper&) = delete;

    /**
     * @brief 静态检测 device 的 MTL4 可用性
     * @param device 非 null 的 MTL::Device 指针(不获取所有权)
     * @return Availability 结构体
     *
     * @details 纯查询,不持有任何资源。可独立调用,无需先 Initialize。
     *          GPUFamilyMetal4 = 5002 来自 macOS 26 SDK MTLDevice.hpp。
     */
    static Availability Detect(MTL::Device* device);

    /**
     * @brief 初始化 wrapper(占位 — 目前只缓存 detect 结果)
     * @param device 非 null 的 MTL::Device 指针(不获取所有权)
     * @return true 若 detect 成功且 mtl4Available == true
     *
     * @details 不分配任何 Metal 资源,不调用 MTL4 API。后续神经渲染项目
     *          会在此处创建 MTL4 BinaryFunction / ArgumentTable 等。
     */
    bool Initialize(MTL::Device* device);

    /**
     * @brief 关闭 wrapper(占位 — 释放未来 Initialize 持有的资源)
     */
    void Shutdown();

    /**
     * @brief wrapper 是否可用(已 Initialize 成功)
     */
    bool IsAvailable() const { return available_.mtl4Available; }

    /**
     * @brief 获取最近一次 Detect / Initialize 的 Availability 快照
     */
    const Availability& GetAvailability() const { return available_; }

private:
    MTL::Device*  device_{nullptr};  ///< 不持有(retain 由 MetalDevice 管理)
    Availability  available_{};
};

} // namespace primal::graphics::rhi
