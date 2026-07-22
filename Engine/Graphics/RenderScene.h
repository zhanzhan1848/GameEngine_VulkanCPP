#pragma once
#include "CommonHeaders.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include <vector>
#include <mutex>
#include <functional>

namespace primal::graphics {

struct StreamingMesh;  // defined in Graphics/RenderPipeline/StreamingMesh.h

enum class LightType {
    Directional,
    Point,
    Spot,
    Area // Reserved
};

struct RenderLight {
    id::id_type entityId{id::invalid_id};
    LightType type{LightType::Directional};
    
    math::v3 color{1.f, 1.f, 1.f};     // Linear color
    f32 intensity{1.f};                // Lux for Directional, Lumen for Point/Spot
    
    math::v3 position{0.f, 0.f, 0.f};  // World space position
    f32 range{10.f};                   // Attenuation range (for Point/Spot)
    
    math::v3 direction{0.f, -1.f, 0.f}; // Light direction
    f32 innerCone{0.9f};               // Cos(theta)
    f32 outerCone{0.8f};               // Cos(theta)
    
    bool castShadow{false};
    f32 shadowBias{0.005f};
};

// --- Streaming Mesh Record (Phase 9.3b) ---
// Bridges GlobalSDFMeshNode (PCG side) and GPUDrivenDrawPipeline (draw side).
// Tombstone lifecycle: when a node dies mid-frame we cannot remove the record
// immediately (the GPU may still be iterating the list). We mark tombstoned=true
// and let ClearTombstonedStreamingMeshes() erase it at frame boundary after GPU
// work is finished. `mesh` is node-owned; RenderScene does not free it.
struct StreamingMeshRecord {
    id::id_type entity_id{id::invalid_id};
    StreamingMesh* mesh{nullptr};
    // Triple-buffer slot (0..MAX_FRAMES_IN_FLIGHT-1). Producer writes slot N
    // in frame N; consumer reads matching slot N in the same frame. By the
    // time CPU reuses slot N (3 frames later), GPU work from frame N is
    // guaranteed complete — no cross-frame read-write race on the buffer set.
    u32   slot{0};
    math::v3 bounds_min{};
    math::v3 bounds_max{};
    bool visible{true};
    bool tombstoned{false};                        // pending removal at frame end
    u64  last_drawn_generation{0};
};

/**
 * @brief Render Scene
 * @details Manages all RenderProxies and Lights, provides spatial query and culling interfaces
 */
class RenderScene {
public:
    RenderScene() = default;
    ~RenderScene() = default;

    /**
     * @brief Add a Render Proxy
     * @param proxy Render Proxy Object
     */
    void AddProxy(const RenderProxy& proxy);

    /**
     * @brief 移除渲染代理
     * @param entityId 关联的实体ID
     */
    void RemoveProxy(id::id_type entityId);

    /**
     * @brief Update Render Proxy
     * @param entityId Entity ID
     * @param proxy New Render Proxy Data
     */
    void UpdateProxy(id::id_type entityId, const RenderProxy& proxy);

    // --- Light Management ---

    /**
     * @brief Add a Light
     * @param light Render Light Object
     */
    void AddLight(const RenderLight& light);

    /**
     * @brief Remove a Light
     * @param entityId Entity ID
     */
    void RemoveLight(id::id_type entityId);

    /**
     * @brief Update a Light
     * @param entityId Entity ID
     * @param light New Render Light Data
     */
    void UpdateLight(id::id_type entityId, const RenderLight& light);

    /**
     * @brief 清空所有光源（保留 proxies）
     * @details 用于每帧 ECS Light 同步前清空旧状态。
     *          与 Clear() 不同，只清 lights_，不动 proxies_ 和 reflectionPlanes_。
     */
    void ClearLights();

    // --- Reflection Plane Management ---

    struct RenderReflectionPlane {
        id::id_type entityId{id::invalid_id};
        math::v3 position;
        math::v3 normal;
        math::v2 size;
        f32 bias{0.0f};
    };

    void AddReflectionPlane(const RenderReflectionPlane& plane);
    void RemoveReflectionPlane(id::id_type entityId);
    void UpdateReflectionPlane(id::id_type entityId, const RenderReflectionPlane& plane);
    const utl::vector<RenderReflectionPlane>& GetReflectionPlanes() const { return reflectionPlanes_; }

    /**
     * @brief Get all lights
     * @return Const reference to all lights
     */
    const utl::vector<RenderLight>& GetLights() const { return lights_; }

    /**
     * @brief Frustum Culling
     * @param frustum 视锥体
     * @return 可见的 RenderProxy 列表
     */
    utl::vector<const RenderProxy*> Cull(const rhi::Frustum& frustum) const;

    /**
     * @brief 视锥体剔除（无分配版本）
     * @param frustum 视锥体
     * @param outProxies 输出的可见 RenderProxy 列表（会被追加）
     */
    void Cull(const rhi::Frustum& frustum, utl::vector<const RenderProxy*>& outProxies) const;

    /**
     * @brief 获取所有渲染代理
     * @return 所有渲染代理的常量引用
     */
    const utl::vector<RenderProxy>& GetProxies() const { return proxies_; }

    /**
     * @brief 清空场景
     */
    void Clear();

    // --- Streaming Mesh Management (Phase 9.3b) ---
    // slot: triple-buffer index (0..MAX_FRAMES_IN_FLIGHT-1). Default 0 keeps
    // C ABI clients (single-buffered) working without changes.
    id::id_type RegisterStreamingMesh(StreamingMesh* mesh, u32 slot,
                                      const math::v3& bounds_min,
                                      const math::v3& bounds_max);
    void UpdateStreamingMesh(id::id_type entity_id, u64 generation,
                             const math::v3& bounds_min, const math::v3& bounds_max);
    void UnregisterStreamingMesh(id::id_type entity_id);
    void ClearTombstonedStreamingMeshes();   // call at frame boundary after GPU work

    // --- Thread-safe streaming mesh iteration ---
    // GetStreamingMeshes() returns a const ref WITHOUT holding mutex_. Callers
    // that iterate while PCG node threads may mutate the list must use
    // ForEachStreamingMesh instead — it holds mutex_ for the full iteration so
    // a concurrent push_back / erase_unordered cannot invalidate the reference.
    void ForEachStreamingMesh(const std::function<void(const StreamingMeshRecord&)>& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& sm : streaming_meshes_) {
            fn(sm);
        }
    }
    const utl::vector<StreamingMeshRecord>& GetStreamingMeshes() const { return streaming_meshes_; }

private:
    utl::vector<RenderProxy> proxies_;      ///< Store all RenderProxies linearly
    utl::vector<RenderLight> lights_;       ///< Store all RenderLights linearly
    utl::vector<RenderReflectionPlane> reflectionPlanes_;
    utl::vector<StreamingMeshRecord> streaming_meshes_;
    id::id_type next_streaming_entity_id_{1}; ///< Monotonic entity id for streaming meshes
    mutable std::mutex mutex_;              ///< Thread safety mutex
};

} // namespace primal::graphics
