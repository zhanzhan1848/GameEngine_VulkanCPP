#pragma once

#include "RHIResource.h"
#include "RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief 渲染通道抽象基类
 * @details 封装了渲染通道的描述符和平台相关的实现（如 Metal 的 MTLRenderPassDescriptor）
 *          通过显式的 RenderPass 对象，可以优化描述符的创建和复用，特别是在多线程环境下。
 */
class RHIRenderPass : public RHIResource {
public:
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param desc 渲染通道描述符
     */
    RHIRenderPass(RHIDeviceBase& device, const RenderPassDesc& desc)
        : RHIResource(device, ResourceDesc(ResourceType::RenderPass, ResourceUsage::None, GPUMemoryUsage::Unknown, 0)), desc_(desc) {
        // RenderPassDesc 不直接对应 ResourceDesc
    }

    virtual ~RHIRenderPass() = default;

    /**
     * @brief 获取渲染通道描述符
     */
    const RenderPassDesc& GetDesc() const { return desc_; }

    /**
     * @brief 获取颜色附件数量
     */
    uint32_t GetColorAttachmentCount() const { return static_cast<uint32_t>(desc_.colorAttachments.size()); }

    /**
     * @brief 检查是否有深度附件
     */
    bool HasDepthAttachment() const { return desc_.depthAttachment.texture != handles::INVALID_RESOURCE; }

    /**
     * @brief 检查是否有模板附件
     */
    bool HasStencilAttachment() const { return desc_.stencilAttachment.texture != handles::INVALID_RESOURCE; }

protected:
    RenderPassDesc desc_;
};

} // namespace primal::graphics::rhi
