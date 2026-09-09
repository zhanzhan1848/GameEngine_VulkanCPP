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

    rhi::ResourceHandle card_data_buffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::ResourceHandle card_lookup_buffer_ = rhi::handles::INVALID_RESOURCE;

    // Uniform card grid (T1.3): offsets buffer carries the 64B header +
    // per-cell prefix sums; cards buffer holds concatenated card indices.
    rhi::ResourceHandle grid_offsets_buffer_ = rhi::handles::INVALID_RESOURCE;
    rhi::ResourceHandle grid_cards_buffer_ = rhi::handles::INVALID_RESOURCE;
};

} // namespace
