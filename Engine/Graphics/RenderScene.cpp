#include "RenderScene.h"
#include <algorithm>

namespace primal::graphics {

void RenderScene::AddProxy(const RenderProxy& proxy) {
    std::lock_guard<std::mutex> lock(mutex_);
    proxies_.push_back(proxy);
}

void RenderScene::RemoveProxy(id::id_type entityId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (u64 i = 0; i < proxies_.size(); ++i) {
        if (proxies_[i].entityId == entityId) {
            proxies_.erase(i);
            return;
        }
    }
}

void RenderScene::UpdateProxy(id::id_type entityId, const RenderProxy& newProxy) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 线性查找并更新
    // 注意：如果 proxies_ 数量很大，这里需要优化（例如使用 map 索引或 spatial partitioning）
    for (auto& proxy : proxies_) {
        if (proxy.entityId == entityId) {
            proxy = newProxy;
            return;
        }
    }
    // If not found, add it to maintain consistency
    proxies_.push_back(newProxy);
}

void RenderScene::AddLight(const RenderLight& light) {
    std::lock_guard<std::mutex> lock(mutex_);
    lights_.push_back(light);
}

void RenderScene::RemoveLight(id::id_type entityId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (u64 i = 0; i < lights_.size(); ++i) {
        if (lights_[i].entityId == entityId) {
            lights_.erase(i);
            return;
        }
    }
}

void RenderScene::UpdateLight(id::id_type entityId, const RenderLight& newLight) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& light : lights_) {
        if (light.entityId == entityId) {
            light = newLight;
            return;
        }
    }
    lights_.push_back(newLight);
}

utl::vector<const RenderProxy*> RenderScene::Cull(const rhi::Frustum& frustum) const {
    utl::vector<const RenderProxy*> visibleProxies;
    Cull(frustum, visibleProxies);
    return visibleProxies;
}

void RenderScene::Cull(const rhi::Frustum& frustum, utl::vector<const RenderProxy*>& outProxies) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // 预估容量，避免频繁分配
    if (outProxies.capacity() < outProxies.size() + proxies_.size()) {
        outProxies.reserve(outProxies.size() + proxies_.size());
    }

    for (const auto& proxy : proxies_) {
        // 使用 RenderProxy 中存储的世界空间包围盒进行剔除
        if (proxy.worldAABB.IsValid()) {
            if (frustum.IsBoxVisible(proxy.worldAABB)) {
                outProxies.push_back(&proxy);
            }
        } else {
            // 如果 AABB 无效，保守起见认为可见，或者忽略（取决于策略）
            // 这里我们假设它是一个点，或者回退到简单的位置检查
            math::v3 position{proxy.transform.columns[3][0], proxy.transform.columns[3][1], proxy.transform.columns[3][2]};
            f32 radius = 1.0f; // 默认半径
            if (frustum.IsSphereVisible(position, radius)) {
                outProxies.push_back(&proxy);
            }
        }
    }
}

void RenderScene::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    proxies_.clear();
    lights_.clear();
}

} // namespace primal::graphics
