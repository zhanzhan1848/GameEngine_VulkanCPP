#pragma once

#include "../../Core/RHIRenderPass.h"
#include "MetalCommon.h"
#include "Utilities/Vector.h"

namespace primal::graphics::rhi {

class MetalDevice;
class MetalTexture;

/**
 * @brief Metal 渲染通道实现
 */
class MetalRenderPass : public RHIRenderPass {
public:
    MetalRenderPass(MetalDevice& device, const RenderPassDesc& desc);
    ~MetalRenderPass() override;

    bool Initialize() override;

    /**
     * @brief 获取 Metal 渲染通道描述符
     * @details 如果底层纹理发生变化，会自动更新描述符
     */
    MTL::RenderPassDescriptor* GetNativeRenderPassDescriptor();

protected:
    void destroyImpl() override;

    void* mapImpl(u64 /*offset*/, u64 /*size*/) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* /*data*/, u64 /*size*/, u64 /*offset*/) override { return false; }

private:
    /**
     * @brief 构建 Metal 描述符
     */
    void buildDescriptor();

    /**
     * @brief 检查附件是否有效（底层纹理是否发生变化）
     */
    bool isDirty() const;

    MTL::RenderPassDescriptor* mtlPassDesc_{nullptr};
    
    // 缓存的纹理指针，用于检测底层纹理变化
    struct CachedAttachment {
        ResourceHandle handle;
        MTL::Texture* nativeTexture;
    };
    
    utl::vector<CachedAttachment> cachedColorAttachments_;
    CachedAttachment cachedDepthAttachment_;
    CachedAttachment cachedStencilAttachment_;
};

} // namespace primal::graphics::rhi
