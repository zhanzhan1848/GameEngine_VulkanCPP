#pragma once

#include "CommonHeaders.h"
#include "SurfaceCacheTypes.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"  // rhi::RHICardBakeData (T4.2)

namespace primal::graphics::lumen {

struct MeshInstanceInfo {
    math::v3  aabb_min;
    math::v3  aabb_max;
    uint32_t  instance_id;
    float     screen_space_area;

    // T4.2: offline card bake (world-transformed at registration). Non-zero
    // count → RebuildCardAllocation emits these instead of the 6-direction
    // AABB derivation; empty → legacy fallback for this instance.
    uint32_t  baked_card_start = 0;   // into baked_cards_pool_
    uint32_t  baked_card_count = 0;
};

class CardGenerator {
public:
    CardGenerator() = default;

    void Initialize(rhi::RHIDeviceBase* device, uint32_t atlas_size, uint32_t page_size, uint32_t max_cards);
    void RegisterMesh(const math::v3& aabb_min, const math::v3& aabb_max, uint32_t instance_id);
    // T4.2: register an instance whose mesh carries offline card bake data.
    // Baked cards are mesh-local; `world` places them into the same space the
    // fallback AABBs use. The world AABB of the baked cards becomes this
    // entry's lookup AABB (tighter than the sphere-derived fallback cube).
    void RegisterMeshBaked(const rhi::RHICardBakeData& bake, const math::m4x4& world, uint32_t instance_id);
    // T4.2: drop all registrations before a snapshot Rebind re-registers the
    // scene — the registration loop runs on every full rebuild and would
    // otherwise append duplicate instances.
    void ResetRegistrations();
    void RebuildCardAllocation();

    // ============================================================================
    // [WRC 计划 Stream D2/D3/E2]
    //   D2（PRIMAL_SC_PAGE_TABLE=1）：页表间接化——每卡记录物理 page run
    //     （page 粒度坐标），随卡片缓冲一起上传；无淘汰（v1）。
    //   D3：物理页池 + LRU 挤页——页表模式下 RebuildInstance 的重发卡优先
    //     复用自由页段（free-run first-fit）；自由段不足时按 LRU
    //     （card_last_touch_，TouchCard 由 LightEval/capture 两侧刷新）挤掉
    //     最久未服务的卡片页段，替代 capacity_full 静默丢弃。
    //   E2：RebuildInstance——单实例增量重建（槽位重排），配合管线侧
    //     事件驱动变换脏路径（替代 30 帧冷却 + 全量 Rebind）。
    // ============================================================================

    /// D2: 开启页表模式（PRIMAL_SC_PAGE_TABLE=1 时由 SurfaceCachePass 调用）。
    void EnablePageTable() { page_table_enabled_ = true; }
    /// D2: 页表（每卡一个 u32 = 页坐标 pack：x_page<<16 | y_page）。
    const std::vector<u32>& GetPageTable() const { return page_table_; }
    rhi::ResourceHandle GetPageTableBuffer() const { return page_table_buffer_; }
    /// D3: 卡片服务新近度（LightEval Top-K / card-view capture 两侧刷新）。
    void TouchCard(u32 card_idx, u32 frame) {
        if (card_idx < card_last_touch_.size()) card_last_touch_[card_idx] = frame;
    }
    /// E2: 单实例增量重建——更新注册 AABB 后重发该实例的卡片。页表关闭时
    /// 等价于标记全量重分配（allocation_dirty_）。
    bool RebuildInstance(uint32_t instance_id,
                         const math::v3& aabb_min, const math::v3& aabb_max);

    rhi::ResourceHandle GetCardDataBuffer() const { return card_data_buffer_; }
    rhi::ResourceHandle GetCardLookupBuffer() const { return card_lookup_buffer_; }
    rhi::ResourceHandle GetGridOffsetsBuffer() const { return grid_offsets_buffer_; }
    rhi::ResourceHandle GetGridCardsBuffer() const { return grid_cards_buffer_; }
    uint32_t GetCardCount() const { return card_count_; }
    uint32_t GetLookupCount() const { return static_cast<uint32_t>(meshes_.size()); }
    // X2/A3: 配置容量（== LumenConfig::surface_cache_max_cards）。旧 SPGI 注入点
    // 上传的 cardCount 是该容量值；SurfaceCacheView 同时携带活跃值与容量。
    uint32_t GetMaxCardCount() const { return max_cards_; }
    const std::vector<SurfaceCacheCard>& GetCards() const { return cards_; }
    const std::vector<SurfaceCacheCardLookup>& GetLookups() const { return lookups_; }
    // T4.2: card-source observability for the one-shot registration log.
    uint32_t GetBakedMeshCount() const { return baked_mesh_count_; }
    uint32_t GetFallbackMeshCount() const { return fallback_mesh_count_; }

private:
    void UploadToGPU();
    void BuildCardGrid();

    rhi::RHIDeviceBase* device_ = nullptr;
    uint32_t atlas_size_ = 0;
    uint32_t page_size_ = 0;
    uint32_t max_cards_ = 0;

    std::vector<MeshInstanceInfo> meshes_;
    std::vector<SurfaceCacheCard> baked_cards_pool_;
    std::vector<SurfaceCacheCard> cards_;
    std::vector<SurfaceCacheCardLookup> lookups_;
    uint32_t card_count_ = 0;
    uint32_t baked_mesh_count_ = 0;
    uint32_t fallback_mesh_count_ = 0;
    bool allocation_dirty_ = true;

    // [WRC 计划 Stream D2/D3] 页表模式状态。
    bool page_table_enabled_ = false;
    std::vector<u32> page_table_;        // card slot → 页坐标 pack
    std::vector<u32> card_last_touch_;   // card slot → 最后服务帧（LRU）
    struct FreeRun { uint32_t x, y, res; };
    std::vector<FreeRun> free_runs_;     // D3: 自由页段（重发优先复用）
    rhi::ResourceHandle page_table_buffer_ = rhi::handles::INVALID_RESOURCE;

    rhi::ResourceHandle card_data_buffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::ResourceHandle card_lookup_buffer_ = rhi::handles::INVALID_RESOURCE;

    // Uniform card grid (T1.3): offsets buffer carries the 64B header +
    // per-cell prefix sums; cards buffer holds concatenated card indices.
    rhi::ResourceHandle grid_offsets_buffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::ResourceHandle grid_cards_buffer_ = rhi::handles::INVALID_RESOURCE;
};

} // namespace
