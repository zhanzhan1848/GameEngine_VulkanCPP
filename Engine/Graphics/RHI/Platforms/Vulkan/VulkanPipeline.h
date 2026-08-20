/**
 * @file VulkanPipeline.h
 * @brief Vulkan RHI Pipeline 实现(graphics + compute)
 * @details Phase 4: GraphicsPipelineDesc/ComputePipelineDesc → VkPipeline。
 *          独立 class(非 RHIResource),持 pipeline + pipelineLayout 缓存 + 1 套 VkRenderPass 兼容性。
 *          Recreate() 用于 Phase 5+ 的 ReloadShader 重建。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIDevice.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanPipeline {
    friend class VulkanDevice;
    friend class VulkanCommandBuffer;
public:
    explicit VulkanPipeline(VulkanDevice& device);
    VulkanPipeline(VulkanPipeline&& other) noexcept;
    VulkanPipeline& operator=(VulkanPipeline&& other) noexcept;
    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;
    ~VulkanPipeline();

    bool Initialize(const GraphicsPipelineDesc& desc);
    bool Initialize(const ComputePipelineDesc& desc);
    void Destroy();

    /// 热重载(Phase 5+ 使用)
    bool Recreate();

    VkPipeline       GetNativePipeline() const { return pipeline_; }
    VkPipelineLayout GetNativePipelineLayout() const;
    VkPipelineBindPoint GetBindPoint() const {
        return isCompute_ ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    }

    bool IsCompute() const { return isCompute_; }
    PipelineHandle GetHandle() const { return handle_; }
    void SetHandle(PipelineHandle h) { handle_ = h; }

    const GraphicsPipelineDesc& GetGraphicsDesc() const { return graphicsDesc_; }

private:
    VulkanDevice& device_;
    PipelineHandle handle_{handles::INVALID_PIPELINE};

    bool isCompute_{false};
    GraphicsPipelineDesc graphicsDesc_{};
    ComputePipelineDesc  computeDesc_{};

    VkPipeline       pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout ownedLayout_{VK_NULL_HANDLE};  ///< fallback layout(无 desc.layout 时创建)
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
