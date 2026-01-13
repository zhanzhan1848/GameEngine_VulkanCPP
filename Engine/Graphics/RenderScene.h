#pragma once
#include "CommonHeaders.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include <vector>
#include <mutex>

namespace primal::graphics {

/**
 * @brief 渲染场景
 * @details 维护所有 RenderProxy 的空间结构，提供空间查询和剔除接口
 */
class RenderScene {
public:
    RenderScene() = default;
    ~RenderScene() = default;

    /**
     * @brief 添加渲染代理
     * @param proxy 渲染代理对象
     */
    void AddProxy(const RenderProxy& proxy);

    /**
     * @brief 移除渲染代理
     * @param entityId 关联的实体ID
     */
    void RemoveProxy(id::id_type entityId);

    /**
     * @brief 更新渲染代理
     * @param entityId 关联的实体ID
     * @param proxy 新的渲染代理数据
     */
    void UpdateProxy(id::id_type entityId, const RenderProxy& proxy);

    /**
     * @brief 视锥体剔除
     * @param frustum 视锥体
     * @return 可见的 RenderProxy 列表
     */
    std::vector<const RenderProxy*> Cull(const rhi::Frustum& frustum) const;

    /**
     * @brief 视锥体剔除（无分配版本）
     * @param frustum 视锥体
     * @param outProxies 输出的可见 RenderProxy 列表（会被追加）
     */
    void Cull(const rhi::Frustum& frustum, std::vector<const RenderProxy*>& outProxies) const;

    /**
     * @brief 获取所有渲染代理
     * @return 所有渲染代理的常量引用
     */
    const std::vector<RenderProxy>& GetProxies() const { return proxies_; }

    /**
     * @brief 清空场景
     */
    void Clear();

private:
    std::vector<RenderProxy> proxies_;      ///< 线性存储所有 RenderProxy
    mutable std::mutex mutex_;              ///< 线程安全互斥锁
};

} // namespace primal::graphics
