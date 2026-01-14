#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"

namespace primal::graphics {

/**
 * @brief 前向渲染器
 * @details 实现标准的前向渲染管线，包括 Z-Prepass, Opaque Pass, Transparent Pass
 */
class ForwardRenderer {
public:
    ForwardRenderer();
    ~ForwardRenderer();

    /**
     * @brief 初始化渲染器
     * @param device RHI设备指针
     * @return 是否初始化成功
     */
    bool Initialize(rhi::RHIDeviceBase* device);

    /**
     * @brief 关闭渲染器
     */
    void Shutdown();

    /**
     * @brief 执行渲染
     * @param cmdBuffer 命令缓冲区
     * @param scene 渲染场景
     * @param view 渲染视图
     * @param renderTarget 渲染目标纹理句柄
     * @param depthStencil 深度模板纹理句柄
     * @param materials 材质实例映射表
     */
    void Render(rhi::RHICommandBuffer* cmdBuffer, 
                const RenderScene& scene, 
                const RenderView& view, 
                rhi::ResourceHandle renderTarget, 
                rhi::ResourceHandle depthStencil,
                const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                uint32_t frameIndex,
                uint32_t width,
                uint32_t height);

private:
    /**
     * @brief 深度预通过 Pass
     * @details 只写入深度缓冲区，用于减少 Overdraw
     */
    void DepthPrePass(rhi::RHICommandBuffer* cmdBuffer, 
                      const RenderView& view, 
                      rhi::ResourceHandle depthStencil,
                      const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                      const utl::vector<const RenderProxy*>& proxies,
                      uint32_t frameIndex,
                      uint32_t width,
                      uint32_t height);

    /**
     * @brief 不透明物体绘制 Pass
     * @details 绘制所有不透明物体
     */
    void OpaquePass(rhi::RHICommandBuffer* cmdBuffer, 
                    const RenderView& view, 
                    const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                    const utl::vector<const RenderProxy*>& proxies,
                    uint32_t frameIndex);

    /**
     * @brief 透明物体绘制 Pass
     * @details 绘制所有透明物体（需排序）
     */
    void TransparentPass(rhi::RHICommandBuffer* cmdBuffer, 
                         const RenderView& view, 
                         const std::unordered_map<id::id_type, class MaterialInstance*>& materials,
                         const utl::vector<const RenderProxy*>& proxies,
                         uint32_t frameIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    
    // 缓存的 RenderPass 句柄，如果配置没变可以复用
    rhi::RenderPassHandle depthPrePass_{rhi::handles::INVALID_RESOURCE};
    rhi::RenderPassHandle opaquePass_{rhi::handles::INVALID_RESOURCE};
    rhi::RenderPassHandle transparentPass_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
